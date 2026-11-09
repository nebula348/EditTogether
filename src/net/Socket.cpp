#include "Socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace et::net {

namespace {

constexpr size_t kMaxOutBuffer = 96 * 1024 * 1024; // drop peers that stop reading
constexpr size_t kMaxReadPerUpdate = 8 * 1024 * 1024; // don't stall a frame on a big snapshot

bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

sockaddr_in toSockaddr(Endpoint const& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(e.ip);
    a.sin_port = htons(e.port);
    return a;
}

std::string errnoStr(char const* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

} // namespace

void Fd::reset() {
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
}

std::string ipToString(uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." +
           std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}

std::string Endpoint::str() const {
    return ipToString(ip) + ":" + std::to_string(port);
}

std::optional<uint32_t> parseIPv4(std::string_view s) {
    in_addr a{};
    std::string z(s);
    if (inet_pton(AF_INET, z.c_str(), &a) != 1) return std::nullopt;
    return ntohl(a.s_addr);
}

std::vector<Interface> localInterfaces() {
    // getifaddrs is API 24+ while the build targets 23, so resolve it at runtime.
    using GetFn = int (*)(ifaddrs**);
    using FreeFn = void (*)(ifaddrs*);
    static auto get = reinterpret_cast<GetFn>(dlsym(RTLD_DEFAULT, "getifaddrs"));
    static auto release = reinterpret_cast<FreeFn>(dlsym(RTLD_DEFAULT, "freeifaddrs"));
    std::vector<Interface> out;
    ifaddrs* list = nullptr;
    if (!get || !release || get(&list) != 0) return out;
    for (auto* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        Interface itf;
        itf.name = i->ifa_name ? i->ifa_name : "";
        itf.ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr)
            itf.broadcast = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr);
        else if (i->ifa_netmask)
            itf.broadcast = itf.ip | ~ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
        out.push_back(itf);
    }
    release(list);
    return out;
}

// ------------------------------------------------------------------ TcpConn

std::unique_ptr<TcpConn> TcpConn::connect(Endpoint const& to, std::string& error) {
    Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd) { error = errnoStr("socket"); return nullptr; }
    if (!setNonBlocking(fd.get())) { error = errnoStr("fcntl"); return nullptr; }
    int one = 1;
    setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    auto addr = toSockaddr(to);
    int r = ::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    if (r != 0 && errno != EINPROGRESS) { error = errnoStr("connect"); return nullptr; }
    auto c = std::make_unique<TcpConn>(std::move(fd), to);
    c->m_connecting = r != 0;
    return c;
}

TcpConn::TcpConn(Fd fd, Endpoint peer) : m_fd(std::move(fd)), m_peer(peer) {
    setNonBlocking(m_fd.get());
    int one = 1;
    setsockopt(m_fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

bool TcpConn::fail(std::string why) {
    if (!m_dead) m_error = std::move(why);
    m_dead = true;
    m_fd.reset();
    return false;
}

void TcpConn::send(std::string_view payload) {
    if (m_dead) return;
    if (m_out.size() - m_outOff + payload.size() > kMaxOutBuffer) {
        fail("Peer is not reading data fast enough");
        return;
    }
    if (m_outOff > 0 && m_outOff == m_out.size()) {
        m_out.clear();
        m_outOff = 0;
    }
    uint32_t n = static_cast<uint32_t>(payload.size());
    char len[4] = {char(n & 255), char((n >> 8) & 255), char((n >> 16) & 255), char((n >> 24) & 255)};
    m_out.append(len, 4);
    m_out.append(payload);
}

bool TcpConn::flush() {
    while (m_outOff < m_out.size()) {
        ssize_t w = ::send(m_fd.get(), m_out.data() + m_outOff, m_out.size() - m_outOff, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return fail(errnoStr("send"));
        }
        m_outOff += static_cast<size_t>(w);
        m_bytesOut += static_cast<uint64_t>(w);
    }
    m_out.clear();
    m_outOff = 0;
    return true;
}

bool TcpConn::update(std::vector<std::string>& frames) {
    if (m_dead) return false;
    if (m_connecting) {
        pollfd p{m_fd.get(), POLLOUT, 0};
        if (::poll(&p, 1, 0) <= 0) return true;
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(m_fd.get(), SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            errno = err;
            return fail(errnoStr("connect"));
        }
        m_connecting = false;
    }

    char buf[64 * 1024];
    size_t readNow = 0;
    std::string readError; // deferred so frames sent right before a close (Kicked, RoomClosed) still arrive
    while (readNow < kMaxReadPerUpdate) {
        ssize_t r = ::recv(m_fd.get(), buf, sizeof buf, 0);
        if (r == 0) { readError = "Connection closed by peer"; break; }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            readError = errnoStr("recv");
            break;
        }
        m_in.append(buf, static_cast<size_t>(r));
        readNow += static_cast<size_t>(r);
        m_bytesIn += static_cast<uint64_t>(r);
    }

    size_t pos = 0;
    while (m_in.size() - pos >= 4) {
        auto b = reinterpret_cast<unsigned char const*>(m_in.data() + pos);
        uint32_t n = b[0] | (b[1] << 8) | (b[2] << 16) | (uint32_t(b[3]) << 24);
        if (n > m_maxFrame) return fail("Packet exceeds size limit");
        if (m_in.size() - pos - 4 < n) break;
        frames.emplace_back(m_in, pos + 4, n);
        pos += 4 + n;
    }
    m_in.erase(0, pos);

    if (!readError.empty()) return fail(readError);
    return flush();
}

// ------------------------------------------------------------------ TcpListener

bool TcpListener::open(uint16_t port, std::string& error) {
    for (uint16_t p : {port, uint16_t(0)}) {
        Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
        if (!fd) { error = errnoStr("socket"); return false; }
        int one = 1;
        setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        auto addr = toSockaddr({INADDR_ANY, p});
        if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            error = errnoStr("bind");
            continue;
        }
        if (::listen(fd.get(), 8) != 0 || !setNonBlocking(fd.get())) {
            error = errnoStr("listen");
            continue;
        }
        socklen_t len = sizeof addr;
        getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        m_fd = std::move(fd);
        return true;
    }
    return false;
}

std::unique_ptr<TcpConn> TcpListener::accept() {
    sockaddr_in a{};
    socklen_t len = sizeof a;
    int c = ::accept(m_fd.get(), reinterpret_cast<sockaddr*>(&a), &len);
    if (c < 0) return nullptr;
    return std::make_unique<TcpConn>(Fd(c), Endpoint{ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)});
}

// ------------------------------------------------------------------ UdpSocket

bool UdpSocket::open(uint16_t port, bool reuse, std::string& error) {
    Fd fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd) { error = errnoStr("socket"); return false; }
    int one = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    if (reuse) {
        setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    }
    auto addr = toSockaddr({INADDR_ANY, port});
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) { error = errnoStr("bind"); return false; }
    if (!setNonBlocking(fd.get())) { error = errnoStr("fcntl"); return false; }
    m_fd = std::move(fd);
    return true;
}

bool UdpSocket::sendTo(Endpoint const& to, std::string_view data) {
    auto addr = toSockaddr(to);
    return ::sendto(m_fd.get(), data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof addr) >= 0;
}

bool UdpSocket::recvFrom(std::string& data, Endpoint& from) {
    char buf[2048];
    sockaddr_in a{};
    socklen_t len = sizeof a;
    ssize_t r = ::recvfrom(m_fd.get(), buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&a), &len);
    if (r < 0) return false;
    data.assign(buf, static_cast<size_t>(r));
    from = {ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
    return true;
}

} // namespace et::net
