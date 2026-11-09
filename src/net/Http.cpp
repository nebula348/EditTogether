#include "Http.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>

namespace et::net {

namespace {
double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

constexpr size_t kMaxBody = 1u << 20;
} // namespace

std::optional<Url> parseUrl(std::string const& text) {
    Url u;
    std::string_view s = text;
    if (s.substr(0, 8) == "https://") { u.tls = true; s.remove_prefix(8); }
    else if (s.substr(0, 7) == "http://") { u.tls = false; u.port = 80; s.remove_prefix(7); }
    else return std::nullopt;
    size_t slash = s.find('/');
    std::string_view authority = s.substr(0, slash);
    if (slash != std::string_view::npos) u.base = std::string(s.substr(slash));
    while (!u.base.empty() && u.base.back() == '/') u.base.pop_back();
    size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        int port = std::atoi(std::string(authority.substr(colon + 1)).c_str());
        if (port <= 0 || port > 65535) return std::nullopt;
        u.port = static_cast<uint16_t>(port);
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) return std::nullopt;
    u.host = std::string(authority);
    return u;
}

bool decodeChunked(std::string_view in, std::string& out, size_t* consumed) {
    out.clear();
    size_t p = 0;
    for (;;) {
        size_t eol = in.find("\r\n", p);
        if (eol == std::string_view::npos) return false;
        std::string sizeText(in.substr(p, eol - p));
        size_t semi = sizeText.find(';');
        if (semi != std::string::npos) sizeText.resize(semi);
        if (sizeText.empty() || sizeText.size() > 8) return false;
        char* end = nullptr;
        unsigned long n = std::strtoul(sizeText.c_str(), &end, 16);
        if (*end != '\0') return false;
        p = eol + 2;
        if (n == 0) { // trailers are skipped up to the final blank line
            size_t fin = in.find("\r\n", p);
            while (fin != std::string_view::npos && fin != p) {
                p = fin + 2;
                fin = in.find("\r\n", p);
            }
            if (fin == std::string_view::npos) return false;
            if (consumed) *consumed = fin + 2;
            return true;
        }
        if (n > kMaxBody || out.size() + n > kMaxBody || in.size() < p + n + 2) return false;
        out.append(in.substr(p, n));
        p += n;
        if (in.substr(p, 2) != "\r\n") return false;
        p += 2;
    }
}

HttpResponse HttpClient::attempt(char const* method, std::string const& path, std::string const& body, bool& reused) {
    HttpResponse res;
    double deadline = nowSec() + kTimeout;
    reused = m_stream.open();
    if (!reused) {
        if (!m_stream.connect(m_url.host, m_url.port, m_url.tls, kTimeout, res.error)) return res;
    }
    std::string req = std::string(method) + " " + m_url.base + path + " HTTP/1.1\r\nHost: " + m_url.host +
                      "\r\nUser-Agent: EditTogether\r\nAccept: application/json\r\nConnection: keep-alive\r\n";
    if (body.size() || std::string_view(method) == "POST")
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    if (!m_stream.write(req.data(), req.size())) { res.error = m_stream.error(); m_stream.close(); return res; }

    std::string buf;
    size_t headerEnd = std::string::npos;
    long contentLength = -1;
    bool chunked = false, closeAfter = false;
    char tmp[4096];
    for (;;) {
        // Drain what is readable now.
        for (;;) {
            int n = m_stream.read(reinterpret_cast<uint8_t*>(tmp), sizeof(tmp));
            if (n < 0) {
                // A close after a complete body-less-length reply is fine; anything else is a failure.
                if (headerEnd != std::string::npos && contentLength < 0 && !chunked) { res.body = buf.substr(headerEnd); m_stream.close(); return res; }
                res.status = 0;
                res.error = m_stream.error().empty() ? "Connection closed" : m_stream.error();
                m_stream.close();
                return res;
            }
            if (n == 0) break;
            buf.append(tmp, static_cast<size_t>(n));
            if (buf.size() > kMaxBody + 16384) { res.error = "Reply too large"; m_stream.close(); return res; }
        }
        if (headerEnd == std::string::npos) {
            size_t e = buf.find("\r\n\r\n");
            if (e != std::string::npos) {
                headerEnd = e + 4;
                std::string head = buf.substr(0, e);
                size_t sp = head.find(' ');
                res.status = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
                size_t pos = head.find("\r\n");
                while (pos != std::string::npos) {
                    pos += 2;
                    size_t next = head.find("\r\n", pos);
                    std::string line = head.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string k = lower(line.substr(0, colon));
                        std::string v = lower(line.substr(colon + 1));
                        v.erase(0, v.find_first_not_of(' '));
                        if (k == "content-length") contentLength = std::atol(v.c_str());
                        else if (k == "transfer-encoding" && v.find("chunked") != std::string::npos) chunked = true;
                        else if (k == "connection" && v.find("close") != std::string::npos) closeAfter = true;
                    }
                    pos = next;
                }
                if (res.status < 100 || contentLength > static_cast<long>(kMaxBody)) {
                    res.status = 0;
                    res.error = "Bad HTTP reply";
                    m_stream.close();
                    return res;
                }
            }
        }
        if (headerEnd != std::string::npos) {
            if (chunked) {
                if (decodeChunked(std::string_view(buf).substr(headerEnd), res.body)) {
                    if (closeAfter) m_stream.close();
                    return res;
                }
            } else if (contentLength >= 0 && buf.size() - headerEnd >= static_cast<size_t>(contentLength)) {
                res.body = buf.substr(headerEnd, static_cast<size_t>(contentLength));
                if (closeAfter) m_stream.close();
                return res;
            } else if (contentLength < 0 && res.status != 0 && (res.status == 204 || res.status == 304)) {
                return res;
            }
        }
        int left = static_cast<int>((deadline - nowSec()) * 1000);
        if (left <= 0) {
            res.status = 0;
            res.error = "Timed out waiting for the server";
            m_stream.close();
            return res;
        }
        if (!m_stream.hasBuffered()) waitFds(m_stream.fd(), -1, std::min(left, 500));
    }
}

HttpResponse HttpClient::request(char const* method, std::string const& path, std::string const& body) {
    bool reused = false;
    HttpResponse r = attempt(method, path, body, reused);
    if (r.status == 0 && reused) r = attempt(method, path, body, reused); // the idle connection had gone stale
    return r;
}

} // namespace et::net
