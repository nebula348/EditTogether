#pragma once

#include "ChunkLink.hpp"
#include "Relay.hpp"

#include <functional>
#include <memory>
#include <string>

namespace et::net {

// Rooms over a Relay. A room with code CODE uses:
//   private-et-CODE-l        lobby, where joiners announce their public id
//   private-et-CODE-p<jid>   one private channel per player (host + that player only)
// jid is the first 16 bytes of SHA-256(jsecret); the auth service only signs a player channel for whoever knows
// jsecret, or for the host. Each channel carries a ChunkLink, so the cores see the same Link as on the LAN.
std::string lobbyChannel(std::string const& code);
std::string peerChannel(std::string const& code, std::string const& jid);
std::string jidOf(std::string const& jsecretHex);
std::string newSecret(); // 16 random bytes as hex
bool validCode(std::string const& code);
std::string normalizeCode(std::string text); // upper-case, drops spaces and dashes

using ClockFn = std::function<double()>;

// Host side: finds joiners through the lobby and hands out one Link per connection.
class RelayHost {
public:
    static constexpr size_t kMaxPeerChannels = 128;
    static constexpr double kIdleChannelSeconds = 120.0;

    RelayHost(Relay& relay, std::string code, std::string hostToken, ClockFn clock);
    ~RelayHost();
    RelayHost(RelayHost const&) = delete;

    void pump(); // call every tick: routes relay events to links
    // A new connection whose first data arrived; nullptr when none is waiting.
    std::unique_ptr<Link> accept();
    void setAccepting(bool on);
    bool lobbyReady() const;
    size_t channelCount() const;
    std::string const& code() const;

    struct Impl; // internal

private:
    std::shared_ptr<Impl> m;
};

// Player side: a Link to the host of room `code`. Polls the relay itself, so give it its own Relay.
// Fails with a readable error when the relay is lost, the host does not answer or refuses the channel.
std::unique_ptr<Link> connectToRoom(Relay& relay, std::string const& code, std::string const& jsecretHex, ClockFn clock);

} // namespace et::net
