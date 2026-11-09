#pragma once

#include "Link.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace et::net {

// RAII file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : m_fd(fd) {}
    ~Fd() { reset(); }
    Fd(Fd&& o) noexcept : m_fd(o.release()) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) { reset(); m_fd = o.release(); }
        return *this;
    }
    Fd(Fd const&) = delete;
    Fd& operator=(Fd const&) = delete;
    int get() const { return m_fd; }
    explicit operator bool() const { return m_fd >= 0; }
    int release() { int f = m_fd; m_fd = -1; return f; }
    void reset();

private:
    int m_fd = -1;
};

struct Endpoint {
    uint32_t ip = 0; // host byte order
    uint16_t port = 0;
    std::string str() const;
};
std::optional<uint32_t> parseIPv4(std::string_view s);
std::string ipToString(uint32_t ip);

struct Interface {
    std::string name;
    uint32_t ip = 0;
    uint32_t broadcast = 0;
};
// Up, non-loopback IPv4 interfaces.
std::vector<Interface> localInterfaces();

// Non-blocking TCP connection with length-prefixed framing: [u32 LE length][payload].
class TcpConn final : public Link {
public:
    static std::unique_ptr<TcpConn> connect(Endpoint const& to, std::string& error);
    explicit TcpConn(Fd fd, Endpoint peer);

    // Performs pending I/O. Complete frames are appended to `frames`.
    // Returns false once the connection is closed or failed (see error()).
    bool update(std::vector<std::string>& frames) override;
    void send(std::string_view payload) override;
    void setMaxFrame(uint32_t n) override { m_maxFrame = n; }
    bool connected() const { return !m_connecting; }
    bool outEmpty() const override { return m_outOff >= m_out.size(); }
    std::string const& error() const override { return m_error; }
    std::string describe() const override { return m_peer.str(); }
    Endpoint const& peer() const { return m_peer; }
    uint64_t bytesIn() const override { return m_bytesIn; }
    uint64_t bytesOut() const override { return m_bytesOut; }

private:
    bool fail(std::string why);
    bool flush();

    Fd m_fd;
    Endpoint m_peer;
    bool m_connecting = false;
    bool m_dead = false;
    std::string m_in;
    std::string m_out;
    size_t m_outOff = 0;
    uint32_t m_maxFrame = 4096;
    std::string m_error;
    uint64_t m_bytesIn = 0, m_bytesOut = 0;
};

class TcpListener {
public:
    // Tries `port`, then any free port.
    bool open(uint16_t port, std::string& error);
    std::unique_ptr<TcpConn> accept();
    uint16_t port() const { return m_port; }

private:
    Fd m_fd;
    uint16_t m_port = 0;
};

class UdpSocket {
public:
    // port 0 = ephemeral. reuse allows several listeners on the discovery port.
    bool open(uint16_t port, bool reuse, std::string& error);
    bool sendTo(Endpoint const& to, std::string_view data);
    // Returns false when no datagram is waiting.
    bool recvFrom(std::string& data, Endpoint& from);
    explicit operator bool() const { return bool(m_fd); }

private:
    Fd m_fd;
};

} // namespace et::net
