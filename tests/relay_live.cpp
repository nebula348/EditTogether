// Real-network test: two independent clients talk through the real Apinator service, authorised by a running
// relay auth service (ET_RELAY_URL, default http://127.0.0.1:8787 for `wrangler dev`; or the deployed Worker).
// Prints measured latencies. Anything it prints is measured, nothing is simulated.
#include "core/ClientCore.hpp"
#include "core/HostCore.hpp"
#include "net/ApinatorClient.hpp"
#include "net/RelayTransport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
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

template <class F>
static bool until(F pred, double seconds) {
    double end = now() + seconds;
    while (now() < end) {
        if (pred()) return true;
        nap();
    }
    return pred();
}

static void pctl(char const* what, std::vector<double> v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double sum = std::accumulate(v.begin(), v.end(), 0.0);
    std::printf("  %s: n=%zu min=%.0f ms median=%.0f ms avg=%.0f ms p95=%.0f ms max=%.0f ms\n", what, v.size(), v.front(),
                v[v.size() / 2], sum / static_cast<double>(v.size()), v[static_cast<size_t>(static_cast<double>(v.size()) * 0.95)], v.back());
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string url = std::getenv("ET_RELAY_URL") ? std::getenv("ET_RELAY_URL") : "http://127.0.0.1:8787";
    std::printf("relay auth service: %s\n", url.c_str());

    std::printf("room_creation\n");
    double t0 = now();
    RoomGrant grant = requestRoom(url);
    std::printf("  POST /v1/room took %.0f ms\n", (now() - t0) * 1000);
    CHECK(grant.error.empty());
    CHECK(validCode(grant.code));
    if (!grant.error.empty()) { std::printf("  error: %s\n", grant.error.c_str()); return 1; }
    std::printf("  room code %s\n", grant.code.c_str());

    ApinatorClient hostRelay({url});
    ApinatorClient playerRelay({url});
    ObjectStore seed;
    for (uint32_t i = 1; i <= 2000; ++i) seed.set(makeId(1, i), "1,1,2," + std::to_string(i) + ",3,15");

    double clock0 = now();
    auto clk = [&] { return now() - clock0; };
    HostCore host({77, "Live room", "Host", 4, ""}, seed, clk(), 9);
    ClientCore local;
    local.startLocal(1, seed, "Live room", clk());
    RelayHost transport(hostRelay, grant.code, grant.hostToken, clk);
    std::map<uint32_t, std::unique_ptr<Link>> conns;

    std::string secret = newSecret();
    auto link = connectToRoom(playerRelay, grant.code, secret, clk);
    link->setMaxFrame(kMaxFrameHostToClient);
    ClientCore player;
    player.start(77, "Ana", 0, clk());
    std::vector<ClientCore::Action> actions;
    auto has = [&](ClientCore::ActionKind k) {
        return std::any_of(actions.begin(), actions.end(), [&](auto const& a) { return a.kind == k; });
    };

    auto step = [&] {
        double t = clk();
        transport.pump();
        while (auto c = transport.accept()) {
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
            auto c = conns.find(o.peer);
            if (c != conns.end() && !o.bytes.empty()) c->second->send(o.bytes);
        }
        frames.clear();
        if (link && !link->update(frames)) {
            std::printf("  player link closed: %s\n", link->error().c_str());
            link.reset();
        }
        for (auto& f : frames) player.onMessage(f, t);
        player.tick(t);
        for (auto& m : player.takeOutbox())
            if (link) link->send(m);
        for (auto& a : player.takeActions()) actions.push_back(a);
        local.takeActions();
    };

    std::printf("join_over_internet\n");
    double joinStart = now();
    CHECK(until([&] { step(); return has(ClientCore::ActionKind::Pending); }, 30));
    std::printf("  join request reached the host after %.0f ms (WebSocket + auth + lobby + private channel)\n", (now() - joinStart) * 1000);
    CHECK(host.pendingCount() == 1);
    host.accept(host.peers().rbegin()->first, clk());
    double acceptAt = now();
    CHECK(until([&] { step(); return has(ClientCore::ActionKind::LoadSnapshot); }, 30));
    std::printf("  snapshot of %zu objects delivered %.0f ms after approval\n", seed.objects.size(), (now() - acceptAt) * 1000);
    CHECK(player.store.hash() == host.store().hash());

    std::printf("editing_round_trips\n");
    std::vector<double> toHost, toPlayer;
    for (int i = 1; i <= 40; ++i) {
        uint64_t seqBefore = host.seq();
        double a = now();
        player.submit({{OpKind::Create, makeId(player.me(), static_cast<uint32_t>(i)), "1,8,2," + std::to_string(i) + ",3,15"}});
        CHECK(until([&] { step(); return host.seq() > seqBefore; }, 10));
        toHost.push_back((now() - a) * 1000);
        // Host-side edit back to the player.
        uint64_t playerSeq = player.seq();
        double b = now();
        local.submit({{OpKind::Modify, makeId(1, static_cast<uint32_t>(i)), "1,1,2," + std::to_string(500 + i) + ",3,15"}});
        CHECK(until([&] { step(); return player.seq() > playerSeq; }, 10));
        toPlayer.push_back((now() - b) * 1000);
    }
    pctl("player -> host edit (one way, applied)", toHost);
    pctl("host -> player edit (one way, applied)", toPlayer);
    CHECK(until([&] { step(); return player.store.hash() == host.store().hash() && local.store.hash() == host.store().hash(); }, 10));
    CHECK(player.store.objects.size() == 2040);
    std::printf("  game-protocol round trip (ClientCore ping): %.0f ms\n", player.rttMs());
    std::printf("  Apinator WebSocket ping/pong: host %.0f ms, player %.0f ms\n", hostRelay.pingMs(), playerRelay.pingMs());

    std::printf("simultaneous_edits\n");
    for (int i = 0; i < 25; ++i) {
        player.submit({{OpKind::Create, makeId(player.me(), static_cast<uint32_t>(5000 + i)), "1,8,2,1,3,15"}});
        local.submit({{OpKind::Create, makeId(1, static_cast<uint32_t>(5000 + i)), "1,8,2,2,3,15"}});
    }
    CHECK(until([&] { step(); return host.seq() >= 40 + 50 && player.seq() == host.seq() && local.seq() == host.seq(); }, 20));
    CHECK(player.store.hash() == host.store().hash());
    CHECK(local.store.hash() == host.store().hash());
    CHECK(player.store.objects.size() == 2090);

    std::printf("reconnection_after_network_failure\n");
    {
        // The host's connection to Apinator breaks. Its player links fail, the player notices the silence, reconnects
        // with its token (no second approval) and gets the room state back.
        uint64_t seqBefore = host.seq();
        actions.clear();
        hostRelay.simulateDisconnect();
        double dropAt = now();
        CHECK(until([&] { step(); return has(ClientCore::ActionKind::ConnectionLost); }, 40));
        std::printf("  player noticed the failure after %.1f s\n", now() - dropAt);
        uint64_t token = player.token();
        link.reset();
        double reconnectAt = now();
        actions.clear();
        link = connectToRoom(playerRelay, grant.code, secret, clk);
        link->setMaxFrame(kMaxFrameHostToClient);
        player.start(77, "Ana", token, clk());
        CHECK(until([&] { step(); return has(ClientCore::ActionKind::LoadSnapshot); }, 40));
        std::printf("  back in the room %.0f ms after reconnecting, host relay reconnected: %s\n", (now() - reconnectAt) * 1000, hostRelay.connected() ? "yes" : "no");
        CHECK(!has(ClientCore::ActionKind::Pending));
        CHECK(player.store.hash() == host.store().hash());
        CHECK(host.seq() == seqBefore);
        CHECK(host.activeCount() == 2);
    }

    std::printf("unauthorised_access\n");
    {
        ApinatorClient spy({url});
        std::string victim = peerChannel(grant.code, jidOf(secret));
        spy.subscribe(victim, "\"jsecret\":\"" + newSecret() + "\"");
        std::vector<RelayEvent> events;
        bool refused = until([&] {
            spy.poll(events);
            return std::any_of(events.begin(), events.end(), [&](auto const& e) { return e.kind == RelayEvent::Kind::SubscribeFailed && e.channel == victim; });
        }, 15);
        CHECK(refused);
        bool subscribed = std::any_of(events.begin(), events.end(), [&](auto const& e) { return e.kind == RelayEvent::Kind::Subscribed && e.channel == victim; });
        CHECK(!subscribed);
        for (auto const& e : events)
            if (e.kind == RelayEvent::Kind::SubscribeFailed) std::printf("  refused: %s\n", e.data.c_str());
    }

    std::printf("\nApinator messages: host in/out %llu/%llu, player in/out %llu/%llu\n",
                static_cast<unsigned long long>(hostRelay.messagesIn()), static_cast<unsigned long long>(hostRelay.messagesOut()),
                static_cast<unsigned long long>(playerRelay.messagesIn()), static_cast<unsigned long long>(playerRelay.messagesOut()));
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
