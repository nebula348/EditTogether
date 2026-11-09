#include "Tls.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace et::net {

namespace {

constexpr char const* kFallbackRoots =
#include "CaBundle.inc"
    ;

// Loaded once: the system store, then the bundled roots in case the store can't be read.
mbedtls_x509_crt& trustAnchors() {
    static mbedtls_x509_crt chain;
    static std::once_flag once;
    std::call_once(once, [] {
        mbedtls_x509_crt_init(&chain);
#ifdef __ANDROID__
        mbedtls_x509_crt_parse_path(&chain, "/apex/com.android.conscrypt/cacerts");
        mbedtls_x509_crt_parse_path(&chain, "/system/etc/security/cacerts");
#else
        mbedtls_x509_crt_parse_file(&chain, "/etc/ssl/certs/ca-certificates.crt");
#endif
        std::string pem(kFallbackRoots);
        mbedtls_x509_crt_parse(&chain, reinterpret_cast<unsigned char const*>(pem.c_str()), pem.size() + 1);
    });
    return chain;
}

double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string tlsError(int code) {
    char buf[160];
    mbedtls_strerror(code, buf, sizeof(buf));
    return buf;
}

} // namespace

struct Stream::Impl {
    int sock = -1;
    bool tls = false;
    bool psaReady = false;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool tlsInit = false;
    std::string out; // plain bytes waiting to be written
    std::string error;
    bool dead = false;
    size_t retryLen = 0; // mbedTLS wants a retried write to use exactly the same length

    static int bioSend(void* ctx, unsigned char const* buf, size_t len) {
        int fd = *static_cast<int*>(ctx);
        ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
        if (n >= 0) return static_cast<int>(n);
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    static int bioRecv(void* ctx, unsigned char* buf, size_t len) {
        int fd = *static_cast<int*>(ctx);
        ssize_t n = ::recv(fd, buf, len, 0);
        if (n > 0) return static_cast<int>(n);
        if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }

    void fail(std::string why) {
        if (!dead) error = std::move(why);
        dead = true;
    }

    void freeTls() {
        if (!tlsInit) return;
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        tlsInit = false;
    }
};

Stream::Stream() : m(std::make_unique<Impl>()) {}
Stream::~Stream() { close(); }

void Stream::close() {
    if (m->tls && m->tlsInit && m->sock >= 0 && !m->dead) mbedtls_ssl_close_notify(&m->ssl);
    m->freeTls();
    if (m->sock >= 0) ::close(m->sock);
    m->sock = -1;
    m->out.clear();
    m->dead = true;
}

bool Stream::open() const { return m->sock >= 0 && !m->dead; }
int Stream::fd() const { return m->sock; }
std::string const& Stream::error() const { return m->error; }
bool Stream::hasBuffered() const { return m->tls && m->tlsInit && mbedtls_ssl_get_bytes_avail(&m->ssl) > 0; }
bool Stream::wantsWrite() const { return !m->out.empty(); }

int waitFds(int fd, int wakeFd, int timeoutMs, bool wantWrite) {
    pollfd p[2];
    p[0] = {fd, static_cast<short>(POLLIN | (wantWrite ? POLLOUT : 0)), 0};
    p[1] = {wakeFd, POLLIN, 0};
    int n = ::poll(p, wakeFd >= 0 ? 2 : 1, timeoutMs);
    if (n <= 0) return 0;
    int r = 0;
    if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) r |= 1;
    if (wakeFd >= 0 && (p[1].revents & POLLIN)) r |= 2;
    if (p[0].revents & POLLOUT) r |= 4;
    return r;
}

