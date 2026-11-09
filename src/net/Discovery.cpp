#include "Discovery.hpp"

#include <set>

namespace et::net {

void RoomAnnouncer::update(double now, Announce const& a) {
    if (!m_sock || now - m_last < 1.0) return;
    m_last = now;
    auto data = encodeAnnounce(a);
    // Android drops some limited (255.255.255.255) broadcasts on certain drivers; subnet broadcast covers those.
    std::set<uint32_t> targets = {0xFFFFFFFFu};
    for (auto const& itf : localInterfaces())
        if (itf.broadcast) targets.insert(itf.broadcast);
    for (auto t : targets)
        if (m_sock.sendTo({t, kDiscoveryPort}, data)) ++m_sent;
}

void RoomBrowser::update(double now) {
    if (m_sock) {
        std::string data;
        Endpoint from;
        for (int i = 0; i < 64 && m_sock.recvFrom(data, from); ++i) {
            auto a = decodeAnnounce(data);
            if (!a) { ++m_invalid; continue; }
            ++m_received;
            m_rooms[a->roomId] = {*a, {from.ip, a->tcpPort}, now};
        }
    }
    std::erase_if(m_rooms, [&](auto const& kv) { return now - kv.second.lastSeen > kExpiry; });
}

} // namespace et::net
