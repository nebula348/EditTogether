#include "HostCore.hpp"

#include <algorithm>

namespace et {

std::string versionMismatch(uint16_t theirs) {
    return "This room uses a different EditTogether version (room protocol " + std::to_string(kProtocolVersion) +
           ", yours " + std::to_string(theirs) + "). Everyone needs the same version of the mod.";
}

HostCore::HostCore(Config cfg, ObjectStore seed, double now, uint64_t tokenSeed)
  : m_cfg(std::move(cfg)), m_store(std::move(seed)), m_lastTick(now), m_lastChecksum(now), m_rng(tokenSeed | 1) {
    m_cfg.maxPlayers = std::clamp(m_cfg.maxPlayers, 2, static_cast<int>(kMaxPeers));
    Peer host;
    host.id = kLocalPeer;
    host.nickname = m_cfg.hostName;
    host.role = Role::Admin;
    host.active = true;
    host.helloReceived = true;
    host.lastSeen = now;
    host.rateRefill = now;
    m_peers[kLocalPeer] = host;
}

uint64_t HostCore::newToken() {
    // splitmix64
    uint64_t z = (m_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return z ? z : 1;
}

uint32_t HostCore::onConnect(double now) {
    Peer p;
    p.id = m_nextPeer++;
    p.lastSeen = now;
    p.requestedAt = now;
    p.rateRefill = now;
    m_peers[p.id] = p;
    return p.id;
}

void HostCore::send(uint32_t peer, Message const& m, bool closeAfter) {
    m_out.push_back({peer, encode(m), closeAfter});
}

void HostCore::broadcast(Message const& m) {
    auto bytes = encode(m);
    for (auto const& [id, p] : m_peers)
        if (p.active) m_out.push_back({id, bytes, false});
}

void HostCore::broadcastExcept(uint32_t skip, Message const& m) {
    auto bytes = encode(m);
    for (auto const& [id, p] : m_peers)
        if (p.active && id != skip) m_out.push_back({id, bytes, false});
}

uint32_t HostCore::lockOwner(uint64_t id) const {
    auto it = m_lockOwner.find(id);
    return it == m_lockOwner.end() ? 0 : it->second;
}

int HostCore::activeCount() const {
    return static_cast<int>(std::count_if(m_peers.begin(), m_peers.end(), [](auto const& kv) { return kv.second.active; }));
}

int HostCore::pendingCount() const {
    return static_cast<int>(std::count_if(m_peers.begin(), m_peers.end(), [](auto const& kv) {
        return !kv.second.active && kv.second.helloReceived;
    }));
}

void HostCore::removePeer(uint32_t peer, std::string_view reason, bool forgetToken, bool silent) {
    if (peer == kLocalPeer) return;
    auto it = m_peers.find(peer);
    if (it == m_peers.end()) return;
    Peer p = it->second;
    m_peers.erase(it);
    m_out.push_back({peer, {}, true}); // make sure the transport is closed
    if (forgetToken) m_knownTokens.erase(p.token);
    if (p.active) {
        if (m_playing.erase(peer)) {
            msg::PlayState stop;
            stop.peer = peer;
            m_playBatch.push_back(stop);
        }
        releaseLocks(peer);
        broadcastPeerList();
        if (!silent) m_events.push_back({EventKind::PeerLeft, peer, p.nickname, std::string(reason)});
    }
}

void HostCore::onDisconnect(uint32_t peer, std::string_view reason) {
    removePeer(peer, reason, false);
}

void HostCore::protocolError(uint32_t peer, std::string_view what) {
    if (peer == kLocalPeer) return;
    send(peer, msg::Kicked{std::string("Protocol error: ") + std::string(what)}, true);
    removePeer(peer, what, true);
}

void HostCore::onMessage(uint32_t peerId, std::string_view payload, double now) {
    if (m_closed) return;
    auto it = m_peers.find(peerId);
    if (it == m_peers.end()) return;
    Peer& p = it->second;
    auto m = decode(payload);
    if (!m) {
        // A Hello from another EditTogether version doesn't decode; say so instead of "malformed packet".
        if (auto v = peekHelloProtocol(payload); v && *v != kProtocolVersion && !p.active && !p.helloReceived) {
            p.helloReceived = true;
            return reject(peerId, versionMismatch(*v));
        }
        return protocolError(peerId, "malformed packet");
    }
    p.lastSeen = now;

    if (auto ping = std::get_if<msg::Ping>(&*m)) return send(peerId, msg::Pong{ping->time});
    if (std::holds_alternative<msg::Pong>(*m)) return;
    if (std::holds_alternative<msg::Leave>(*m)) return removePeer(peerId, "Left the room", true);

    if (!p.active) {
        auto hello = std::get_if<msg::Hello>(&*m);
        if (!hello || p.helloReceived) return protocolError(peerId, "unexpected message before approval");
        return handleHello(p, *hello, now);
    }

    if (peerId != kLocalPeer && !takeRateToken(p, now)) {
        if (!p.needsResync) m_events.push_back({EventKind::RateLimited, peerId, p.nickname, {}});
        p.needsResync = true; // dropped ops would leave the peer diverged; resync it on the next tick
        return;
    }

    if (auto ops = std::get_if<msg::Ops>(&*m)) return handleOps(p, *ops);
    if (auto locks = std::get_if<msg::LockSet>(&*m)) return handleLocks(p, locks->ids);
    if (auto play = std::get_if<msg::PlayState>(&*m)) {
        if (play->playing) m_playing.insert(p.id);
        else m_playing.erase(p.id);
        msg::PlayState out = *play;
        out.peer = p.id; // never trust the sender's claim of who is playing
        // Batched: one message per recipient every kPlayFlush instead of one per sample per recipient.
        if (m_playBatch.size() < kMaxPeers * 64) m_playBatch.push_back(out);
        return;
    }
    if (std::holds_alternative<msg::SnapshotRequest>(*m)) {
        if (peerId != kLocalPeer && now - p.lastSnapshotAt >= 2.0) sendSnapshot(p, now);
        return;
    }
    if (std::holds_alternative<msg::Hello>(*m)) return;
    protocolError(peerId, "unexpected message type");
}

bool HostCore::takeRateToken(Peer& p, double now) {
    p.rateTokens = std::min(kRateBurst, p.rateTokens + (now - p.rateRefill) * kRatePerSec);
    p.rateRefill = now;
    if (p.rateTokens < 1.0) return false;
    p.rateTokens -= 1.0;
    return true;
}

void HostCore::handleHello(Peer& p, msg::Hello const& h, double now) {
    p.helloReceived = true;
    p.nickname = sanitizeName(h.nickname, kMaxNickname, "Player");
    if (h.protocol != kProtocolVersion) return reject(p.id, versionMismatch(h.protocol));
    if (h.roomId != 0 && h.roomId != m_cfg.roomId) return reject(p.id, "Room no longer exists");

    // Previously approved player reconnecting: no approval or password needed, role kept.
    if (auto known = h.token ? m_knownTokens.find(h.token) : m_knownTokens.end(); known != m_knownTokens.end()) {
        // The old connection may still look alive (it takes kPeerTimeout to notice a dropped Wi-Fi link):
        // the token proves it's the same player, so the new connection replaces it.
        auto stale = std::find_if(m_peers.begin(), m_peers.end(), [&](auto const& kv) {
            return kv.first != p.id && kv.second.active && kv.second.token == h.token;
        });
        if (stale != m_peers.end()) removePeer(stale->first, "Reconnected", false, true);
        else if (activeCount() >= m_cfg.maxPlayers) return reject(p.id, "Room is full");
        p.token = h.token;
        p.nickname = known->second.nickname;
        return activate(p, known->second.role, now, true);
    }
    if (activeCount() >= m_cfg.maxPlayers) return reject(p.id, "Room is full");
    if (!m_cfg.password.empty() && h.password != m_cfg.password) return reject(p.id, "Wrong password");
    if (pendingCount() > static_cast<int>(kMaxPending)) return reject(p.id, "Too many pending requests");
    p.requestedAt = now;
    send(p.id, msg::JoinPending{});
    m_events.push_back({EventKind::JoinRequest, p.id, p.nickname, {}});
}

void HostCore::activate(Peer& p, Role role, double now, bool reconnect) {
    p.active = true;
    p.role = role;
    if (p.token == 0) p.token = newToken();
    m_knownTokens[p.token] = {p.nickname, role};
    send(p.id, msg::JoinAccepted{p.id, p.token, m_cfg.roomName});
    sendSnapshot(p, now);
    broadcastPeerList();
    m_events.push_back({EventKind::PeerJoined, p.id, p.nickname, reconnect ? "reconnected" : ""});
}

void HostCore::accept(uint32_t peer, double now) {
    auto it = m_peers.find(peer);
    if (it == m_peers.end() || it->second.active || !it->second.helloReceived) return;
    if (activeCount() >= m_cfg.maxPlayers) return reject(peer, "Room is full");
    activate(it->second, Role::Editor, now, false);
}

void HostCore::reject(uint32_t peer, std::string_view reason) {
    auto it = m_peers.find(peer);
    if (it == m_peers.end() || it->second.active) return;
    send(peer, msg::JoinRejected{std::string(reason)}, true);
    removePeer(peer, reason, true);
}

void HostCore::kick(uint32_t peer, std::string_view reason) {
    if (peer == kLocalPeer || !m_peers.contains(peer)) return;
    send(peer, msg::Kicked{std::string(reason)}, true);
    removePeer(peer, "Removed by the administrator", true);
}

void HostCore::setRole(uint32_t peer, Role role) {
    auto it = m_peers.find(peer);
    if (peer == kLocalPeer || role == Role::Admin || it == m_peers.end() || !it->second.active) return;
    it->second.role = role;
    m_knownTokens[it->second.token].role = role;
    if (!canEdit(role)) releaseLocks(peer);
    broadcastPeerList();
}

void HostCore::releaseLocks(uint32_t peer) {
    auto it = m_locksByPeer.find(peer);
    if (it == m_locksByPeer.end()) return;
    for (auto id : it->second)
        if (lockOwner(id) == peer) m_lockOwner.erase(id);
    m_locksByPeer.erase(it);
    broadcast(msg::LockState{{peer, {}}});
}

void HostCore::close(std::string_view reason) {
    if (m_closed) return;
    std::vector<uint32_t> ids;
    for (auto const& [id, p] : m_peers)
        if (id != kLocalPeer) ids.push_back(id);
    for (auto id : ids) send(id, msg::RoomClosed{std::string(reason)}, true);
    m_peers.erase(m_peers.upper_bound(kLocalPeer), m_peers.end());
    m_closed = true;
}

void HostCore::broadcastPeerList() {
    msg::PeerList list;
    for (auto const& [id, p] : m_peers)
        if (p.active) list.peers.push_back({id, p.nickname, p.role});
    broadcast(list);
}

void HostCore::sendSnapshot(Peer& p, double now) {
    p.lastSnapshotAt = now;
    p.needsResync = false;
    std::vector<LockGroup> locks;
    for (auto const& [owner, ids] : m_locksByPeer) locks.push_back({owner, ids});
    m_out.push_back({p.id, encodeSnapshot(m_seq, m_store.header, m_store.objects, locks), false});
}

std::string HostCore::validate(Peer const& p, Op const& op) const {
    if (!canEdit(p.role)) return "Viewers cannot edit the level";
    auto lockedByOther = [&](uint64_t id) -> std::string {
        uint32_t owner = lockOwner(id);
        if (owner == 0 || owner == p.id) return {};
        auto it = m_peers.find(owner);
        return "Object is being edited by " + (it != m_peers.end() ? it->second.nickname : std::string("another player"));
    };
    switch (op.kind) {
        case OpKind::Header:
            return isValidHeaderString(op.data) ? "" : "Invalid level settings";
        case OpKind::Create:
            if (idOwner(op.id) != p.id) return "Invalid object identifier";
            if (m_store.has(op.id)) return "Duplicate object identifier";
            if (m_store.objects.size() >= kMaxObjects) return "Object limit reached";
            return isValidObjectString(op.data) ? "" : "Invalid object data";
        case OpKind::Modify:
            if (!m_store.has(op.id)) return "Object no longer exists";
            if (auto why = lockedByOther(op.id); !why.empty()) return why;
            return isValidObjectString(op.data) ? "" : "Invalid object data";
        case OpKind::Delete:
            if (!m_store.has(op.id)) return "Object no longer exists";
            return lockedByOther(op.id);
    }
    return "Unknown operation";
}

Op HostCore::authoritativeFor(Op const& op) const {
    if (op.kind == OpKind::Header) return {OpKind::Header, 0, m_store.header};
    if (auto s = m_store.get(op.id)) return {OpKind::Modify, op.id, *s};
    return {OpKind::Delete, op.id, {}};
}

void HostCore::handleOps(Peer& p, msg::Ops const& m) {
    if (m.clientSeq <= p.lastClientSeq) return; // duplicate or replayed batch
    p.lastClientSeq = m.clientSeq;

    msg::OpsBroadcast accepted;
    accepted.firstSeq = m_seq + 1;
    accepted.fromPeer = p.id;
    accepted.clientSeq = m.clientSeq;
    msg::Correction corr;
    for (auto const& op : m.ops) {
        auto why = validate(p, op);
        if (why.empty()) {
            m_store.apply(op);
            if (op.kind == OpKind::Delete) m_lockOwner.erase(op.id);
            accepted.ops.push_back(op);
        } else {
            corr.ops.push_back(authoritativeFor(op));
            corr.reason = why;
        }
    }
    m_seq += accepted.ops.size();
    if (!accepted.ops.empty()) broadcast(accepted);
    if (!corr.ops.empty()) send(p.id, corr);
}

void HostCore::handleLocks(Peer& p, std::vector<uint64_t> const& ids) {
    auto& mine = m_locksByPeer[p.id];
    for (auto id : mine)
        if (lockOwner(id) == p.id) m_lockOwner.erase(id);
    mine.clear();
    if (canEdit(p.role)) {
        for (auto id : ids) {
            if (!m_store.has(id) || m_lockOwner.contains(id)) continue; // missing, taken, or duplicate in list
            m_lockOwner[id] = p.id;
            mine.push_back(id);
        }
    }
    broadcast(msg::LockState{{p.id, mine}});
}

void HostCore::tick(double now) {
    if (m_closed) return;
    // A long gap means the app was suspended (Android background); don't time everyone out for it.
    if (now - m_lastTick > 5.0)
        for (auto& [id, p] : m_peers) p.lastSeen = now;
    m_lastTick = now;
    m_peers[kLocalPeer].lastSeen = now;

    std::vector<std::pair<uint32_t, bool>> drop; // (peer, wasPending)
    for (auto const& [id, p] : m_peers) {
        if (id == kLocalPeer) continue;
        if (now - p.lastSeen > kPeerTimeout) drop.push_back({id, false});
        else if (!p.active && now - p.requestedAt > kPendingTimeout) drop.push_back({id, true});
    }
    for (auto [id, pending] : drop) {
        if (pending) reject(id, "Join request timed out");
        else removePeer(id, "Connection timed out", false);
    }

    for (auto& [id, p] : m_peers)
        if (p.active && p.needsResync && now - p.lastSnapshotAt >= 2.0) sendSnapshot(p, now);

    if (!m_playBatch.empty() && now - m_lastPlayFlush >= m_playFlush) {
        m_lastPlayFlush = now;
        broadcast(msg::PlayStates{std::exchange(m_playBatch, {})}); // receivers skip their own samples
    }

    if (now - m_lastChecksum >= kChecksumInterval) {
        m_lastChecksum = now;
        broadcast(msg::Checksum{m_seq, m_store.hash(), static_cast<uint32_t>(m_store.objects.size())});
    }
}

} // namespace et