bool Stream::connect(std::string const& host, uint16_t port, bool tls, double timeoutSec, std::string& error) {
    close();
    m->dead = false;
    m->error.clear();
    m->retryLen = 0;
    double deadline = nowSec() + timeoutSec;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int gai = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (gai != 0 || !res) {
        error = "Can't find " + host + " (" + gai_strerror(gai) + ")";
        return false;
    }

    std::string lastError = "no address";
    for (addrinfo* a = res; a && m->sock < 0; a = a->ai_next) {
        int s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) continue;
        ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK);
        int one = 1;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        int rc = ::connect(s, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{s, POLLOUT, 0};
            int left = static_cast<int>(std::max(0.0, deadline - nowSec()) * 1000);
            if (::poll(&p, 1, left) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
                rc = err == 0 ? 0 : -1;
                if (err) lastError = strerror(err);
            } else {
                rc = -1;
                lastError = "timed out";
            }
        } else if (rc != 0) {
            lastError = strerror(errno);
        }
        if (rc == 0) m->sock = s;
        else ::close(s);
    }
    ::freeaddrinfo(res);
    if (m->sock < 0) {
        error = "Can't connect to " + host + " (" + lastError + ")";
        return false;
    }

    m->tls = tls;
    if (!tls) return true;

    if (!m->psaReady) {
        psa_crypto_init();
        m->psaReady = true;
    }
    mbedtls_ssl_init(&m->ssl);
    mbedtls_ssl_config_init(&m->conf);
    mbedtls_entropy_init(&m->entropy);
    mbedtls_ctr_drbg_init(&m->drbg);
    m->tlsInit = true;
    int rc = mbedtls_ctr_drbg_seed(&m->drbg, mbedtls_entropy_func, &m->entropy, nullptr, 0);
    if (rc == 0) rc = mbedtls_ssl_config_defaults(&m->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        error = "TLS setup failed: " + tlsError(rc);
        close();
        return false;
    }
    mbedtls_ssl_conf_authmode(&m->conf, MBEDTLS_SSL_VERIFY_REQUIRED); // certificates are always verified
    mbedtls_ssl_conf_ca_chain(&m->conf, &trustAnchors(), nullptr);
    mbedtls_ssl_conf_rng(&m->conf, mbedtls_ctr_drbg_random, &m->drbg);
    mbedtls_ssl_conf_min_tls_version(&m->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    if ((rc = mbedtls_ssl_setup(&m->ssl, &m->conf)) != 0 || (rc = mbedtls_ssl_set_hostname(&m->ssl, host.c_str())) != 0) {
        error = "TLS setup failed: " + tlsError(rc);
        close();
        return false;
    }
    mbedtls_ssl_set_bio(&m->ssl, &m->sock, Impl::bioSend, Impl::bioRecv, nullptr);

    for (;;) {
        rc = mbedtls_ssl_handshake(&m->ssl);
        if (rc == 0) return true;
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) break;
        int left = static_cast<int>((deadline - nowSec()) * 1000);
        if (left <= 0) {
            error = "TLS handshake with " + host + " timed out";
            close();
            return false;
        }
        pollfd p{m->sock, static_cast<short>(rc == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT), 0};
        ::poll(&p, 1, left);
    }
    error = "TLS handshake with " + host + " failed: " + tlsError(rc);
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        char info[200];
        mbedtls_x509_crt_verify_info(info, sizeof(info), "", mbedtls_ssl_get_verify_result(&m->ssl));
        error += std::string(" - ") + info;
    }
    close();
    return false;
}

int Stream::read(uint8_t* buf, size_t n) {
    if (!open()) return -1;
    if (!m->tls) {
        ssize_t r = ::recv(m->sock, buf, n, 0);
        if (r > 0) return static_cast<int>(r);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        m->fail(r == 0 ? "Connection closed" : strerror(errno));
        return -1;
    }
    int r = mbedtls_ssl_read(&m->ssl, buf, n);
    if (r > 0) return r;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE ||
        r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET || r == MBEDTLS_ERR_SSL_RECEIVED_EARLY_DATA)
        return 0;
    m->fail(r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF ? "Connection closed" : tlsError(r));
    return -1;
}

bool Stream::flush() {
    if (!open()) return false;
    while (!m->out.empty()) {
        int w;
        if (m->tls) {
            size_t len = m->retryLen ? m->retryLen : std::min<size_t>(m->out.size(), 16384);
            w = mbedtls_ssl_write(&m->ssl, reinterpret_cast<unsigned char const*>(m->out.data()), len);
            if (w == MBEDTLS_ERR_SSL_WANT_WRITE || w == MBEDTLS_ERR_SSL_WANT_READ) { m->retryLen = len; return true; }
            if (w < 0) { m->fail(tlsError(w)); return false; }
            m->retryLen = 0;
        } else {
            ssize_t r = ::send(m->sock, m->out.data(), m->out.size(), MSG_NOSIGNAL);
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return true;
            if (r < 0) { m->fail(strerror(errno)); return false; }
            w = static_cast<int>(r);
        }
        m->out.erase(0, static_cast<size_t>(w));
    }
    return true;
}

bool Stream::write(char const* data, size_t n) {
    if (!open()) return false;
    m->out.append(data, n);
    if (m->out.size() > (8u << 20)) { m->fail("Send buffer overflow"); return false; }
    return flush();
}

} // namespace et::net
