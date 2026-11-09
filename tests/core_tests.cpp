// Core logic tests: protocol, validation, room authority, ordering, locks, reconciliation.
// Pure C++, runs on the build machine and on Android (see tests/README.md).
#include "core/ClientCore.hpp"
#include "core/Bytes.hpp"
#include "core/HostCore.hpp"
#include "core/Protocol.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>

using namespace et;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

static std::vector<std::pair<char const*, std::function<void()>>>& registry() {
    static std::vector<std::pair<char const*, std::function<void()>>> r;
    return r;
}
#define TEST(name)                                                               \
    static void name();                                                          \
    static bool name##_reg = (registry().push_back({#name, name}), true);        \
    static void name()

static std::string obj(int x) { return "1,1,2," + std::to_string(x) + ",3,15"; }
static msg::PlayState playing(float x, float y, uint8_t mode = 0);

// ---------------------------------------------------------------- harness
// Host + N remote clients wired through in-memory "sockets".
struct Net {
    double now = 100.0;
    HostCore host;
    ClientCore local; // host's own editor (peer 1)
    std::map<uint32_t, ClientCore> clients; // keyed by host-side peer id
    std::map<uint32_t, bool> closed;
    std::vector<ClientCore::Action> localActions;
    std::map<uint32_t, std::vector<ClientCore::Action>> actions;

    explicit Net(ObjectStore seed = {}, int max = 4)
      : host({0xABC, "Room", "Host", max, ""}, seed, 100.0, 42) {
        local.startLocal(1, seed, "Room", now);
    }

    uint32_t connect(std::string nick, uint64_t token = 0, uint64_t roomId = 0xABC) {
        uint32_t id = host.onConnect(now);
        clients[id].start(roomId, nick, token, now);
        pump();
        return id;
    }

    void pump() {
        for (int guard = 0; guard < 50; ++guard) {
            bool any = false;
            for (auto& m : local.takeOutbox()) { any = true; host.onMessage(1, m, now); }
            for (auto& [id, c] : clients)
                for (auto& m : c.takeOutbox())
                    if (!closed[id]) { any = true; host.onMessage(id, m, now); }
            for (auto& o : host.takeOutbox()) {
                any = true;
                if (o.peer == 1) { if (!o.bytes.empty()) local.onMessage(o.bytes, now); }
                else if (!closed[o.peer]) {
                    if (!o.bytes.empty()) clients[o.peer].onMessage(o.bytes, now);
                    if (o.closeAfter) closed[o.peer] = true;
                }
            }
            for (auto& a : local.takeActions()) localActions.push_back(a);
            for (auto& [id, c] : clients)
                for (auto& a : c.takeActions()) actions[id].push_back(a);
            if (!any) return;
        }
    }

    void advance(double dt) {
        now += dt;
        host.tick(now);
        local.tick(now);
        for (auto& [id, c] : clients) c.tick(now);
        pump();
    }

    bool hasAction(uint32_t id, ClientCore::ActionKind k) {
        for (auto& a : actions[id]) if (a.kind == k) return true;
        return false;
    }
    ClientCore::Action const* lastAction(uint32_t id, ClientCore::ActionKind k) {
        ClientCore::Action const* r = nullptr;
        for (auto& a : actions[id]) if (a.kind == k) r = &a;
        return r;
    }
    uint32_t joinAccepted(std::string nick) {
        auto id = connect(nick);
        host.accept(id, now);
        pump();
        return id;
    }
};

// ---------------------------------------------------------------- protocol
TEST(bytes_roundtrip_and_bounds) {
    std::string big = "1,1,2,15,3,45";
    Message m = msg::Ops{7, {{OpKind::Create, makeId(2, 1), big}, {OpKind::Delete, 99, ""}}};
    auto enc = encode(m);
    auto dec = decode(enc);
    CHECK(dec && *dec == m);
    for (size_t cut = 0; cut < enc.size(); ++cut) CHECK(!decode(std::string_view(enc).substr(0, cut)));
    CHECK(!decode(enc + "x")); // trailing garbage
    CHECK(!decode(std::string("\xff", 1)));   // unknown type
    std::string hugeCount = encode(msg::LockSet{});
    hugeCount[1] = hugeCount[2] = hugeCount[3] = hugeCount[4] = '\xff';
    CHECK(!decode(hugeCount)); // count larger than the remaining bytes
}

TEST(every_message_roundtrips) {
    std::vector<Message> all = {
        msg::Hello{1, 5, "Ana", 9, "secret"}, msg::Ops{1, {{OpKind::Header, 0, "kA13,0"}}}, msg::LockSet{{1, 2, 3}},
        msg::SnapshotRequest{}, msg::Leave{}, msg::Ping{5}, msg::Pong{6}, msg::JoinPending{},
        msg::JoinAccepted{2, 77, "Room"}, msg::JoinRejected{"no"}, msg::PeerList{{{1, "Host", Role::Admin}, {2, "Ana", Role::Viewer}}},
        msg::OpsBroadcast{10, 2, 3, {{OpKind::Modify, 4, obj(1)}}}, msg::Correction{{{OpKind::Delete, 4, ""}}, "why"},
        msg::LockState{{2, {4, 5}}}, msg::Checksum{1, 2, 3},
        msg::Snapshot{3, "kA13,0", {{makeId(1, 1), obj(1)}}, {{2, {makeId(1, 1)}}}},
        msg::Kicked{"bye"}, msg::RoomClosed{"closed"}, msg::PlayStates{{playing(1, 2), playing(3, 4, 7)}},
    };
    for (auto const& m : all) {
        auto d = decode(encode(m));
        CHECK(d && *d == m);
    }
}

