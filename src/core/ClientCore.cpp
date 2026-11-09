#include "ClientCore.hpp"

#include <algorithm>

namespace et {

void ClientCore::send(Message const& m) {
    m_out.push_back(encode(m));
}

void ClientCore::start(uint64_t roomId, std::string const& nickname, uint64_t token, double now, std::string const& password) {
    *this = ClientCore{};
    m_token = token;
    m_lastHeard = m_lastPing = m_lastTick = now;
    m_state = State::Pending;
    send(msg::Hello{kProtocolVersion, roomId, nickname, token, password});
}

void ClientCore::startLocal(uint32_t peerId, ObjectStore seed, std::string roomName, double now) {
    *this = ClientCore{};
    store = std::move(seed);
    m_me = peerId;
    m_role = Role::Admin;
    m_roomName = std::move(roomName);
    m_state = State::Active;
    m_lastHeard = m_lastPing = m_lastTick = now;
}

void ClientCore::fail(std::string reason) {
    m_state = State::Closed;
    act(ActionKind::ConnectionLost, std::move(reason));
}

std::string ClientCore::nicknameOf(uint32_t peer) const {
    for (auto const& p : m_peers)
        if (p.id == peer) return p.nickname;
    return "another player";
}

uint32_t ClientCore::lockOwner(uint64_t id) const {
    auto it = m_lockOwner.find(id);
    return it == m_lockOwner.end() ? 0 : it->second;
}

void ClientCore::setLockGroup(LockGroup const& g) {
    auto& ids = m_locks[g.owner];
    for (auto id : ids)
        if (lockOwner(id) == g.owner) m_lockOwner.erase(id);
    ids = g.ids;
    for (auto id : ids) m_lockOwner[id] = g.owner;
    if (ids.empty()) m_locks.erase(g.owner);
}

void ClientCore::onMessage(std::string_view payload, double now) {
    if (m_state == State::Closed) return;
    auto m = decode(payload);
    if (!m) return fail("Received invalid data from the host");
    m_lastHeard = now;

    if (auto ping = std::get_if<msg::Ping>(&*m)) return send(msg::Pong{ping->time});
    if (auto pong = std::get_if<msg::Pong>(&*m)) {
        m_rttMs = std::max(0.0, now * 1000.0 - static_cast<double>(pong->time));
        return;
    }
    if (std::holds_alternative<msg::JoinPending>(*m)) {
        if (m_state == State::Pending) act(ActionKind::Pending);
        return;
    }
    if (auto r = std::get_if<msg::JoinRejected>(&*m)) {
        m_state = State::Closed;
        return act(ActionKind::Rejected, r->reason);
    }
    if (auto k = std::get_if<msg::Kicked>(&*m)) {
        m_state = State::Closed;
        return act(ActionKind::Kicked, k->reason);
    }
    if (auto c = std::get_if<msg::RoomClosed>(&*m)) {
        m_state = State::Closed;
        return act(ActionKind::RoomClosed, c->reason);
    }
    if (auto a = std::get_if<msg::JoinAccepted>(&*m)) {
        if (m_state != State::Pending) return fail("Unexpected join confirmation");
        m_me = a->peerId;
        m_token = a->token;
        m_roomName = a->roomName;
        m_role = Role::Editor;
        m_state = State::Active;
        m_awaitingSnapshot = true;
        return act(ActionKind::Joined);
    }
    if (m_state != State::Active) return fail("Unexpected message before approval");

    if (auto s = std::get_if<msg::Snapshot>(&*m)) {
        store.clear();
        store.header = s->header;
        for (auto& [id, str] : s->objects) store.set(id, std::move(str));
        m_seq = s->seq;
        m_lockOwner.clear();
        m_locks.clear();
        for (auto const& g : s->locks) setLockGroup(g);
        m_awaitingSnapshot = false;
        act(ActionKind::LoadSnapshot);
        return act(ActionKind::LocksChanged);
    }
    if (auto b = std::get_if<msg::OpsBroadcast>(&*m)) {
        if (m_awaitingSnapshot) return; // the snapshot will include these
        uint64_t n = b->ops.size();
        if (n == 0 || b->firstSeq + n - 1 <= m_seq) return; // duplicate
        if (b->firstSeq > m_seq + 1) {
            m_awaitingSnapshot = true;
            send(msg::SnapshotRequest{});
            return act(ActionKind::SyncError, "Missed level updates");
        }
        Action a{ActionKind::ApplyOps, {}, b->fromPeer == m_me, {}};
        for (uint64_t i = m_seq + 1 - b->firstSeq; i < n; ++i) {
            store.apply(b->ops[i]);
            a.ops.push_back(std::move(b->ops[i]));
        }
        m_seq = b->firstSeq + n - 1;
        m_actions.push_back(std::move(a));
        return;
    }
    if (auto c = std::get_if<msg::Correction>(&*m)) {
        m_actions.push_back({ActionKind::Correction, std::move(c->ops), false, c->reason});
        return;
    }
    if (auto l = std::get_if<msg::LockState>(&*m)) {
        setLockGroup(l->group);
        return act(ActionKind::LocksChanged);
    }
    if (auto batch = std::get_if<msg::PlayStates>(&*m)) {
        for (auto const& ps : batch->states) {
            if (ps.peer == m_me) continue; // our own samples come back in the shared batch
            if (ps.playing) m_plays[ps.peer].push(ps, now);
            else m_plays.erase(ps.peer);
        }
        return;
    }
    if (auto p = std::get_if<msg::PeerList>(&*m)) {
        m_peers = p->peers;
        std::erase_if(m_plays, [&](auto const& kv) {
            return std::none_of(m_peers.begin(), m_peers.end(), [&](auto const& e) { return e.id == kv.first; });
        });
        act(ActionKind::PeersChanged);
        for (auto const& e : m_peers) {
            if (e.id == m_me && e.role != m_role) {
                m_role = e.role;
                act(ActionKind::RoleChanged, roleName(e.role));
            }
        }
        return;
    }
    if (auto c = std::get_if<msg::Checksum>(&*m)) {
        if (m_awaitingSnapshot || c->seq != m_seq) return;
        if (c->hash != store.hash() || c->count != store.objects.size()) {
            ++m_checksumFailures;
            m_awaitingSnapshot = true;
            send(msg::SnapshotRequest{});
            act(ActionKind::SyncError, "Level state mismatch detected");
        }
        return;
    }
    fail("Unexpected message from the host");
}

void ClientCore::tick(double now) {
    if (m_state == State::Closed || m_state == State::Idle) return;
    if (now - m_lastTick > 5.0) m_lastHeard = now; // app was suspended, not the host's fault
    m_lastTick = now;
    if (m_me == 1) return; // in-process host editor: no transport to watch
    if (now - m_lastHeard > kHostTimeout) return fail("Connection to the host timed out");
    if (now - m_lastPing >= kPingInterval) {
        m_lastPing = now;
        send(msg::Ping{static_cast<uint64_t>(now * 1000.0)});
    }
}

void ClientCore::submit(std::vector<Op> ops) {
    if (m_state != State::Active || ops.empty()) return;
    // Split so no message exceeds the per-message op limit or the host's frame limit.
    size_t i = 0;
    while (i < ops.size()) {
        msg::Ops m{++m_clientSeq, {}};
        size_t bytes = 64;
        while (i < ops.size() && m.ops.size() < kMaxOpsPerMessage && bytes + ops[i].data.size() + 32 < kMaxFrameClientToHost) {
            bytes += ops[i].data.size() + 32;
            m.ops.push_back(std::move(ops[i++]));
        }
        if (m.ops.empty()) { ++i; continue; } // single op larger than a frame: drop (validation would reject it anyway)
        send(m);
    }
}

void ClientCore::setLocks(std::vector<uint64_t> ids) {
    if (m_state != State::Active) return;
    if (ids.size() > kMaxLockIds) ids.resize(kMaxLockIds);
    send(msg::LockSet{std::move(ids)});
}

void ClientCore::requestSnapshot() {
    if (m_state != State::Active || m_me == 1) return;
    m_awaitingSnapshot = true;
    send(msg::SnapshotRequest{});
}

void ClientCore::sendPlayState(msg::PlayState const& state) {
    if (m_state != State::Active) return;
    send(state);
}

void ClientCore::leave() {
    if (m_state == State::Closed) return;
    send(msg::Leave{});
    m_state = State::Closed;
}

} // namespace et
