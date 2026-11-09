// Real-socket tests: TCP framing, UDP discovery, and a full host <-> client session over loopback.
// POSIX only (Linux/WSL or Android via adb; see tests/README.md).
#include "core/ClientCore.hpp"
#include "core/HostCore.hpp"
#include "net/ChunkLink.hpp"
#include "net/Discovery.hpp"
#include "net/Socket.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace et;
using namespace et::net;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static void nap() { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
constexpr uint32_t kLoopback = 0x7F000001;

// Polls until pred() or timeout.
template <class F>
static bool until(F pred, double seconds = 3.0) {
    double end = now() + seconds;
    while (now() < end) {
        if (pred()) return true;
        nap();
    }
    return pred();
}

static void tcp_framing() {
    std::printf("tcp_framing\n");
    TcpListener l;
    std::string err;
    CHECK(l.open(0, err));
    auto client = TcpConn::connect({kLoopback, l.port()}, err);
    CHECK(client != nullptr);
    std::unique_ptr<TcpConn> server;
    CHECK(until([&] { if (!server) server = l.accept(); return server != nullptr; }));
    if (!client || !server) return;
    server->setMaxFrame(4 * 1024 * 1024);

    std::string big(3 * 1024 * 1024, 'x');
    client->send("");
    client->send("hello");
    client->send(big);
    std::vector<std::string> got, scratch;
    CHECK(until([&] {
        client->update(scratch);
        server->update(got);
        return got.size() >= 3;
    }));
    CHECK(got.size() == 3 && got[0].empty() && got[1] == "hello" && got[2] == big);

    // A frame announcing more than the limit kills the connection.
    server->setMaxFrame(16);
    client->send(std::string(17, 'y'));
    bool alive = true;
    until([&] {
        client->update(scratch);
        alive = server->update(got);
        return !alive;
    });
    CHECK(!alive);
    CHECK(server->error() == "Packet exceeds size limit");
}

static void tcp_last_frame_before_close_is_delivered() {
    std::printf("tcp_last_frame_before_close_is_delivered\n");
    TcpListener l;
    std::string err;
    CHECK(l.open(0, err));
    auto client = TcpConn::connect({kLoopback, l.port()}, err);
    std::unique_ptr<TcpConn> server;
    until([&] { if (!server) server = l.accept(); return server != nullptr; });
    if (!client || !server) return;
    client->setMaxFrame(1024);
    std::vector<std::string> got;
    until([&] { return client->update(got) && client->connected(); });
    server->send("goodbye");
    until([&] { server->update(got); return server->outEmpty(); });
    server.reset(); // close right after sending, like Kicked/RoomClosed
    bool alive = true;
    until([&] { alive = client->update(got); return !alive; });
    CHECK(!alive);
    CHECK(got.size() == 1 && got[0] == "goodbye");
}

static void connect_refused_reports_error() {
    std::printf("connect_refused_reports_error\n");
    TcpListener l;
    std::string err;
    l.open(0, err);
    uint16_t port = l.port();
    l = TcpListener(); // nothing listens there now
    auto c = TcpConn::connect({kLoopback, port}, err);
    if (!c) { CHECK(!err.empty()); return; }
    std::vector<std::string> f;
    bool alive = true;
    until([&] { alive = c->update(f); return !alive; });
    CHECK(!alive);
    CHECK(!c->error().empty());
}

static void udp_discovery() {
    std::printf("udp_discovery\n");
    RoomBrowser browser;
    std::string err;
    CHECK(browser.start(err));
    UdpSocket sender;
    CHECK(sender.open(0, false, err));
    Announce a{kProtocolVersion, 777, 47812, 1, 4, true, false, "Test Room", "Host", "Level"};
    sender.sendTo({kLoopback, kDiscoveryPort}, encodeAnnounce(a));
    sender.sendTo({kLoopback, kDiscoveryPort}, "garbage");
    CHECK(until([&] { browser.update(now()); return browser.rooms().count(777) == 1; }));
    if (browser.rooms().count(777)) {
        auto const& r = browser.rooms().at(777);
        CHECK(r.info == a);
        CHECK(r.host.ip == kLoopback && r.host.port == 47812);
    }
    CHECK(browser.invalid() >= 1);
    // Expiry
    double later = now() + RoomBrowser::kExpiry + 1;
    browser.update(later);
    CHECK(browser.rooms().empty());

    // Real broadcast path (needs a network interface with broadcast, e.g. Wi-Fi).
    if (localInterfaces().empty()) {
        std::printf("  SKIP broadcast: no non-loopback IPv4 interface\n");
        return;
    }
    RoomAnnouncer ann;
    CHECK(ann.start(err));
    a.roomId = 888;
    ann.update(now(), a);
    bool seen = until([&] { browser.update(now()); return browser.rooms().count(888) == 1; }, 2.0);
    if (!seen) std::printf("  NOTE broadcast not looped back on this host (normal on some networks)\n");
    CHECK(ann.sent() >= 1);
}

// Host and client cores wired through real TCP sockets, like Session does.
static void end_to_end_session() {
    std::printf("end_to_end_session\n");
    TcpListener l;
    std::string err;
    CHECK(l.open(0, err));
    ObjectStore seed;
    seed.set(makeId(1, 1), "1,1,2,15,3,15");
    double t0 = now();
    HostCore host({55, "Room", "Host", 4, ""}, seed, t0, 7);
    ClientCore local;
    local.startLocal(1, seed, "Room", t0);
    std::map<uint32_t, std::unique_ptr<TcpConn>> conns;

    auto conn = TcpConn::connect({kLoopback, l.port()}, err);
    conn->setMaxFrame(kMaxFrameHostToClient);
    ClientCore client;
    client.start(55, "Ana", 0, t0);
    std::vector<ClientCore::Action> actions;

    auto step = [&] {
        double t = now();
        while (auto c = l.accept()) {
            c->setMaxFrame(kMaxFramePending);
            conns[host.onConnect(t)] = std::move(c);
        }
        std::vector<std::string> frames;
        for (auto it = conns.begin(); it != conns.end();) {
            frames.clear();
            bool ok = it->second->update(frames);
            for (auto& f : frames) host.onMessage(it->first, f, t);
            if (!ok) { host.onDisconnect(it->first, "closed"); it = conns.erase(it); continue; }
            if (host.peers().count(it->first) && host.peers().at(it->first).active) it->second->setMaxFrame(kMaxFrameClientToHost);
            ++it;
        }
        host.tick(t);
        for (auto& m : local.takeOutbox()) host.onMessage(1, m, t);
        for (auto& o : host.takeOutbox()) {
            if (o.peer == 1) { if (!o.bytes.empty()) local.onMessage(o.bytes, t); continue; }
            if (conns.count(o.peer) && !o.bytes.empty()) conns[o.peer]->send(o.bytes);
        }
        frames.clear();
        if (conn) {
            conn->update(frames);
            for (auto& f : frames) client.onMessage(f, t);
            client.tick(t);
            for (auto& m : client.takeOutbox()) conn->send(m);
        }
        for (auto& a : client.takeActions()) actions.push_back(a);
        local.takeActions();
    };
    auto has = [&](ClientCore::ActionKind k) {
        for (auto& a : actions) if (a.kind == k) return true;
        return false;
    };

    CHECK(until([&] { step(); return has(ClientCore::ActionKind::Pending); }));
    CHECK(host.pendingCount() == 1);
    uint32_t pid = host.peers().rbegin()->first;
    host.accept(pid, now());
    CHECK(until([&] { step(); return has(ClientCore::ActionKind::LoadSnapshot); }));
    CHECK(client.store.hash() == host.store().hash());

    client.submit({{OpKind::Create, makeId(client.me(), 1), "1,8,2,30,3,15"}});
    local.submit({{OpKind::Modify, makeId(1, 1), "1,1,2,45,3,15"}});
    CHECK(until([&] { step(); return client.seq() == 2 && local.seq() == 2; }));
    CHECK(client.store.hash() == host.store().hash());
    CHECK(local.store.hash() == host.store().hash());

    // Drop the connection; the host releases the peer, the client reconnects with its token.
    uint64_t token = client.token();
    conn.reset();
    CHECK(until([&] { step(); return host.activeCount() == 1; }));
    conn = TcpConn::connect({kLoopback, l.port()}, err);
    conn->setMaxFrame(kMaxFrameHostToClient);
    actions.clear();
    client.start(55, "Ana", token, now());
    CHECK(until([&] { step(); return has(ClientCore::ActionKind::LoadSnapshot); }));
    CHECK(!has(ClientCore::ActionKind::Pending)); // no second approval
    CHECK(client.store.hash() == host.store().hash());
    CHECK(client.store.objects.size() == 2);
}

// A relay that reorders and duplicates messages, like a store-and-forward server may.
static void chunk_link_over_messy_relay() {
    std::printf("chunk_link_over_messy_relay\n");
    double clock = 0;
    std::vector<std::string> wireAB, wireBA;
    ChunkLink a(77, "A", [&](std::string b) { wireAB.push_back(std::move(b)); }, [&] { return clock; }, 1e9);
    ChunkLink b(77, "B", [&](std::string s) { wireBA.push_back(std::move(s)); }, [&] { return clock; }, 1e9);
    a.setMaxFrame(16 * 1024 * 1024);
    b.setMaxFrame(16 * 1024 * 1024);

    std::string big;
    for (int i = 0; i < 200000; ++i) big += "1,1,2," + std::to_string(i % 977) + ",3,15;"; // ~3 MB, compressible
    std::string noise(20000, '\0');
    uint32_t x = 1;
    for (auto& ch : noise) { x = x * 1103515245 + 12345; ch = static_cast<char>(x >> 16); } // incompressible
    std::vector<std::string> sent = {"", "hello", big, noise, std::string(8000, 'z'), "last"};
    for (auto const& s : sent) a.send(s);
    std::vector<std::string> got, ignored;
    a.update(ignored);
    CHECK(wireAB.size() > 6);
    size_t bigChunks = wireAB.size();
    // Deliver in a scrambled order with duplicates.
    std::vector<std::string> scrambled = wireAB;
    for (size_t i = 0; i + 1 < scrambled.size(); i += 2) std::swap(scrambled[i], scrambled[i + 1]);
    scrambled.push_back(wireAB[0]);
    scrambled.push_back(wireAB[wireAB.size() / 2]);
    for (auto const& m : scrambled) {
        auto c = decodeChunk(m);
        CHECK(c.has_value());
        if (c) b.deliver(*c);
    }
    CHECK(b.update(got));
    CHECK(got == sent);
    CHECK(bigChunks < 3 * 1024 * 1024 / ChunkLink::kChunkSize); // compression kicked in

    // Size limit applies to the decompressed frame, so a tiny bomb can't blow up memory.
    auto bomb = compressFrame(std::string(50 * 1024 * 1024, 'a'));
    CHECK(bomb.size() < 200 * 1024);
    CHECK(!decompressFrame(bomb, 1024 * 1024));
    ChunkLink small(5, "S", [](std::string) {}, [&] { return clock; }, 1e9);
    small.setMaxFrame(1000);
    Chunk c{5, 0, Chunk::End | Chunk::Compressed, bomb.substr(0, 8000)};
    small.deliver(c);
    CHECK(!small.update(ignored));

    // A gap that never fills breaks the link instead of stalling forever.
    ChunkLink gap(9, "G", [](std::string) {}, [&] { return clock; }, 1e9);
    gap.deliver({9, 1, Chunk::End, "x"});
    CHECK(gap.update(ignored));
    clock += ChunkLink::kReorderTimeout + 1;
    CHECK(!gap.update(ignored));

    // Chunks of another connection (epoch) are ignored; garbage doesn't decode.
    CHECK(!decodeChunk("abc"));
    CHECK(!decodeChunk(std::string(9, '\0') + std::string(9000, 'x')));
}

static void chunk_link_rate_limit() {
    std::printf("chunk_link_rate_limit\n");
    double clock = 0;
    size_t sentBytes = 0;
    ChunkLink a(1, "A", [&](std::string b) { sentBytes += b.size(); }, [&] { return clock; }, 64 * 1024);
    std::string noise(512 * 1024, '\0');
    uint32_t x = 7;
    for (auto& ch : noise) { x = x * 1103515245 + 12345; ch = static_cast<char>(x >> 16); }
    a.send(noise);
    std::vector<std::string> ignored;
    a.update(ignored);
    CHECK(sentBytes <= 64 * 1024 + 9); // first second's budget only
    CHECK(!a.outEmpty());
    for (int i = 0; i < 10; ++i) { clock += 1.0; a.update(ignored); }
    CHECK(a.outEmpty());
    CHECK(sentBytes >= noise.size());
}

int main() {
    chunk_link_over_messy_relay();
    chunk_link_rate_limit();
    tcp_framing();
    tcp_last_frame_before_close_is_delivered();
    connect_refused_reports_error();
    udp_discovery();
    end_to_end_session();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