TEST(announce_roundtrip_and_rejects_garbage) {
    Announce a{kProtocolVersion, 123, 47812, 2, 4, true, true, "Room", "Host", "Level"};
    Announce other = a;
    other.protocol = 3; // a room from another version is still listed, marked with its version
    auto od = decodeAnnounce(encodeAnnounce(other));
    CHECK(od && od->protocol == 3 && od->roomName == "Room");
    auto d = decodeAnnounce(encodeAnnounce(a));
    CHECK(d && *d == a);
    CHECK(!decodeAnnounce("hello"));
    CHECK(!decodeAnnounce(""));
    auto enc = encodeAnnounce(a);
    CHECK(!decodeAnnounce(enc.substr(0, enc.size() - 1)));
    Announce evil = a;
    evil.roomName = std::string(500, 'x') + "\x01";
    CHECK(!decodeAnnounce(encodeAnnounce(evil))); // name longer than reader limit
}

TEST(object_string_validation) {
    CHECK(isValidObjectString("1,1,2,15,3,45"));
    CHECK(isValidObjectString("1,914,2,15,3,45,31,SGVsbG8=,57,1.2.3"));
    CHECK(!isValidObjectString(""));
    CHECK(!isValidObjectString("2,15,3,45"));     // no object id
    CHECK(!isValidObjectString("1,0,2,15"));      // id 0
    CHECK(!isValidObjectString("1,-5,2,15"));
    CHECK(!isValidObjectString("1,1,2"));         // dangling key
    CHECK(!isValidObjectString("1,1;1,2"));       // separator injection
    CHECK(!isValidObjectString("1,1,ab,3"));      // non-numeric key
    CHECK(!isValidObjectString("1,1," + std::string(40000, '1') + ",1"));
    CHECK(isValidHeaderString("kS38,1_40_2_125,kA13,0"));
    CHECK(!isValidHeaderString("kA13,0;1,1"));
}

TEST(header_normalization_drops_editor_view_keys) {
    CHECK(normalizeHeader("kA13,0,kI1,120.5,kI2,30,kI3,1,kA2,1") == "kA13,0,kA2,1");
    CHECK(normalizeHeader("kI1,1") == "");
    CHECK(normalizeHeader("kA13,0") == "kA13,0");
    CHECK(normalizeHeader("kS38,1_2,kS39,3,kA2,0") == "kS38,1_2,kA2,0");
}

TEST(sanitize_names) {
    CHECK(sanitizeName("  Ana  ", 20, "P") == "Ana");
    CHECK(sanitizeName("\x01\x02", 20, "P") == "P");
    CHECK(sanitizeName(std::string(50, 'a'), 20, "P").size() == 20);
}

// ---------------------------------------------------------------- rooms & joining
TEST(join_requires_explicit_approval) {
    Net n;
    auto id = n.connect("Ana");
    CHECK(n.hasAction(id, ClientCore::ActionKind::Pending));
    CHECK(!n.hasAction(id, ClientCore::ActionKind::Joined));
    auto ev = n.host.takeEvents();
    CHECK(ev.size() == 1 && ev[0].kind == HostCore::EventKind::JoinRequest && ev[0].nickname == "Ana");
    n.advance(1.0);
    CHECK(n.clients[id].state() == ClientCore::State::Pending);
    n.host.accept(id, n.now);
    n.pump();
    CHECK(n.hasAction(id, ClientCore::ActionKind::Joined));
    CHECK(n.hasAction(id, ClientCore::ActionKind::LoadSnapshot));
    CHECK(n.clients[id].role() == Role::Editor);
    CHECK(n.clients[id].peers().size() == 2);
}

TEST(join_rejected) {
    Net n;
    auto id = n.connect("Ana");
    n.host.reject(id, "No thanks");
    n.pump();
    auto a = n.lastAction(id, ClientCore::ActionKind::Rejected);
    CHECK(a && a->text == "No thanks");
    CHECK(n.closed[id]);
}

TEST(pending_peer_cannot_send_ops) {
    Net n;
    auto id = n.connect("Mallory");
    n.clients[id].start(0xABC, "x", 0, n.now); // reset to get a fresh outbox; fake an active client
    n.clients[id].takeOutbox();
    n.host.onMessage(id, encode(msg::Ops{1, {{OpKind::Create, makeId(id, 1), obj(1)}}}), n.now);
    n.pump();
    CHECK(n.closed[id]);
    CHECK(n.host.store().objects.empty());
}

