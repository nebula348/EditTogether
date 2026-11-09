// EditTogether test bot: a scripted second player for testing with a single phone.
// Runs on the phone itself through adb (or any Linux box on the same Wi-Fi).
//   et_bot scan [seconds]                       list rooms announced on the network
//   et_bot client <ip> [port] [name] [seconds]  join a room, edit, play, print what others do
//   et_bot host [room] [name] [seconds]         host a room, accept joiners, edit, play
//   et_bot onlinehost <relayUrl> [room] [name] [seconds]   host through the Apinator relay and print the room code
//   et_bot onlineclient <relayUrl> <code> [name] [seconds] join an online room by its code
#include "core/Bytes.hpp"
#include "core/ClientCore.hpp"
#include "core/HostCore.hpp"
#include "net/ApinatorClient.hpp"
#include "net/Discovery.hpp"
#include "net/RelayTransport.hpp"
#include "net/Socket.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>

using namespace et;
using namespace et::net;

static double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static void nap() { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

static std::string at(int x, int y) { return ",2," + std::to_string(x) + ",3," + std::to_string(y); }

// One object of each kind we want to prove syncs: blocks, spikes, slopes, triggers, text, groups, transforms.
static std::vector<std::pair<char const*, std::string>> sampleObjects(int x0, int y0) {
    return {
        {"block", "1,1" + at(x0, y0)},
        {"spike", "1,8" + at(x0 + 30, y0)},
        {"slope", "1,289" + at(x0 + 60, y0)},
        {"rotated+scaled block", "1,1" + at(x0 + 90, y0 + 30) + ",6,45,128,1.5,129,1.5"},
        {"block in group 2", "1,1" + at(x0 + 120, y0) + ",57,2"},
        {"move trigger (group 2)", "1,901" + at(x0 + 150, y0 + 60) + ",51,2,28,60,10,1"},
        {"color trigger (ch 1 red)", "1,899" + at(x0 + 180, y0 + 60) + ",23,1,7,255,8,0,9,0,10,0.5"},
        {"text object", "1,914" + at(x0 + 210, y0 + 30) + ",31,SGVsbG8gZnJvbSB0aGUgYm90"},
    };
}

static void printOps(char const* who, std::vector<Op> const& ops) {
    for (auto const& op : ops) {
        char const* k = op.kind == OpKind::Create ? "CREATE" : op.kind == OpKind::Modify ? "MODIFY"
                      : op.kind == OpKind::Delete ? "DELETE" : "SETTINGS";
        std::printf("  [%s] %s id=%llx %.70s\n", who, k, (unsigned long long)op.id, op.data.c_str());
    }
}

// A player that jumps and spins, cycling through every game mode every 4 s.
static msg::PlayState fakePlayer(float x, double e) {
    msg::PlayState s;
    s.playing = true;
    s.time = static_cast<uint64_t>(now() * 1000.0);
    s.x = x;
    s.mode = static_cast<uint8_t>(static_cast<int>(e / 4.0) % 8);
    double hop = std::abs(std::sin(e * 3.0));
    s.y = 105.f + 60.f * static_cast<float>(hop);
    s.rotation = s.mode == 0 ? static_cast<float>(std::fmod(e * 360.0, 360.0)) : static_cast<float>(20.0 * std::cos(e * 3.0));
    s.icon = static_cast<uint16_t>(1 + static_cast<int>(e) % 30);
    s.cube = 25;
    s.color1 = 12;
    s.color2 = 9;
    s.glowColor = 3;
    s.flags = msg::PlayState::Glow;
    return s;
}

// Shared scripted behaviour for both modes, driven by elapsed time.
struct Script {
    ClientCore& me;
    bool playOnly = false;
    double start = 0;
    std::vector<uint64_t> mine;
    uint32_t counter = 0;
    int phase = 0;
    double lastMove = 0, lastPlay = 0;

    void step(double t) {
        double e = t - start;
        if (playOnly) {
            if (t - lastPlay < 1.0 / 15.0) return;
            if (phase == 0) { phase = 3; std::printf("bot: playing the level (slowly, from x=150), cycling game modes every 4 s\n"); }
            lastPlay = t;
            me.sendPlayState(fakePlayer(150.f + static_cast<float>(std::fmod(e * 60.0, 900.0)), e));
            return;
        }
        if (phase == 0 && e > 1.0) {
            phase = 1;
            std::printf("bot: creating sample objects at x=315..\n");
            std::vector<Op> ops;
            for (auto const& [name, str] : sampleObjects(315, 105)) {
                uint64_t id = makeId(me.me(), ++counter);
                mine.push_back(id);
                ops.push_back({OpKind::Create, id, str});
                std::printf("  + %s\n", name);
            }
            me.submit(ops);
        }
        if (phase == 1 && e > 3.0) {
            phase = 2;
            std::printf("bot: moving the first block for 20 s while holding its lock\n");
            me.setLocks({mine[0]});
        }
        if (phase == 2 && t - lastMove > 0.1) {
            lastMove = t;
            int x = 315 + static_cast<int>(120 * std::sin(e));
            int y = 165 + static_cast<int>(45 * std::cos(e));
            me.submit({{OpKind::Modify, mine[0], "1,1" + at(x, y)}});
            if (e > 23.0) {
                phase = 3;
                me.setLocks({});
                std::printf("bot: lock released; now 'playing' the level for 15 s\n");
            }
        }
        if (phase == 3 && t - lastPlay > 1.0 / 15.0) {
            lastPlay = t;
            float px = 150.f + static_cast<float>((e - 23.0) * 60.0); // slow, so it stays on screen
            me.sendPlayState(fakePlayer(px, e));
            if (e > 38.0) {
                phase = 4;
                me.sendPlayState(msg::PlayState{});
                std::printf("bot: stopped playing. Objects stay; edit freely, I'll print what arrives.\n");
            }
        }
    }
};

static void handle(ClientCore& c, char const* tag) {
    for (auto& a : c.takeActions()) {
        using K = ClientCore::ActionKind;
        switch (a.kind) {
            case K::Pending: std::printf("bot: waiting for the administrator to accept...\n"); break;
            case K::Joined: std::printf("bot: ACCEPTED as peer %u\n", c.me()); break;
            case K::Rejected: std::printf("bot: REJECTED: %s\n", a.text.c_str()); break;
            case K::Kicked: std::printf("bot: KICKED: %s\n", a.text.c_str()); break;
            case K::RoomClosed: std::printf("bot: ROOM CLOSED: %s\n", a.text.c_str()); break;
            case K::ConnectionLost: std::printf("bot: CONNECTION LOST: %s\n", a.text.c_str()); break;
            case K::LoadSnapshot: std::printf("bot: snapshot: %zu objects, seq %llu\n", c.store.objects.size(), (unsigned long long)c.seq()); break;
            case K::ApplyOps: if (!a.own) printOps(tag, a.ops); break;
            case K::Correction: std::printf("bot: correction (%s)\n", a.text.c_str()); printOps("correction", a.ops); break;
            case K::SyncError: std::printf("bot: SYNC ERROR: %s\n", a.text.c_str()); break;
            case K::PeersChanged:
                std::printf("bot: players:");
                for (auto const& p : c.peers()) std::printf(" %s(%s)", p.nickname.c_str(), roleName(p.role));
                std::printf("\n");
                break;
            case K::RoleChanged: std::printf("bot: my role is now %s\n", a.text.c_str()); break;
            case K::LocksChanged: break;
        }
    }
    static std::map<uint32_t, bool> wasPlaying;
    for (auto const& p : c.peers()) {
        bool playing = c.plays().count(p.id) > 0;
        if (playing != wasPlaying[p.id])
            std::printf("bot: %s %s playing\n", p.nickname.c_str(), playing ? "started" : "stopped");
        wasPlaying[p.id] = playing;
        static double lastPrint = 0;
        if (playing && now() - lastPrint > 0.5) {
            lastPrint = now();
            auto st = c.plays().at(p.id).at(now());
            if (!st) continue;
            auto const& s = *st;
            static char const* modes[] = {"cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"};
            std::printf("  %s: %s icon %u colors %u/%u%s at (%.0f, %.0f) rot %.0f size %.1f%s\n", p.nickname.c_str(),
                modes[s.mode], s.icon, s.color1, s.color2, (s.flags & msg::PlayState::Glow) ? " glow" : "", s.x, s.y,
                s.rotation, s.scale, (s.flags & msg::PlayState::UpsideDown) ? " upside-down" : "");
        }
    }
}

static int scan(double seconds) {
    RoomBrowser b;
    std::string err;
    if (!b.start(err)) { std::printf("cannot listen on UDP %u: %s\n", kDiscoveryPort, err.c_str()); return 1; }
    double end = now() + seconds;
    while (now() < end) { b.update(now()); nap(); }
    std::printf("%zu room(s), %llu announcements, %llu invalid\n", b.rooms().size(),
        (unsigned long long)b.received(), (unsigned long long)b.invalid());
    for (auto const& [id, r] : b.rooms())
        std::printf("  '%s' admin=%s players=%u/%u level='%s' at %s\n", r.info.roomName.c_str(), r.info.hostName.c_str(),
            r.info.players, r.info.maxPlayers, r.info.levelName.c_str(), r.host.str().c_str());
    return 0;
}

// Mirror mode: records the phone's real play stream, then replays it as our own with the original timing.
struct Mirror {
    bool enabled = false;
    std::vector<msg::PlayState> recording;
    double recordEnded = -1;
    size_t replayIndex = 0;
    double replayStart = 0;
    uint64_t firstTime = 0;
    bool replaying = false;

    void observe(std::string_view frame, double t) {
        if (!enabled || replaying) return;
        auto m = decode(frame);
        if (!m) return;
        auto ps = std::get_if<msg::PlayState>(&*m);
        if (!ps) return;
        if (ps->playing) {
            if (recording.empty()) std::printf("bot: recording %s's movement...\n", "the player");
            recording.push_back(*ps);
            recordEnded = -1;
        } else if (!recording.empty()) {
            recordEnded = t;
            std::printf("bot: recorded %zu samples (%.1f s); replaying them as myself in 2 s\n", recording.size(),
                (recording.back().time - recording.front().time) / 1000.0);
        }
    }

    void step(ClientCore& c, double t) {
        if (!enabled || recording.empty()) return;
        if (!replaying) {
            if (recordEnded < 0 || t - recordEnded < 2.0) return;
            replaying = true;
            replayIndex = 0;
            replayStart = t;
            firstTime = recording.front().time;
        }
        double elapsedMs = (t - replayStart) * 1000.0;
        while (replayIndex < recording.size() && recording[replayIndex].time - firstTime <= elapsedMs) {
            msg::PlayState s = recording[replayIndex++];
            s.peer = 0;
            s.time = static_cast<uint64_t>(t * 1000.0); // our clock now; spacing preserved by the loop above
            c.sendPlayState(s);
        }
        if (replayIndex >= recording.size()) {
            c.sendPlayState(msg::PlayState{});
            std::printf("bot: replay done. Play again to record a new run.\n");
            recording.clear();
            replaying = false;
            recordEnded = -1;
        }
    }
};

static int clientLoop(std::unique_ptr<Link> conn, std::string name, double seconds, bool playOnly, std::string password, bool mirrorMode) {
    conn->setMaxFrame(kMaxFrameHostToClient);
    ClientCore c;
    c.start(0, name, 0, now(), password);
    Script s{c, false, 0, {}};
    s.playOnly = playOnly;
    Mirror mirror;
    mirror.enabled = mirrorMode;
    double end = now() + seconds;
    bool started = false;
    while (now() < end) {
        double t = now();
        std::vector<std::string> frames;
        bool ok = conn->update(frames);
        for (auto& f : frames) {
            mirror.observe(f, t);
            c.onMessage(f, t);
        }
        c.tick(t);
        if (c.state() == ClientCore::State::Active && !c.awaitingSnapshot()) {
            if (!started) { started = true; s.start = t; }
            if (mirrorMode) mirror.step(c, t);
            else s.step(t);
        }
        for (auto& m : c.takeOutbox()) conn->send(m);
        handle(c, "phone");
        if (!ok) { std::printf("bot: transport closed: %s\n", conn->error().c_str()); return 1; }
        if (c.state() == ClientCore::State::Closed) return 1;
        nap();
    }
    std::printf("bot: done. Room has %zu objects at seq %llu\n", c.store.objects.size(), (unsigned long long)c.seq());
    c.leave();
    for (auto& m : c.takeOutbox()) conn->send(m);
    std::vector<std::string> ignored;
    for (int i = 0; i < 20 && !conn->outEmpty(); ++i) { conn->update(ignored); nap(); }
    return 0;
}

static int client(std::string ip, uint16_t port, std::string name, double seconds, bool playOnly, std::string password, bool mirrorMode = false) {
    auto addr = parseIPv4(ip);
    if (!addr) { std::printf("bad ip\n"); return 1; }
    std::string err;
    auto conn = TcpConn::connect({*addr, port}, err);
    if (!conn) { std::printf("connect: %s\n", err.c_str()); return 1; }
    return clientLoop(std::move(conn), name, seconds, playOnly, password, mirrorMode);
}

// Joins an online room by code through the relay.
static int onlineClient(std::string url, std::string code, std::string name, double seconds) {
    ApinatorClient relay({url});
    auto link = connectToRoom(relay, normalizeCode(code), newSecret(), now);
    std::printf("bot: joining online room %s through %s\n", normalizeCode(code).c_str(), url.c_str());
    return clientLoop(std::move(link), name, seconds, false, "", false);
}

static std::string g_onlineUrl; // set by onlinehost: also accept players through the relay

static int host(std::string room, std::string name, double seconds, uint16_t announceProtocol = kProtocolVersion) {
    TcpListener l;
    RoomAnnouncer ann;
    std::string err;
    if (!l.open(kDefaultTcpPort, err)) { std::printf("listen: %s\n", err.c_str()); return 1; }
    if (!ann.start(err)) std::printf("announce unavailable: %s\n", err.c_str());
    ObjectStore seed;
    seed.header = "kA13,0,kA15,0,kA16,0,kA14,,kA6,0,kA7,0,kA17,0,kA18,0,kA2,0,kA3,0,kA8,0,kA4,0,kA9,0,kA10,0,kA11,0";
    for (int i = 0; i < 10; ++i) seed.set(makeId(1, 1000 + i), "1,1" + at(15 + 30 * i, 15)); // a floor
    double t0 = now();
    HostCore h({0xB07, room, name, 10, ""}, seed, t0, static_cast<uint64_t>(t0 * 1e6));
    ClientCore local;
    local.startLocal(1, seed, room, t0);
    std::map<uint32_t, std::unique_ptr<Link>> conns;
    std::map<uint32_t, double> pendingSince;
    std::unique_ptr<ApinatorClient> relay;
    std::unique_ptr<RelayHost> relayHost;
    if (!g_onlineUrl.empty()) {
        auto grant = requestRoom(g_onlineUrl);
        if (!grant.error.empty()) { std::printf("online: %s\n", grant.error.c_str()); return 1; }
        relay = std::make_unique<ApinatorClient>(ApinatorClient::Config{g_onlineUrl});
        relayHost = std::make_unique<RelayHost>(*relay, grant.code, grant.hostToken, now);
        h.setPlayFlush(0.1);
        std::printf("ONLINE ROOM CODE: %s\n", grant.code.c_str());
    }
    Script s{local, false, 0, {}};
    bool started = false;
    std::printf("bot: hosting '%s' on port %u\n", room.c_str(), l.port());
    double end = t0 + seconds;
    while (now() < end) {
        double t = now();
        while (auto c = l.accept()) {
            c->setMaxFrame(kMaxFramePending);
            uint32_t id = h.onConnect(t);
            std::printf("bot: connection from %s\n", c->peer().str().c_str());
            conns[id] = std::move(c);
        }
        if (relayHost) {
            relayHost->pump();
            while (auto c = relayHost->accept()) {
                c->setMaxFrame(kMaxFramePending);
                uint32_t id = h.onConnect(t);
                std::printf("bot: online connection (%s)\n", c->describe().c_str());
                conns[id] = std::move(c);
            }
        }
        std::vector<std::string> frames;
        for (auto it = conns.begin(); it != conns.end();) {
            frames.clear();
            bool ok = it->second->update(frames);
            for (auto& f : frames) h.onMessage(it->first, f, t);
            if (!ok) { h.onDisconnect(it->first, it->second->error()); it = conns.erase(it); continue; }
            ++it;
        }
        for (auto const& e : h.takeEvents()) {
            if (e.kind == HostCore::EventKind::JoinRequest) {
                std::printf("bot: join request from %s (accepting in 3 s)\n", e.nickname.c_str());
                pendingSince[e.peer] = t;
            } else if (e.kind == HostCore::EventKind::PeerJoined) {
                std::printf("bot: %s joined\n", e.nickname.c_str());
                if (!started) { started = true; s.start = t; }
            } else if (e.kind == HostCore::EventKind::PeerLeft) {
                std::printf("bot: %s left (%s)\n", e.nickname.c_str(), e.detail.c_str());
            }
        }
        for (auto it = pendingSince.begin(); it != pendingSince.end();) {
            if (t - it->second < 3.0) { ++it; continue; }
            h.accept(it->first, t);
            it = pendingSince.erase(it);
        }
        if (started) s.step(t);
        h.tick(t);
        local.tick(t);
        for (int i = 0; i < 8; ++i) {
            bool any = false;
            for (auto& m : local.takeOutbox()) { any = true; h.onMessage(1, m, t); }
            for (auto& o : h.takeOutbox()) {
                any = true;
                if (o.peer == 1) { if (!o.bytes.empty()) local.onMessage(o.bytes, t); continue; }
                auto c = conns.find(o.peer);
                if (c != conns.end() && !o.bytes.empty()) c->second->send(o.bytes);
            }
            if (!any) break;
        }
        handle(local, "phone");
        for (auto const& [id, p] : h.peers())
            if (p.active) {
                auto c = conns.find(id);
                if (c != conns.end()) c->second->setMaxFrame(kMaxFrameClientToHost);
            }
        Announce a{announceProtocol, 0xB07, l.port(), static_cast<uint8_t>(h.activeCount()), 10, true, false, room, name, "Bot Level"};
        ann.update(t, a);
        nap();
    }
    h.close("The bot finished");
    std::printf("bot: done. Room has %zu objects at seq %llu\n", h.store().objects.size(), (unsigned long long)h.seq());
    return 0;
}

// Joins, then drops the connection without saying goodbye (like a Wi-Fi blip) and reconnects with its token
// before the host notices. Expected: back in without a new approval.
static int reconnectTest(std::string ip, uint16_t port, std::string name, std::string password) {
    auto addr = parseIPv4(ip);
    if (!addr) return 1;
    std::string err;
    auto conn = TcpConn::connect({*addr, port}, err);
    if (!conn) { std::printf("connect: %s\n", err.c_str()); return 1; }
    conn->setMaxFrame(kMaxFrameHostToClient);
    ClientCore c;
    c.start(0, name, 0, now(), password);
    double joinedAt = -1, end = now() + 180;
    bool dropped = false, sawPending = false;
    while (now() < end) {
        double t = now();
        std::vector<std::string> frames;
        if (conn && !conn->update(frames)) { std::printf("bot: transport closed: %s\n", conn->error().c_str()); return 1; }
        for (auto& f : frames) c.onMessage(f, t);
        c.tick(t);
        if (conn) for (auto& m : c.takeOutbox()) conn->send(m);
        for (auto& a : c.takeActions()) {
            using K = ClientCore::ActionKind;
            if (a.kind == K::Pending) {
                sawPending = true;
                std::printf("bot: %s\n", dropped ? "PENDING again after reconnecting (approval asked twice)" : "waiting for approval...");
            }
            if (a.kind == K::Joined) {
                std::printf("bot: %s as peer %u\n", dropped ? "RECONNECTED" : "accepted", c.me());
                if (dropped) {
                    std::printf("bot: result: %s\n", sawPending ? "FAIL (asked for approval again)" : "OK (no new approval, role kept)");
                    std::printf("bot: my role is %s\n", roleName(c.role()));
                    return sawPending ? 1 : 0;
                }
                joinedAt = t;
                sawPending = false;
            }
            if (a.kind == K::Rejected || a.kind == K::Kicked) { std::printf("bot: %s\n", a.text.c_str()); return 1; }
        }
        if (!dropped && joinedAt > 0 && t - joinedAt > 4.0) {
            uint64_t token = c.token();
            std::printf("bot: dropping the connection without goodbye, reconnecting with my token...\n");
            conn.reset(); // the host's side of the old socket closes only when it notices
            dropped = true;
            conn = TcpConn::connect({*addr, port}, err);
            if (!conn) { std::printf("reconnect: %s\n", err.c_str()); return 1; }
            conn->setMaxFrame(kMaxFrameHostToClient);
            c.start(0, name, token, t, password);
        }
        nap();
    }
    return 1;
}

// Sends a Hello in an older protocol layout and prints what the host answers.
static int oldVersionTest(std::string ip, uint16_t port) {
    auto addr = parseIPv4(ip);
    std::string err;
    auto conn = addr ? TcpConn::connect({*addr, port}, err) : nullptr;
    if (!conn) { std::printf("connect failed\n"); return 1; }
    conn->setMaxFrame(kMaxFrameHostToClient);
    ByteWriter w; // protocol 4 Hello: no password field
    w.u8(1);
    w.u16(4);
    w.u64(0);
    w.str("OldVersionBot");
    w.u64(0);
    conn->send(w.buf);
    double end = now() + 10;
    while (now() < end) {
        std::vector<std::string> frames;
        bool ok = conn->update(frames);
        for (auto& f : frames) {
            auto m = decode(f);
            if (m && std::holds_alternative<msg::JoinRejected>(*m)) { std::printf("host answered: %s\n", std::get<msg::JoinRejected>(*m).reason.c_str()); return 0; }
            if (m && std::holds_alternative<msg::Kicked>(*m)) { std::printf("host kicked: %s\n", std::get<msg::Kicked>(*m).reason.c_str()); return 1; }
        }
        if (!ok) { std::printf("closed: %s\n", conn->error().c_str()); return 1; }
        nap();
    }
    std::printf("no answer\n");
    return 1;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string mode = argc > 1 ? argv[1] : "";
    auto arg = [&](int i, char const* def) { return std::string(argc > i ? argv[i] : def); };
    if (mode == "scan") return scan(std::atof(arg(2, "5").c_str()));
    if (mode == "onlineclient") return onlineClient(arg(2, ""), arg(3, ""), arg(4, "OnlineBot"), std::atof(arg(5, "120").c_str()));
    if (mode == "onlinehost") {
        g_onlineUrl = arg(2, "");
        return host(arg(3, "Online Bot Room"), arg(4, "OnlineHostBot"), std::atof(arg(5, "300").c_str()));
    }
    if (mode == "mirror")
        return client(arg(2, "127.0.0.1"), static_cast<uint16_t>(std::atoi(arg(3, "47812").c_str())), arg(4, "Mirror"),
            std::atof(arg(5, "600").c_str()), false, arg(6, ""), true);
    if (mode == "client" || mode == "player")
        return client(arg(2, "127.0.0.1"), static_cast<uint16_t>(std::atoi(arg(3, "47812").c_str())), arg(4, "TestBot"),
            std::atof(arg(5, "120").c_str()), mode == "player", arg(6, ""));
    if (mode == "host")
        return host(arg(2, "Bot Room"), arg(3, "TestBot"), std::atof(arg(4, "180").c_str()),
            static_cast<uint16_t>(std::atoi(arg(5, std::to_string(kProtocolVersion).c_str()).c_str())));
    if (mode == "reconnect")
        return reconnectTest(arg(2, "127.0.0.1"), static_cast<uint16_t>(std::atoi(arg(3, "47812").c_str())), arg(4, "Blip"), arg(5, ""));
    if (mode == "oldversion") return oldVersionTest(arg(2, "127.0.0.1"), static_cast<uint16_t>(std::atoi(arg(3, "47812").c_str())));
    std::printf("usage: et_bot scan [s] | client|player|mirror <ip> [port] [name] [s] [password] | host [room] [name] [s]\n");
    return 2;
}
