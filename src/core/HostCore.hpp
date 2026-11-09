#pragma once

#include "ObjectStore.hpp"
#include "Protocol.hpp"

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace et {

// Rejection text for a player whose mod speaks another protocol version.
std::string versionMismatch(uint16_t theirs);

// Host-authoritative room logic. Pure state machine: no sockets, no Geode.
// Peer 1 is the host's own editor, connected in-process.
class HostCore {
public:
    static constexpr uint32_t kLocalPeer = 1;
    static constexpr double kPeerTimeout = 15.0;
    static constexpr double kPendingTimeout = 60.0;
    static constexpr double kChecksumInterval = 3.0;
    static constexpr double kRatePerSec = 120.0;
    static constexpr double kRateBurst = 240.0;
    static constexpr uint32_t kMaxPending = 8;
    static constexpr double kPlayFlush = 0.05; // player positions are relayed in batches 20 times per second

    struct Config {
        uint64_t roomId = 0;
        std::string roomName;
        std::string hostName;
        int maxPlayers = 4;
        std::string password; // empty = no password
    };
    struct Out {
        uint32_t peer = 0;
        std::string bytes; // empty = nothing to send
        bool closeAfter = false;
    };
    enum class EventKind { JoinRequest, PeerJoined, PeerLeft, RateLimited };
    struct Event {
        EventKind kind;
        uint32_t peer = 0;
        std::string nickname;
        std::string detail;
    };
    struct Peer {
        uint32_t id = 0;
        std::string nickname;
        Role role = Role::Editor;
        bool active = false;
        bool helloReceived = false;
        uint64_t token = 0;
        uint32_t lastClientSeq = 0;
        double lastSeen = 0;
        double requestedAt = 0;
        double rateTokens = kRateBurst;
        double rateRefill = 0;
        double lastSnapshotAt = -100;
        bool needsResync = false;
    };

    HostCore(Config cfg, ObjectStore seed, double now, uint64_t tokenSeed);

    uint32_t onConnect(double now);
    // Called when the transport closed by itself. Safe to call for unknown peers.
    void onDisconnect(uint32_t peer, std::string_view reason);
    void onMessage(uint32_t peer, std::string_view payload, double now);
    void tick(double now);

    // Administrator actions.
    void accept(uint32_t peer, double now);
    void reject(uint32_t peer, std::string_view reason);
    void kick(uint32_t peer, std::string_view reason);
    void setRole(uint32_t peer, Role role);
    void releaseLocks(uint32_t peer);
    // Seconds between relayed player-position batches (slower over a relay with a message budget).
    void setPlayFlush(double seconds) { m_playFlush = seconds; }
    void close(std::string_view reason);

    std::vector<Out> takeOutbox() { return std::exchange(m_out, {}); }
    std::vector<Event> takeEvents() { return std::exchange(m_events, {}); }

    Config const& config() const { return m_cfg; }
    ObjectStore const& store() const { return m_store; }
    uint64_t seq() const { return m_seq; }
    std::map<uint32_t, Peer> const& peers() const { return m_peers; }
    uint32_t lockOwner(uint64_t id) const;
    int activeCount() const;
    int pendingCount() const;
    bool closed() const { return m_closed; }

private:
    struct KnownToken {
        std::string nickname;
        Role role;
    };

    void send(uint32_t peer, Message const& m, bool closeAfter = false);
    void broadcast(Message const& m);
    void broadcastExcept(uint32_t skip, Message const& m);
    // silent: no PeerLeft event (the same player is reconnecting).
    void removePeer(uint32_t peer, std::string_view reason, bool forgetToken, bool silent = false);
    void protocolError(uint32_t peer, std::string_view what);
    void handleHello(Peer& p, msg::Hello const& h, double now);
    void activate(Peer& p, Role role, double now, bool reconnect);
    void handleOps(Peer& p, msg::Ops const& m);
    void handleLocks(Peer& p, std::vector<uint64_t> const& ids);
    void sendSnapshot(Peer& p, double now);
    void broadcastPeerList();
    std::string validate(Peer const& p, Op const& op) const;
    Op authoritativeFor(Op const& op) const;
    bool takeRateToken(Peer& p, double now);
    uint64_t newToken();

    Config m_cfg;
    ObjectStore m_store;
    uint64_t m_seq = 0;
    std::map<uint32_t, Peer> m_peers;
    uint32_t m_nextPeer = kLocalPeer + 1;
    std::unordered_map<uint64_t, uint32_t> m_lockOwner;
    std::unordered_map<uint32_t, std::vector<uint64_t>> m_locksByPeer;
    std::unordered_map<uint64_t, KnownToken> m_knownTokens;
    std::unordered_set<uint32_t> m_playing;
    std::vector<msg::PlayState> m_playBatch;
    double m_lastPlayFlush = 0;
    double m_playFlush = kPlayFlush;
    std::vector<Out> m_out;
    std::vector<Event> m_events;
    double m_lastTick = 0;
    double m_lastChecksum = 0;
    uint64_t m_rng;
    bool m_closed = false;
};

} // namespace et