TEST(room_full_and_bad_version_and_wrong_room) {
    Net n({}, 2);
    auto a = n.joinAccepted("A");
    CHECK(n.hasAction(a, ClientCore::ActionKind::Joined));
    auto b = n.connect("B");
    auto r = n.lastAction(b, ClientCore::ActionKind::Rejected);
    CHECK(r && r->text == "Room is full");

    Net m;
    uint32_t id = m.host.onConnect(m.now);
    m.host.onMessage(id, encode(msg::Hello{99, 0, "Old", 0, ""}), m.now);
    auto out = m.host.takeOutbox();
    CHECK(!out.empty() && out[0].closeAfter);
    auto rej = decode(out[0].bytes);
    CHECK(rej && std::get<msg::JoinRejected>(*rej).reason == versionMismatch(99));

    // An older client's Hello (v4 layout: no password field) doesn't decode, but still gets a clear answer.
    ByteWriter old;
    old.u8(1);
    old.u16(4);
    old.u64(0);
    old.str("Old");
    old.u64(0);
    uint32_t oid = m.host.onConnect(m.now);
    m.host.onMessage(oid, old.buf, m.now);
    auto out2 = m.host.takeOutbox();
    auto rej2 = out2.empty() ? std::nullopt : decode(out2[0].bytes);
    CHECK(rej2 && std::holds_alternative<msg::JoinRejected>(*rej2) && std::get<msg::JoinRejected>(*rej2).reason == versionMismatch(4));
    CHECK(peekHelloProtocol(old.buf) == std::optional<uint16_t>(4));
    CHECK(!peekHelloProtocol(encode(msg::Ping{1})));

    auto w = m.connect("W", 0, 0xDEAD);
    CHECK(m.hasAction(w, ClientCore::ActionKind::Rejected));
}

TEST(malformed_packet_kicks_peer) {
    Net n;
    auto id = n.joinAccepted("A");
    n.host.onMessage(id, "\x06garbage", n.now);
    n.pump();
    CHECK(n.closed[id]);
    CHECK(n.hasAction(id, ClientCore::ActionKind::Kicked));
}

TEST(pending_request_times_out) {
    Net n;
    auto id = n.connect("Slow");
    for (int i = 0; i < 70; ++i) {
        n.now += 1.0;
        n.clients[id].tick(n.now);
        n.host.tick(n.now);
        n.pump();
    }
    CHECK(n.hasAction(id, ClientCore::ActionKind::Rejected));
}

// ---------------------------------------------------------------- permissions
TEST(viewer_cannot_edit_and_gets_correction) {
    ObjectStore seed;
    seed.set(makeId(1, 1), obj(10));
    Net n(seed);
    auto id = n.joinAccepted("V");
    n.host.setRole(id, Role::Viewer);
    n.pump();
    CHECK(n.clients[id].role() == Role::Viewer);
    CHECK(n.hasAction(id, ClientCore::ActionKind::RoleChanged));
    n.clients[id].submit({{OpKind::Modify, makeId(1, 1), obj(99)}});
    n.pump();
    CHECK(*n.host.store().get(makeId(1, 1)) == obj(10));
    auto c = n.lastAction(id, ClientCore::ActionKind::Correction);
    CHECK(c && c->ops.size() == 1 && c->ops[0].data == obj(10));
}

TEST(create_in_foreign_id_space_rejected) {
    Net n;
    auto a = n.joinAccepted("A");
    n.clients[a].submit({{OpKind::Create, makeId(1, 500), obj(1)}}); // pretending to be the host
    n.pump();
    CHECK(n.host.store().objects.empty());
    auto c = n.lastAction(a, ClientCore::ActionKind::Correction);
    CHECK(c && c->ops[0].kind == OpKind::Delete);
}

TEST(invalid_object_data_rejected) {
    Net n;
    auto a = n.joinAccepted("A");
    n.clients[a].submit({{OpKind::Create, makeId(a, 1), "1,1;evil"}});
    n.pump();
    CHECK(n.host.store().objects.empty());
}

TEST(admin_kick_and_token_revoked) {
    Net n;
    auto a = n.joinAccepted("A");
    uint64_t tok = n.clients[a].token();
    n.host.kick(a, "Bye");
    n.pump();
    CHECK(n.hasAction(a, ClientCore::ActionKind::Kicked));
    auto again = n.connect("A", tok);
    CHECK(n.hasAction(again, ClientCore::ActionKind::Pending)); // kicked tokens need approval again
    CHECK(!n.hasAction(again, ClientCore::ActionKind::Joined));
}

