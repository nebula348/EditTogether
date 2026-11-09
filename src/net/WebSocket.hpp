#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace et::net::ws {

enum Opcode : uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

// RFC 6455 client side. Frames we send are always masked.
std::string encodeFrame(Opcode op, std::string_view payload, uint32_t maskKey);

std::string base64(std::string_view bytes);
std::string base64Decode(std::string_view text, bool& ok);

std::string handshakeRequest(std::string const& host, std::string const& path, std::string const& key);
// Expected Sec-WebSocket-Accept for a request key.
std::string acceptFor(std::string const& key);
// Parses the server's reply once the full header (ending in CRLFCRLF) is in `buf`. Returns the header length, 0 if
// incomplete, or nullopt on failure (error set: refused, wrong status, bad accept).
std::optional<size_t> checkHandshakeResponse(std::string_view buf, std::string const& key, std::string& error);

struct Message {
    Opcode op = Text; // Text, Binary, Close, Ping or Pong; fragmented data messages arrive reassembled
    std::string data;
};

// Incremental frame reader for bytes coming from the server (which must not be masked).
class Reader {
public:
    static constexpr size_t kMaxMessage = 1u << 20; // 1 MB, above Apinator's 256 KB limit
    void feed(char const* data, size_t n) { m_buf.append(data, n); }
    // Next complete message; nullopt when more bytes are needed. error() is set on protocol violations.
    std::optional<Message> next();
    std::string const& error() const { return m_error; }

private:
    std::string m_buf;
    std::string m_message; // fragmented message being assembled
    Opcode m_messageOp = Text;
    bool m_assembling = false;
    std::string m_error;
};

} // namespace et::net::ws
