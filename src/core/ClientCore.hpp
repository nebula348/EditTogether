#pragma once

#include "ObjectStore.hpp"
#include "PlayBuffer.hpp"
#include "Protocol.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

namespace et {

// Client side of the protocol: ordering, duplicate detection, checksum verification, locks, roster.
// Pure logic. Produces Actions for the editor bridge and encoded messages for the transport.
class ClientCore {
public:
    static constexpr double kPingInterval = 2.0;
    static constexpr double kHostTimeout = 15.0;

    enum class State { Idle, Pending, Active, Closed };
    enum class ActionKind {
        Pending,       // waiting for administrator approval
        Joined,        // approved; a snapshot follows
        Rejected,      // text = reason
        Kicked,        // text = reason
        RoomClosed,    // text = reason
        ConnectionLost,// text = reason (protocol error or host silent)
        ApplyOps,      // ops in authoritative order; own = they came from this client
        Correction,    // host rejected some of our ops; ops carry authoritative state; text = reason
        LoadSnapshot,  // store was replaced; rebuild the editor from it
        SyncError,     // checksum mismatch or sequence gap; a snapshot was requested
        LocksChanged,
        PeersChanged,
        RoleChanged,   // our own role changed
    };
    struct Action {
        ActionKind kind;
        std::vector<Op> ops;
        bool own = false;
        std::string text;
    };

    // Remote client: produces a Hello.
    void start(uint64_t roomId, std::string const& nickname, uint64_t token, double now, std::string const& password = {});
    // The host's own editor (peer 1): starts active with the host's level already in `store`.
    void startLocal(uint32_t peerId, ObjectStore seed, std::string roomName, double now);

    void onMessage(std::string_view payload, double now);
    void tick(double now);

    void submit(std::vector<Op> ops);
    void setLocks(std::vector<uint64_t> ids);
    void requestSnapshot();
    void sendPlayState(msg::PlayState const& state);
    void leave();

    std::vector<std::string> takeOutbox() { return std::exchange(m_out, {}); }
    std::vector<Action> takeActions() { return std::exchange(m_actions, {}); }

    State state() const { return m_state; }
    uint32_t me() const { return m_me; }
    Role role() const { return m_role; }
    uint64_t token() const { return m_token; }
    uint64_t seq() const { return m_seq; }
    bool awaitingSnapshot() const { return m_awaitingSnapshot; }
    std::string const& roomName() const { return m_roomName; }
    std::vector<PeerEntry> const& peers() const { return m_peers; }
    std::string nicknameOf(uint32_t peer) const;
    uint32_t lockOwner(uint64_t id) const;
    std::unordered_map<uint32_t, std::vector<uint64_t>> const& locks() const { return m_locks; }
    double rttMs() const { return m_rttMs; }
    // Other players currently playing the level, by peer id (jitter-buffered position streams).
    std::unordered_map<uint32_t, PlayBuffer> const& plays() const { return m_plays; }
    uint32_t checksumFailures() const { return m_checksumFailures; }

    ObjectStore store;

private:
    void send(Message const& m);
    void act(ActionKind k, std::string text = {}) { m_actions.push_back({k, {}, false, std::move(text)}); }
    void fail(std::string reason);
    void setLockGroup(LockGroup const& g);

    State m_state = State::Idle;
    uint32_t m_me = 0;
    Role m_role = Role::Viewer;
    uint64_t m_token = 0;
    uint64_t m_seq = 0;
    uint32_t m_clientSeq = 0;
    bool m_awaitingSnapshot = false;
    std::string m_roomName;
    std::vector<PeerEntry> m_peers;
    std::unordered_map<uint64_t, uint32_t> m_lockOwner;
    std::unordered_map<uint32_t, std::vector<uint64_t>> m_locks;
    std::unordered_map<uint32_t, PlayBuffer> m_plays;
    std::vector<std::string> m_out;
    std::vector<Action> m_actions;
    double m_lastHeard = 0;
    double m_lastPing = 0;
    double m_lastTick = 0;
    double m_rttMs = 0;
    uint32_t m_checksumFailures = 0;
};

} // namespace et