// ---------------------------------------------------------------- ordering & duplicates
TEST(ops_are_sequenced_and_echoed) {
    Net n;
    auto a = n.joinAccepted("A");
    auto b = n.joinAccepted("B");
    n.clients[a].submit({{OpKind::Create, makeId(a, 1), obj(1)}, {OpKind::Create, makeId(a, 2), obj(2)}});
    n.pump();
    CHECK(n.host.seq() == 2);
    CHECK(n.clients[b].seq() == 2 && n.clients[b].store.objects.size() == 2);
    CHECK(n.local.store.objects.size() == 2);
    auto echo = n.lastAction(a, ClientCore::ActionKind::ApplyOps);
    CHECK(echo && echo->own);
    auto other = n.lastAction(b, ClientCore::ActionKind::ApplyOps);
    CHECK(other && !other->own && other->ops.size() == 2);
}

TEST(duplicate_client_batch_ignored) {
    Net n;
    auto a = n.joinAccepted("A");
    auto batch = encode(msg::Ops{5, {{OpKind::Create, makeId(a, 1), obj(1)}}});
    n.host.onMessage(a, batch, n.now);
    n.host.onMessage(a, batch, n.now); // replay
    n.pump();
    CHECK(n.host.seq() == 1);
}

TEST(duplicate_broadcast_ignored_and_gap_requests_snapshot) {
    ClientCore c;
    c.start(1, "A", 0, 0);
    c.onMessage(encode(msg::JoinAccepted{2, 1, "R"}), 0);
    c.onMessage(encode(msg::Snapshot{0, "", {}, {}}), 0);
    auto b1 = encode(msg::OpsBroadcast{1, 1, 1, {{OpKind::Create, makeId(1, 1), obj(1)}}});
    c.onMessage(b1, 0);
    c.onMessage(b1, 0);
    CHECK(c.seq() == 1);
    int applies = 0;
    for (auto& a : c.takeActions()) applies += a.kind == ClientCore::ActionKind::ApplyOps;
    CHECK(applies == 1);
    c.takeOutbox();
    c.onMessage(encode(msg::OpsBroadcast{5, 1, 2, {{OpKind::Create, makeId(1, 2), obj(2)}}}), 0);
    CHECK(c.awaitingSnapshot());
    bool requested = false;
    for (auto& m : c.takeOutbox()) requested |= std::holds_alternative<msg::SnapshotRequest>(*decode(m));
    CHECK(requested);
    // overlapping batch only applies the new tail
    ClientCore d;
    d.start(1, "A", 0, 0);
    d.onMessage(encode(msg::JoinAccepted{2, 1, "R"}), 0);
    d.onMessage(encode(msg::Snapshot{1, "", {{makeId(1, 1), obj(1)}}, {}}), 0);
    d.takeActions();
    d.onMessage(encode(msg::OpsBroadcast{1, 1, 1, {{OpKind::Modify, makeId(1, 1), obj(9)}, {OpKind::Create, makeId(1, 2), obj(2)}}}), 0);
    CHECK(d.seq() == 2 && *d.store.get(makeId(1, 1)) == obj(1) && d.store.has(makeId(1, 2)));
}

// ---------------------------------------------------------------- locks
TEST(locks_block_other_editors) {
    ObjectStore seed;
    seed.set(makeId(1, 1), obj(1));
    Net n(seed);
    auto a = n.joinAccepted("A");
    auto b = n.joinAccepted("B");
    n.clients[a].setLocks({makeId(1, 1)});
    n.pump();
    CHECK(n.host.lockOwner(makeId(1, 1)) == a);
    CHECK(n.clients[b].lockOwner(makeId(1, 1)) == a);
    n.clients[b].setLocks({makeId(1, 1)});
    n.pump();
    CHECK(n.host.lockOwner(makeId(1, 1)) == a); // simultaneous attempt: first one wins
    n.clients[b].submit({{OpKind::Modify, makeId(1, 1), obj(5)}, {OpKind::Delete, makeId(1, 1), ""}});
    n.pump();
    CHECK(*n.host.store().get(makeId(1, 1)) == obj(1));
    auto c = n.lastAction(b, ClientCore::ActionKind::Correction);
    CHECK(c && c->text == "Object is being edited by A");
    n.clients[a].submit({{OpKind::Modify, makeId(1, 1), obj(7)}});
    n.pump();
    CHECK(*n.host.store().get(makeId(1, 1)) == obj(7));
}

TEST(locks_released_on_disconnect_and_by_admin) {
    ObjectStore seed;
    seed.set(makeId(1, 1), obj(1));
    seed.set(makeId(1, 2), obj(2));
    Net n(seed);
    auto a = n.joinAccepted("A");
    auto b = n.joinAccepted("B");
    n.clients[a].setLocks({makeId(1, 1)});
    n.clients[b].setLocks({makeId(1, 2)});
    n.pump();
    n.host.onDisconnect(a, "gone");
    n.pump();
    CHECK(n.host.lockOwner(makeId(1, 1)) == 0);
    CHECK(n.clients[b].lockOwner(makeId(1, 1)) == 0);
    n.host.releaseLocks(b);
    n.pump();
    CHECK(n.host.lockOwner(makeId(1, 2)) == 0);
    CHECK(n.local.lockOwner(makeId(1, 2)) == 0);
}

