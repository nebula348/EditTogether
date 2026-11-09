#pragma once

#include "Socket.hpp"
#include "core/Protocol.hpp"

#include <map>

namespace et::net {

// Host side: broadcasts the room every second on every IPv4 interface + 255.255.255.255.
class RoomAnnouncer {
public:
    bool start(std::string& error) { return m_sock.open(0, false, error); }
    void update(double now, Announce const& a);
    uint64_t sent() const { return m_sent; }

private:
    UdpSocket m_sock;
    double m_last = -10;
    uint64_t m_sent = 0;
};

struct FoundRoom {
    Announce info;
    Endpoint host; // TCP endpoint
    double lastSeen = 0;
};

// Client side: listens for announcements. Rooms disappear after kExpiry seconds of silence.
class RoomBrowser {
public:
    static constexpr double kExpiry = 4.0;

    bool start(std::string& error) { return m_sock.open(kDiscoveryPort, true, error); }
    void update(double now);
    void clear() { m_rooms.clear(); }
    std::map<uint64_t, FoundRoom> const& rooms() const { return m_rooms; }
    uint64_t received() const { return m_received; }
    uint64_t invalid() const { return m_invalid; }

private:
    UdpSocket m_sock;
    std::map<uint64_t, FoundRoom> m_rooms;
    uint64_t m_received = 0, m_invalid = 0;
};

} // namespace et::net
