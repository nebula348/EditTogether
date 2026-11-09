#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace et::net {

// TCP client stream, optionally over TLS (mbedTLS). Network thread only: connect() blocks, the rest doesn't.
class Stream {
public:
    Stream();
    ~Stream();
    Stream(Stream const&) = delete;
    Stream& operator=(Stream const&) = delete;

    bool connect(std::string const& host, uint16_t port, bool tls, double timeoutSec, std::string& error);
    void close();
    bool open() const;
    int fd() const;
    bool hasBuffered() const; // decrypted bytes already waiting

    // >0 bytes read, 0 = nothing right now, -1 = closed or failed (see error()).
    int read(uint8_t* buf, size_t n);
    // Queues data and flushes what the socket accepts. false = failed.
    bool write(char const* data, size_t n);
    bool flush(); // push queued bytes; false = failed
    bool wantsWrite() const;
    std::string const& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Waits for fd (or wakeFd) for up to timeoutMs. Result bits: 1 = fd readable, 2 = wakeFd readable, 4 = fd writable.
int waitFds(int fd, int wakeFd, int timeoutMs, bool wantWrite = false);

} // namespace et::net