TEST(stale_lock_released_on_timeout) {
    ObjectStore seed;
    seed.set(makeId(1, 1), obj(1));
    Net n(seed);
    auto a = n.joinAccepted("A");
    n.clients[a].setLocks({makeId(1, 1)});
    n.pump();
    n.closed[a] = true; // peer vanishes without closing the socket
    for (int i = 0; i < 20; ++i) {
        n.now += 1.0;
        n.host.tick(n.now);
        n.pump();
    }
    CHECK(n.host.lockOwner(makeId(1, 1)) == 0);
    CHECK(n.host.activeCount() == 1);
}

TEST(suspension_does_not_time_out_peers) {
    Net n;
    auto a = n.joinAccepted("A");
    n.now += 60.0; // app was in background for a minute
    n.host.tick(n.now);
    n.clients[a].tick(n.now);
    n.pump();
    CHECK(n.host.activeCount() == 2);
    CHECK(n.clients[a].state() == ClientCore::State::Active);
}

TEST(deleting_locked_object_by_owner_clears_lock) {
    ObjectStore seed;
    seed.set(makeId(1, 1), obj(1));
    Net n(seed);
    auto a = n.joinAccepted("A");
    n.clients[a].setLocks({makeId(1, 1)});
    n.pump();
    n.clients[a].submit({{OpKind::Delete, makeId(1, 1), ""}});
    n.pump();
    CHECK(!n.host.store().has(makeId(1, 1)));
    CHECK(n.host.lockOwner(makeId(1, 1)) == 0);
}

// ---------------------------------------------------------------- reconciliation
TEST(reconnect_with_token_skips_approval_and_resyncs) {
    ObjectStore seed;
    seed.set(makeId(1, 1), obj(1));
    Net n(seed);
    auto a = n.joinAccepted("A");
    n.host.setRole(a, Role::Viewer);
    n.pump();
    uint64_t tok = n.clients[a].token();
    n.host.onDisconnect(a, "wifi dropped");
    n.local.submit({{OpKind::Modify, makeId(1, 1), obj(2)}});
    n.pump();
    auto a2 = n.connect("A", tok);
    CHECK(n.hasAction(a2, ClientCore::ActionKind::Joined));
    CHECK(n.hasAction(a2, ClientCore::ActionKind::LoadSnapshot));
    CHECK(*n.clients[a2].store.get(makeId(1, 1)) == obj(2));
    CHECK(n.clients[a2].role() == Role::Viewer); // role survives reconnection
}

TEST(fast_reconnect_replaces_stale_connection) {
    ObjectStore seed;
    seed.set(makeId(1, 1), obj(1));
    Net n(seed, 2); // full room: host + A
    auto a = n.joinAccepted("A");
    n.host.setRole(a, Role::Viewer);
    n.clients[a].setLocks({});
    n.pump();
    uint64_t tok = n.clients[a].token();
    n.closed[a] = true; // Wi-Fi blip: the host hasn't noticed yet, A's old connection still counts as active
    n.host.takeEvents();
    auto a2 = n.connect("A", tok);
    CHECK(n.hasAction(a2, ClientCore::ActionKind::Joined));      // replaced, even though the room is "full"
    CHECK(!n.hasAction(a2, ClientCore::ActionKind::Pending));    // no second approval
    CHECK(n.clients[a2].role() == Role::Viewer);                  // role kept
    CHECK(n.host.activeCount() == 2);
    CHECK(!n.host.peers().contains(a));                           // stale connection gone
    bool requested = false, left = false;
    for (auto& e : n.host.takeEvents()) {
        requested |= e.kind == HostCore::EventKind::JoinRequest;
        left |= e.kind == HostCore::EventKind::PeerLeft;
    }
    CHECK(!requested && !left); // the admin sees no spurious "wants to join" / "disconnected"
    // A token the host never issued still needs approval.
    auto stranger = n.connect("X", 0x1234);
    CHECK(n.hasAction(stranger, ClientCore::ActionKind::Rejected)); // room is full for newcomers
}

TEST(pending_requests_capped_at_8) {
    Net n({}, 50);
    int pending = 0, rejected = 0;
    for (int i = 0; i < 10; ++i) {
        auto id = n.connect("P" + std::to_string(i));
        pending += n.hasAction(id, ClientCore::ActionKind::Pending);
        rejected += n.hasAction(id, ClientCore::ActionKind::Rejected);
    }
    CHECK(pending == 8);
    CHECK(rejected == 2);
    CHECK(n.host.pendingCount() == 8);
}

TEST(checksum_mismatch_triggers_snapshot_recovery) {
    Net n;
    auto a = n.joinAccepted("A");
    n.local.submit({{OpKind::Create, makeId(1, 1), obj(1)}});
    n.pump();
    n.clients[a].store.set(makeId(1, 1), obj(666)); // simulate corrupted local state
    n.advance(HostCore::kChecksumInterval + 0.1);
    n.advance(2.1); // snapshot request rate limit window
    CHECK(n.hasAction(a, ClientCore::ActionKind::SyncError));
    CHECK(*n.clients[a].store.get(makeId(1, 1)) == obj(1));
    CHECK(!n.clients[a].awaitingSnapshot());
}

