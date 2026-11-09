#include "WebSocket.hpp"

#include <cctype>
#include <utility>

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

namespace et::net::ws {

std::string base64(std::string_view bytes) {
    size_t cap = 4 * ((bytes.size() + 2) / 3) + 1, len = 0;
    std::string out(cap, '\0');
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), cap, &len,
                          reinterpret_cast<unsigned char const*>(bytes.data()), bytes.size());
    out.resize(len);
    return out;
}

std::string base64Decode(std::string_view text, bool& ok) {
    size_t cap = text.size() / 4 * 3 + 3, len = 0;
    std::string out(cap, '\0');
    int rc = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(out.data()), cap, &len,
                                   reinterpret_cast<unsigned char const*>(text.data()), text.size());
    ok = rc == 0;
    out.resize(ok ? len : 0);
    return out;
}

std::string encodeFrame(Opcode op, std::string_view payload, uint32_t maskKey) {
    std::string f;
    f.reserve(payload.size() + 14);
    f.push_back(static_cast<char>(0x80 | op)); // FIN, no fragmentation
    size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(0x80 | n));
    } else if (n <= 0xFFFF) {
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(static_cast<char>(n >> 8));
        f.push_back(static_cast<char>(n & 255));
    } else {
        f.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 255));
    }
    char key[4];
    for (int i = 0; i < 4; ++i) key[i] = static_cast<char>((maskKey >> (8 * i)) & 255);
    f.append(key, 4);
    for (size_t i = 0; i < n; ++i) f.push_back(static_cast<char>(payload[i] ^ key[i & 3]));
    return f;
}

std::string handshakeRequest(std::string const& host, std::string const& path, std::string const& key) {
    return "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
           "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: EditTogether\r\n\r\n";
}

std::string acceptFor(std::string const& key) {
    std::string in = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[20];
    mbedtls_sha1(reinterpret_cast<unsigned char const*>(in.data()), in.size(), digest);
    return base64(std::string_view(reinterpret_cast<char const*>(digest), 20));
}

namespace {
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
} // namespace

std::optional<size_t> checkHandshakeResponse(std::string_view buf, std::string const& key, std::string& error) {
    size_t end = buf.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        if (buf.size() > 16384) { error = "Handshake reply too long"; return std::nullopt; }
        return size_t(0);
    }
    std::string_view head = buf.substr(0, end);
    size_t lineEnd = head.find("\r\n");
    std::string_view status = head.substr(0, lineEnd);
    if (status.size() < 12 || status.substr(0, 9) != "HTTP/1.1 " || status.substr(9, 3) != "101") {
        error = "Server refused the WebSocket: " + std::string(status.substr(0, 80));
        return std::nullopt;
    }
    std::string accept;
    size_t pos = lineEnd;
    while (pos != std::string_view::npos && pos < head.size()) {
        pos += 2;
        size_t next = head.find("\r\n", pos);
        std::string_view line = head.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
        size_t colon = line.find(':');
        if (colon != std::string_view::npos && iequals(line.substr(0, colon), "sec-websocket-accept")) {
            std::string_view v = line.substr(colon + 1);
            while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
            accept = std::string(v);
        }
        pos = next;
    }
    if (accept != acceptFor(key)) { error = "Bad WebSocket accept key"; return std::nullopt; }
    return end + 4;
}

std::optional<Message> Reader::next() {
    for (;;) {
        if (!m_error.empty() || m_buf.size() < 2) return std::nullopt;
        auto b = [&](size_t i) { return static_cast<uint8_t>(m_buf[i]); };
        bool fin = b(0) & 0x80;
        uint8_t op = b(0) & 0x0F;
        if (b(0) & 0x70) { m_error = "Unsupported WebSocket extension"; return std::nullopt; }
        if (b(1) & 0x80) { m_error = "Server frame was masked"; return std::nullopt; }
        uint64_t len = b(1) & 0x7F;
        size_t head = 2;
        if (len == 126) {
            if (m_buf.size() < 4) return std::nullopt;
            len = (static_cast<uint64_t>(b(2)) << 8) | b(3);
            head = 4;
        } else if (len == 127) {
            if (m_buf.size() < 10) return std::nullopt;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | b(2 + i);
            head = 10;
        }
        if (len > kMaxMessage) { m_error = "WebSocket message too large"; return std::nullopt; }
        if (m_buf.size() < head + len) return std::nullopt;
        std::string payload = m_buf.substr(head, static_cast<size_t>(len));
        m_buf.erase(0, head + static_cast<size_t>(len));

        if (op >= 8) { // control frames may be interleaved with a fragmented message
            if (!fin || len > 125) { m_error = "Bad control frame"; return std::nullopt; }
            if (op != Close && op != Ping && op != Pong) { m_error = "Unknown control frame"; return std::nullopt; }
            return Message{static_cast<Opcode>(op), std::move(payload)};
        }
        if (op == Text || op == Binary) {
            if (m_assembling) { m_error = "New message inside a fragmented one"; return std::nullopt; }
            m_messageOp = static_cast<Opcode>(op);
            m_message = std::move(payload);
            m_assembling = !fin;
        } else if (op == Continuation) {
            if (!m_assembling) { m_error = "Unexpected continuation frame"; return std::nullopt; }
            m_message += payload;
            if (m_message.size() > kMaxMessage) { m_error = "WebSocket message too large"; return std::nullopt; }
            m_assembling = !fin;
        } else {
            m_error = "Unknown WebSocket opcode";
            return std::nullopt;
        }
        if (!m_assembling) return Message{m_messageOp, std::exchange(m_message, {})};
    }
}

} // namespace et::net::ws
