#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace et::net {

// Reliable, ordered connection to one peer: TCP on the LAN, a message relay online.
class Link {
public:
    virtual ~Link() = default;
    // Performs pending I/O; complete frames are appended. Returns false once closed/failed (see error()).
    virtual bool update(std::vector<std::string>& frames) = 0;
    virtual void send(std::string_view payload) = 0;
    virtual void setMaxFrame(uint32_t n) = 0;
    virtual bool outEmpty() const = 0;
    virtual std::string const& error() const = 0;
    virtual std::string describe() const = 0;
    virtual uint64_t bytesIn() const = 0;
    virtual uint64_t bytesOut() const = 0;
    virtual bool viaRelay() const { return false; }
};

} // namespace et::net