TEST(rate_limit_forces_resync_instead_of_silent_divergence) {
    Net n;
    auto a = n.joinAccepted("A");
    for (uint32_t i = 1; i <= 400; ++i)
        n.host.onMessage(a, encode(msg::Ops{i, {{OpKind::Create, makeId(a, i), obj(i)}}}), n.now);
    auto ev = n.host.takeEvents();
    bool limited = false;
    for (auto& e : ev) limited |= e.kind == HostCore::EventKind::RateLimited;
    CHECK(limited);
    CHECK(n.host.seq() < 400); // some batches were dropped
    n.pump();
    n.advance(2.5);
    int snapshots = 0;
    for (auto& act : n.actions[a]) snapshots += act.kind == ClientCore::ActionKind::LoadSnapshot;
    CHECK(snapshots >= 2); // initial join + forced resync
    CHECK(n.clients[a].store.hash() == n.host.store().hash());
}

TEST(random_concurrent_editing_converges) {
    std::mt19937 rng(1234);
    Net n;
    auto a = n.joinAccepted("A");
    auto b = n.joinAccepted("B");
    std::map<uint32_t, uint32_t> counters;
    std::vector<uint32_t> who = {1, a, b};
    for (int step = 0; step < 2000; ++step) {
        uint32_t p = who[rng() % 3];
        ClientCore& c = p == 1 ? n.local : n.clients[p];
        auto& objs = c.store.objects;
        int r = rng() % 10;
        if (r < 4 || objs.empty()) {
            c.submit({{OpKind::Create, makeId(p, ++counters[p]), obj(rng() % 1000)}});
        } else {
            auto it = std::next(objs.begin(), rng() % objs.size());
            if (r < 8) c.submit({{OpKind::Modify, it->first, obj(rng() % 1000)}});
            else c.submit({{OpKind::Delete, it->first, ""}});
        }
        if (rng() % 3 == 0) c.setLocks({objs.empty() ? 0 : objs.begin()->first});
        if (rng() % 5 == 0) n.pump();
    }
    n.pump();
    n.advance(HostCore::kChecksumInterval + 0.1);
    CHECK(n.local.store.hash() == n.host.store().hash());
    CHECK(n.clients[a].store.hash() == n.host.store().hash());
    CHECK(n.clients[b].store.hash() == n.host.store().hash());
    CHECK(!n.hasAction(a, ClientCore::ActionKind::SyncError));
    CHECK(!n.hasAction(b, ClientCore::ActionKind::SyncError));
}

TEST(host_close_notifies_everyone) {
    Net n;
    auto a = n.joinAccepted("A");
    n.host.close("Host closed the room");
    n.pump();
    auto c = n.lastAction(a, ClientCore::ActionKind::RoomClosed);
    CHECK(c && c->text == "Host closed the room");
}

TEST(header_ops_sync) {
    Net n;
    auto a = n.joinAccepted("A");
    n.clients[a].submit({{OpKind::Header, 0, "kA13,5"}});
    n.pump();
    CHECK(n.local.store.header == "kA13,5");
    CHECK(n.host.store().header == "kA13,5");
}

static msg::PlayState playing(float x, float y, uint8_t mode) {
    msg::PlayState s;
    s.playing = true;
    s.x = x;
    s.y = y;
    s.mode = mode;
    s.icon = 42;
    s.cube = 7;
    s.rotation = 90.f;
    s.flags = msg::PlayState::UpsideDown;
    return s;
}

TEST(play_state_relayed_and_cleared) {
    Net n;
    auto a = n.joinAccepted("A");
    auto b = n.joinAccepted("B");
    auto host = playing(120.5f, 45.f, 1);
    host.peer = 77; // a forged sender id must be replaced by the real one
    n.local.sendPlayState(host); // host playtesting as a ship
    n.pump(); n.advance(HostCore::kPlayFlush + 0.001); // relayed in the next batch
    CHECK(n.clients[a].plays().count(1) == 1);
    CHECK(n.clients[a].plays().count(77) == 0);
    if (n.clients[a].plays().count(1)) {
        auto p = n.clients[a].plays().at(1).at(n.now);
        CHECK(p && p->x == 120.5f);
        CHECK(p && p->mode == 1 && p->icon == 42 && p->rotation == 90.f && (p->flags & msg::PlayState::UpsideDown));
    }
    CHECK(n.clients[b].plays().count(1) == 1);
    CHECK(n.local.plays().empty()); // not echoed to the player itself
    n.local.sendPlayState(msg::PlayState{});
    n.pump(); n.advance(HostCore::kPlayFlush + 0.001);
    CHECK(n.clients[a].plays().empty());
    n.clients[a].sendPlayState(playing(10, 10));
    n.pump(); n.advance(HostCore::kPlayFlush + 0.001);
    CHECK(n.clients[b].plays().count(a) == 1);
    CHECK(n.clients[a].plays().count(a) == 0); // own samples ignored
    n.host.onDisconnect(a, "gone"); // a player who drops stops "playing" for everyone
    n.pump(); n.advance(HostCore::kPlayFlush + 0.001);
    CHECK(n.clients[b].plays().empty());

    // Many samples within one flush window reach everyone as a single message, none dropped.
    for (int i = 0; i < 3; ++i) {
        auto s = playing(static_cast<float>(i), 0);
        s.time = 1000 + i * 33;
        n.local.sendPlayState(s);
    }
    n.pump();
    size_t batches = 0, samples = 0;
    n.now += HostCore::kPlayFlush + 0.001;
    n.host.tick(n.now);
    for (auto& o : n.host.takeOutbox()) {
        auto m = decode(o.bytes);
        if (m && o.peer == b && std::holds_alternative<msg::PlayStates>(*m)) {
            ++batches;
            samples += std::get<msg::PlayStates>(*m).states.size();
        }
    }
    CHECK(batches == 1 && samples == 3);
}

TEST(password_required_unless_reconnecting) {
    Net n;
    HostCore locked({0xABC, "Room", "Host", 4, "hunter2"}, {}, n.now, 9);
    auto try_ = [&](std::string pw, uint64_t token = 0) {
        uint32_t id = locked.onConnect(n.now);
        locked.onMessage(id, encode(msg::Hello{kProtocolVersion, 0xABC, "Ana", token, pw}), n.now);
        std::optional<Message> first;
        for (auto& o : locked.takeOutbox())
            if (o.peer == id && !o.bytes.empty() && !first) first = decode(o.bytes);
        return std::make_pair(id, first);
    };
    auto [bad, r1] = try_("wrong");
    CHECK(r1 && std::holds_alternative<msg::JoinRejected>(*r1) && std::get<msg::JoinRejected>(*r1).reason == "Wrong password");
    auto [none, r2] = try_("");
    CHECK(r2 && std::holds_alternative<msg::JoinRejected>(*r2));
    auto [good, r3] = try_("hunter2");
    CHECK(r3 && std::holds_alternative<msg::JoinPending>(*r3)); // right password still needs approval
    locked.accept(good, n.now);
    uint64_t token = 0;
    for (auto& o : locked.takeOutbox())
        if (auto m = decode(o.bytes); m && std::holds_alternative<msg::JoinAccepted>(*m)) token = std::get<msg::JoinAccepted>(*m).token;
    CHECK(token != 0);
    locked.onDisconnect(good, "wifi");
    auto [again, r4] = try_("", token); // reconnect with token, no password
    CHECK(r4 && std::holds_alternative<msg::JoinAccepted>(*r4));
}

// ---------------------------------------------------------------- movement interpolation
static msg::PlayState at(double senderSec, float x, float y, float rot = 0, uint8_t life = 0) {
    msg::PlayState s = playing(x, y);
    s.time = static_cast<uint64_t>(senderSec * 1000.0);
    s.rotation = rot;
    s.life = life;
    return s;
}

TEST(play_buffer_interpolates_100ms_behind) {
    PlayBuffer b;
    // Sender clock is 500 s ahead of ours; 20 ms network delay; 30 Hz samples moving at 311 units/s.
    for (int i = 0; i <= 30; ++i) {
        double send = 500.0 + i / 30.0;
        b.push(at(send, static_cast<float>(311.0 * i / 30.0), 105), send - 500.0 + 0.020);
    }
    double now = 1.0 + 0.020; // local time at which the newest sample arrived
    auto s = b.at(now);
    CHECK(s.has_value());
    // Drawn exactly kDelay behind the newest sample: x = 311 * (1.0 - 0.1)
    CHECK(s && std::abs(s->x - 311.f * 0.9f) < 0.5f);
    auto later = b.at(now + 0.05);
    CHECK(later && std::abs(later->x - 311.f * 0.95f) < 0.5f); // moves at the real speed, no lag build-up
    auto held = b.at(now + 1.0);
    CHECK(held && held->x == 311.f); // nothing newer: hold the last real state, never extrapolate
}

TEST(play_buffer_absorbs_jitter) {
    PlayBuffer b;
    double delays[] = {0.010, 0.080, 0.015, 0.060, 0.012, 0.090, 0.011, 0.040};
    for (int i = 0; i < 64; ++i) {
        double send = 10.0 + i / 30.0;
        b.push(at(send, static_cast<float>(i * 10), 0), send + delays[i % 8]);
    }
    // Despite irregular arrival, motion is uniform: positions at equal steps advance equally.
    double t0 = 10.0 + 63 / 30.0 + 0.010;
    auto a = b.at(t0), c = b.at(t0 + 0.0333);
    CHECK(a && c && std::abs((c->x - a->x) - 10.f) < 0.6f);
}

TEST(play_buffer_snaps_on_respawn_and_teleport) {
    PlayBuffer b;
    b.push(at(1.000, 3000, 105, 0, 1), 1.000); // about to die far into the level
    b.push(at(1.033, 3010, 105, 0, 1), 1.033);
    b.push(at(1.066, 15, 105, 0, 2), 1.066);   // respawned at the start: new life
    b.push(at(1.100, 25, 105, 0, 2), 1.100);
    auto mid = b.at(1.100 + 0.05 - 0.0); // render time 1.050: between the last old-life and first new-life sample
    CHECK(mid && mid->x == 3010.f);       // no glide backwards across the level
    PlayBuffer t;
    t.push(at(2.000, 100, 105), 2.000);
    t.push(at(2.033, 100, 900), 2.033); // teleport portal: 795 units in 33 ms
    auto tp = t.at(2.033 + 0.1 - 0.016);
    CHECK(tp && tp->y == 105.f);
    CHECK(PlayBuffer::discontinuous(at(0, 0, 0), at(0.033, 0, 900)));
    CHECK(!PlayBuffer::discontinuous(at(0, 0, 0), at(0.033, 10, 20)));
}

TEST(play_buffer_rotation_shortest_path_any_range) {
    CHECK(std::abs(PlayBuffer::angleDelta(350.f, 10.f) - 20.f) < 1e-3f);
    CHECK(std::abs(PlayBuffer::angleDelta(10.f, 350.f) + 20.f) < 1e-3f);
    CHECK(std::abs(PlayBuffer::angleDelta(-88.f, 512.f) - 240.f + 360.f) < 1e-3f); // 600 = 240 = -120
    CHECK(std::abs(PlayBuffer::angleDelta(-700.f, 20.f) - 0.f) < 1e-3f);           // 720 = 0
    CHECK(std::abs(PlayBuffer::angleDelta(540.f, -540.f)) < 1e-3f);
    PlayBuffer b;
    b.push(at(1.0, 0, 0, 355.f), 1.0);
    b.push(at(1.1, 0, 0, 725.f), 1.1); // 725 = 5 degrees: a 10-degree turn, not 370
    auto s = b.at(1.1 + 0.05);
    CHECK(s && std::abs(PlayBuffer::angleDelta(0.f, s->rotation) - 0.f) < 0.01f); // halfway: 0 degrees
}

TEST(play_buffer_stale_and_out_of_order) {
    PlayBuffer b;
    CHECK(!b.at(0));
    b.push(at(5.0, 10, 0), 1.0);
    b.push(at(4.9, 99, 0), 1.0); // late packet: ignored
    auto s = b.at(1.2);
    CHECK(s && s->x == 10.f);
    CHECK(!b.at(1.0 + PlayBuffer::kStale + 0.1)); // sender went silent: stop drawing
}

TEST(snapshot_direct_encoding_matches_and_is_fast) {
    ObjectStore st;
    st.header = "kA13,0,kA14,";
    for (uint32_t i = 1; i <= 50000; ++i) st.set(makeId(1, i), "1,1,2," + std::to_string(i * 30) + ",3,15,57,1.2.3,21,1004");
    std::vector<LockGroup> locks = {{2, {makeId(1, 5), makeId(1, 6)}}};
    msg::Snapshot s{42, st.header, {}, locks};
    for (auto const& [id, str] : st.objects) s.objects.emplace_back(id, str);
    CHECK(encodeSnapshot(42, st.header, st.objects, locks) == encode(s));
    auto t0 = std::chrono::steady_clock::now();
    auto bytes = encodeSnapshot(42, st.header, st.objects, locks);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  50k-object snapshot: %.1f MB encoded in %.1f ms\n", bytes.size() / 1e6, ms);
    auto d = decode(bytes);
    CHECK(d && std::get<msg::Snapshot>(*d).objects.size() == 50000);
}

TEST(play_state_rejects_non_finite) {
    auto ok = playing(1.f, 2.f, 7);
    auto d = decode(encode(ok));
    CHECK(d && std::get<msg::PlayState>(*d) == ok);
    auto bad = ok;
    bad.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!decode(encode(bad)));
    bad = ok;
    bad.mode = 9; // not a game mode
    CHECK(!decode(encode(bad)));
    bad = ok;
    bad.scale = 1000.f;
    CHECK(!decode(encode(bad)));
}

TEST(room_size_hard_capped_at_100) {
    HostCore h({1, "R", "H", 5000, ""}, {}, 0, 1);
    CHECK(h.config().maxPlayers == 100);
    HostCore small({1, "R", "H", 1, ""}, {}, 0, 1);
    CHECK(small.config().maxPlayers == 2);
    Net n({}, 100);
    for (int i = 0; i < 99; ++i) n.joinAccepted("P" + std::to_string(i));
    CHECK(n.host.activeCount() == 100);
    auto extra = n.connect("Extra");
    auto r = n.lastAction(extra, ClientCore::ActionKind::Rejected);
    CHECK(r && r->text == "Room is full");
    CHECK(n.clients.begin()->second.peers().size() == 100); // full roster fits in one PeerList
}

int main() {
    for (auto& [name, fn] : registry()) {
        int before = g_failed;
        fn();
        std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
