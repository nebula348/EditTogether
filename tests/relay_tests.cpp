// Online transport tests that need no network: JSON, WebSocket framing, HTTP parsing, and a whole host and
// player session over an in-memory relay that follows Apinator's rules and the auth service's access rules.
// The version against the real Apinator is relay_live.cpp.
#include "core/ClientCore.hpp"
#include "core/HostCore.hpp"
#include "net/Http.hpp"
#include "net/Json.hpp"
#include "net/RelayTransport.hpp"
#include "net/WebSocket.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <set>

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

// ------------------------------------------------------------------ pure pieces

static void json_parsing() {
    std::printf("json_parsing\n");
    auto o = json::parseObject(R"({"event":"client-c","channel":"private-et-ABC-l","data":"{\"b\":\"AA==\"}","n":12.5,"ok":true,"x":{"a":[1,2,{"b":"}"}]}})");
    CHECK(o && json::str(*o, "event") == "client-c");
    CHECK(o && json::str(*o, "data") == R"({"b":"AA=="})");
    CHECK(o && json::num(*o, "n") == 12.5);
    CHECK(o && json::str(*o, "ok").empty() && json::num(*o, "missing", 7) == 7);
    CHECK(o && o->at("x").text == R"({"a":[1,2,{"b":"}"}]})");
    auto inner = json::parseObject(json::str(*o, "data"));
    CHECK(inner && json::str(*inner, "b") == "AA==");

    auto u = json::parseObject(R"({"s":"aé😀\n\"q\"\\"})");
    CHECK(u && json::str(*u, "s") == "a\xC3\xA9\xF0\x9F\x98\x80\n\"q\"\\");
    CHECK(json::parseObject("{}") && json::parseObject("  { }  "));
    for (char const* bad : {"", "{", "[]", R"({"a":})", R"({"a":1,})", R"({"a" 1})", R"({"a":"x)", R"({"a":1} x)", R"({"a":"\x"})",
                            R"({"a":"\ud800"})", "{\"a\":\"\x01\"}"})
        CHECK(!json::parseObject(bad));
    std::string deep = "{\"a\":" + std::string(40, '[') + std::string(40, ']') + "}";
    CHECK(!json::parseObject(deep)); // depth limit

    std::string raw = std::string("a\"b\\c\n\x01") + "z";
    auto back = json::parseObject("{\"k\":\"" + json::escape(raw) + "\"}");
    CHECK(back && json::str(*back, "k") == raw);
}

static void websocket_framing() {
    std::printf("websocket_framing\n");
    // RFC 6455 section 1.3 example.
    CHECK(ws::acceptFor("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(ws::base64("Man") == "TWFu" && ws::base64("Ma") == "TWE=");
    bool ok = false;
    CHECK(ws::base64Decode("TWFu", ok) == "Man" && ok);
    ws::base64Decode("not base64!", ok);
    CHECK(!ok);

    // A masked client frame must unmask to the payload (RFC example "Hello" with key 37 fa 21 3d).
    std::string f = ws::encodeFrame(ws::Text, "Hello", 0x3d21fa37);
    CHECK(f.size() == 11 && static_cast<uint8_t>(f[0]) == 0x81 && static_cast<uint8_t>(f[1]) == 0x85);
    CHECK(static_cast<uint8_t>(f[6]) == 0x7f && static_cast<uint8_t>(f[7]) == 0x9f && static_cast<uint8_t>(f[8]) == 0x4d);
    std::string big(70000, 'x');
    std::string bf = ws::encodeFrame(ws::Binary, big, 1);
    CHECK(static_cast<uint8_t>(bf[1]) == 0xFF && bf.size() == 70000 + 14);

    // Server frames: unmasked, fragmented, with a ping in the middle.
    auto server = [](uint8_t b0, std::string const& payload) {
        std::string s;
        s.push_back(static_cast<char>(b0));
        s.push_back(static_cast<char>(payload.size()));
        return s + payload;
    };
    ws::Reader r;
    std::string stream = server(0x01, "Hel") + server(0x89, "p") + server(0x80, "lo") + server(0x81, "x") + server(0x8A, "");
    for (char c : stream) r.feed(&c, 1); // one byte at a time
    std::vector<ws::Message> got;
    while (auto m = r.next()) got.push_back(*m);
    CHECK(got.size() == 4);
    CHECK(got.size() == 4 && got[0].op == ws::Ping && got[0].data == "p");
    CHECK(got.size() == 4 && got[1].op == ws::Text && got[1].data == "Hello");
    CHECK(got.size() == 4 && got[2].op == ws::Text && got[2].data == "x");
    CHECK(got.size() == 4 && got[3].op == ws::Pong);
    CHECK(r.error().empty());

    ws::Reader masked;
    std::string m = "\x81\x81" "abcd" "z";
    masked.feed(m.data(), m.size());
    CHECK(!masked.next() && !masked.error().empty());
    ws::Reader orphan;
    std::string o = server(0x80, "x");
    orphan.feed(o.data(), o.size());
    CHECK(!orphan.next() && !orphan.error().empty());
    ws::Reader huge;
    std::string h = "\x82\x7F";
    h.append(std::string("\x00\x00\x00\x00\x10\x00\x00\x00", 8)); // 256 MB
    huge.feed(h.data(), h.size());
    CHECK(!huge.next() && !huge.error().empty());

    // Handshake validation.
    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nsec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\nEXTRA";
    std::string err;
    auto n = ws::checkHandshakeResponse(reply, key, err);
    CHECK(n && *n == reply.size() - 5);
    CHECK(ws::checkHandshakeResponse(reply.substr(0, 30), key, err) == std::optional<size_t>(0));
    CHECK(!ws::checkHandshakeResponse("HTTP/1.1 403 Forbidden\r\n\r\n", key, err));
    CHECK(!ws::checkHandshakeResponse("HTTP/1.1 101 x\r\nSec-WebSocket-Accept: wrong\r\n\r\n", key, err));
}

static void http_parsing() {
    std::printf("http_parsing\n");
    auto u = parseUrl("https://relay.example.workers.dev/");
    CHECK(u && u->tls && u->host == "relay.example.workers.dev" && u->port == 443 && u->base.empty());
    auto l = parseUrl("http://localhost:8787/api/");
    CHECK(l && !l->tls && l->port == 8787 && l->base == "/api");
    CHECK(!parseUrl("ftp://x") && !parseUrl("https://") && !parseUrl("http://h:99999"));

    std::string out;
    size_t used = 0;
    std::string body = "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nTrailer: x\r\n\r\nNEXT";
    CHECK(decodeChunked(body, out, &used) && out == "hello world" && used == body.size() - 4);
    CHECK(!decodeChunked("5\r\nhel", out));
    CHECK(!decodeChunked("zz\r\nabc\r\n0\r\n\r\n", out));
    CHECK(!decodeChunked("3\r\nabcXX0\r\n\r\n", out));
    CHECK(!decodeChunked("FFFFFFFF\r\n", out));
}

// ------------------------------------------------------------------ in-memory relay

static double g_clock = 0;
static double clk() { return g_clock; }

struct FakeRelay;

struct FakeHub {
    std::string secretHostToken(std::string const& code) const { return "tok-" + code; }
    std::vector<FakeRelay*> endpoints;
    bool duplicate = false; // deliver every message twice
    bool reverse = false;   // deliver each batch in reverse order
    uint64_t delivered = 0;
    void step();
};

// Mirrors server/src/index.js authorize().
static bool allowed(FakeHub const& hub, std::string const& channel, std::string const& authFields) {
    std::string prefix = "private-et-";
    if (channel.rfind(prefix, 0) != 0 || channel.size() < prefix.size() + 10) return false;
    std::string code = channel.substr(prefix.size(), 8);
    std::string kind = channel.substr(prefix.size() + 9);
    if (kind == "l") return true;
    auto obj = json::parseObject("{" + authFields + "}");
    if (!obj) return false;
    if (json::str(*obj, "hostToken") == hub.secretHostToken(code)) return true;
    std::string secret = json::str(*obj, "jsecret");
    return !secret.empty() && kind == "p" + jidOf(secret);
}

struct FakeRelay final : Relay {
    FakeHub& hub;
    struct Op { int kind; std::string channel, a, b; }; // 0 subscribe, 1 unsubscribe, 2 publish
    std::deque<Op> ops;
    std::vector<RelayEvent> inbox;
    std::map<std::string, std::string> wanted;
    std::set<std::string> subscribed;
    bool online = true;
    uint64_t published = 0;

    explicit FakeRelay(FakeHub& h) : hub(h) { h.endpoints.push_back(this); }
    ~FakeRelay() override { hub.endpoints.erase(std::find(hub.endpoints.begin(), hub.endpoints.end(), this)); }

    void subscribe(std::string c, std::string f) override { ops.push_back({0, std::move(c), std::move(f), {}}); }
    void unsubscribe(std::string c) override { ops.push_back({1, std::move(c), {}, {}}); }
    void publish(std::string c, std::string e, std::string d) override { ops.push_back({2, std::move(c), std::move(e), std::move(d)}); }
    void poll(std::vector<RelayEvent>& out) override {
        for (auto& e : inbox) out.push_back(std::move(e));
        inbox.clear();
    }
    bool connected() const override { return online; }

    void drop() { // network loss: subscriptions vanish, the client library will resubscribe
        online = false;
        subscribed.clear();
        ops.clear();
        inbox.push_back({RelayEvent::Kind::Disconnected, {}, {}, "network"});
    }
    void restore() {
        online = true;
        inbox.push_back({RelayEvent::Kind::Connected, {}, {}, {}});
        for (auto const& [c, f] : wanted) ops.push_back({0, c, f, {}});
    }
};

void FakeHub::step() {
    std::vector<std::pair<FakeRelay*, RelayEvent>> deliveries;
    for (auto* ep : endpoints) {
        auto ops = std::move(ep->ops);
        ep->ops.clear();
        for (auto& op : ops) {
            if (op.kind == 0) {
                if (!allowed(*this, op.channel, op.a)) {
                    ep->inbox.push_back({RelayEvent::Kind::SubscribeFailed, op.channel, {}, "not allowed"});
                    continue;
                }
                ep->wanted[op.channel] = op.a;
                ep->subscribed.insert(op.channel);
                ep->inbox.push_back({RelayEvent::Kind::Subscribed, op.channel, {}, {}});
            } else if (op.kind == 1) {
                ep->wanted.erase(op.channel);
                ep->subscribed.erase(op.channel);
            } else if (ep->online && ep->subscribed.count(op.channel)) { // Apinator refuses publishes to unsubscribed channels
                ++ep->published;
                for (auto* other : endpoints)
                    if (other != ep && other->online && other->subscribed.count(op.channel))
                        deliveries.push_back({other, {RelayEvent::Kind::Message, op.channel, op.a, op.b}});
            }
        }
    }
    if (reverse) std::reverse(deliveries.begin(), deliveries.end());
    for (auto& [ep, ev] : deliveries) {
        ++delivered;
        ep->inbox.push_back(ev);
        if (duplicate) ep->inbox.push_back(ev);
    }
}

// ------------------------------------------------------------------ a whole room over the fake relay

struct Rig {
    FakeHub hub;
    FakeRelay hostRelay{hub};
    std::string code = "ABCD2345";
    ObjectStore seed;
    std::unique_ptr<HostCore> host;
    ClientCore local;
    std::unique_ptr<RelayHost> transport;
    std::map<uint32_t, std::unique_ptr<Link>> conns;

    struct Player {
        std::unique_ptr<FakeRelay> relay;
        std::string secret = newSecret();
        std::unique_ptr<Link> link;
        ClientCore core;
        std::vector<ClientCore::Action> actions;
        uint64_t token = 0;
        bool has(ClientCore::ActionKind k) const {
            return std::any_of(actions.begin(), actions.end(), [&](auto const& a) { return a.kind == k; });
        }
    };
    std::vector<std::unique_ptr<Player>> players;

    explicit Rig(int objects = 1, std::string password = {}) {
        for (int i = 1; i <= objects; ++i) seed.set(makeId(1, static_cast<uint32_t>(i)), "1,1,2," + std::to_string(i) + ",3,15");
        host = std::make_unique<HostCore>(HostCore::Config{55, "Room", "Host", 4, std::move(password)}, seed, g_clock, 7);
        local.startLocal(1, seed, "Room", g_clock);
        transport = std::make_unique<RelayHost>(hostRelay, code, hub.secretHostToken(code), clk);
    }

    Player& addPlayer(std::string const& name, std::string const& joinCode = {}, std::string const& password = {}) {
        auto p = std::make_unique<Player>();
        p->relay = std::make_unique<FakeRelay>(hub);
        p->link = connectToRoom(*p->relay, joinCode.empty() ? code : joinCode, p->secret, clk);
        p->link->setMaxFrame(kMaxFrameHostToClient);
        p->core.start(55, name, 0, g_clock, password);
        players.push_back(std::move(p));
        return *players.back();
    }

    void reconnect(Player& p) {
        p.token = p.core.token();
        p.actions.clear();
        p.link = connectToRoom(*p.relay, code, p.secret, clk);
        p.link->setMaxFrame(kMaxFrameHostToClient);
        p.core.start(55, "Ana", p.token, g_clock);
    }

    void step() {
        g_clock += 0.02;
        hub.step();
        transport->pump();
        while (auto c = transport->accept()) {
            c->setMaxFrame(kMaxFramePending);
            conns[host->onConnect(g_clock)] = std::move(c);
        }
        std::vector<std::string> frames;
        for (auto it = conns.begin(); it != conns.end();) {
            frames.clear();
            bool ok = it->second->update(frames);
            for (auto& f : frames) host->onMessage(it->first, f, g_clock);
            if (!ok) { host->onDisconnect(it->first, "closed"); it = conns.erase(it); continue; }
            if (host->peers().count(it->first) && host->peers().at(it->first).active) it->second->setMaxFrame(kMaxFrameClientToHost);
            ++it;
        }
        host->tick(g_clock);
        for (auto& m : local.takeOutbox()) host->onMessage(1, m, g_clock);
        for (auto& o : host->takeOutbox()) {
            if (o.peer == 1) { if (!o.bytes.empty()) local.onMessage(o.bytes, g_clock); continue; }
            auto c = conns.find(o.peer);
            if (c != conns.end() && !o.bytes.empty()) c->second->send(o.bytes);
            if (o.closeAfter && c != conns.end()) { c->second->update(frames); }
        }
        for (auto& p : players) {
            frames.clear();
            if (p->link && !p->link->update(frames)) p->link.reset();
            for (auto& f : frames) p->core.onMessage(f, g_clock);
            p->core.tick(g_clock);
            for (auto& m : p->core.takeOutbox())
                if (p->link) p->link->send(m);
            for (auto& a : p->core.takeActions()) p->actions.push_back(a);
        }
        local.takeActions();
    }

    template <class F>
    bool until(F pred, double seconds = 20.0) {
        double end = g_clock + seconds;
        while (g_clock < end) {
            step();
            if (pred()) return true;
        }
        return pred();
    }
};

static void full_session_over_relay() {
    std::printf("full_session_over_relay\n");
    g_clock = 0;
    Rig rig(3000); // a level big enough that the snapshot needs many chunks
    auto& ana = rig.addPlayer("Ana");
    CHECK(rig.until([&] { return ana.has(ClientCore::ActionKind::Pending); }));
    CHECK(rig.host->pendingCount() == 1);
    uint32_t pid = rig.host->peers().rbegin()->first;
    rig.host->accept(pid, g_clock);
    CHECK(rig.until([&] { return ana.has(ClientCore::ActionKind::LoadSnapshot); }));
    CHECK(ana.core.store.hash() == rig.host->store().hash());
    CHECK(ana.core.store.objects.size() == 3000);

    // Edits travel both ways.
    ana.core.submit({{OpKind::Create, makeId(ana.core.me(), 1), "1,8,2,30,3,15"}});
    rig.local.submit({{OpKind::Modify, makeId(1, 1), "1,1,2,45,3,15"}});
    CHECK(rig.until([&] { return ana.core.seq() == 2 && rig.local.seq() == 2; }));
    CHECK(ana.core.store.hash() == rig.host->store().hash());
    CHECK(rig.local.store.hash() == rig.host->store().hash());

    // A second player joins while the first keeps editing.
    auto& bo = rig.addPlayer("Bo");
    CHECK(rig.until([&] { return bo.has(ClientCore::ActionKind::Pending); }));
    rig.host->accept(rig.host->peers().rbegin()->first, g_clock);
    ana.core.submit({{OpKind::Delete, makeId(1, 2), ""}});
    CHECK(rig.until([&] { return bo.has(ClientCore::ActionKind::LoadSnapshot) && ana.core.seq() == bo.core.seq() && ana.core.seq() == rig.host->seq(); }));
    CHECK(bo.core.store.hash() == rig.host->store().hash());
    CHECK(ana.core.store.hash() == rig.host->store().hash());
    CHECK(rig.host->activeCount() == 3);
    // Everything went through one channel per player plus the lobby, never a broadcast of other players' data.
    CHECK(rig.transport->channelCount() == 2);
}

static void survives_duplicates_and_reordering() {
    std::printf("survives_duplicates_and_reordering\n");
    g_clock = 0;
    Rig rig(500);
    rig.hub.duplicate = true;
    rig.hub.reverse = true;
    auto& ana = rig.addPlayer("Ana");
    CHECK(rig.until([&] { return ana.has(ClientCore::ActionKind::Pending); }));
    rig.host->accept(rig.host->peers().rbegin()->first, g_clock);
    CHECK(rig.until([&] { return ana.has(ClientCore::ActionKind::LoadSnapshot); }));
    for (int i = 0; i < 20; ++i) ana.core.submit({{OpKind::Create, makeId(ana.core.me(), static_cast<uint32_t>(i + 1)), "1,8,2," + std::to_string(i) + ",3,15"}});
    CHECK(rig.until([&] { return rig.host->seq() >= 20 && ana.core.seq() == rig.host->seq(); }));
    CHECK(ana.core.store.hash() == rig.host->store().hash());
    CHECK(ana.core.store.objects.size() == 520); // duplicates did not apply twice
}

static void private_channels_keep_outsiders_out() {
    std::printf("private_channels_keep_outsiders_out\n");
    g_clock = 0;
    Rig rig(10);
    auto& ana = rig.addPlayer("Ana");
    CHECK(rig.until([&] { return ana.has(ClientCore::ActionKind::Pending); }));
    rig.host->accept(rig.host->peers().rbegin()->first, g_clock);
    CHECK(rig.until([&] { return ana.has(ClientCore::ActionKind::LoadSnapshot); }));

    // An eavesdropper with the room code sees the lobby but cannot join Ana's channel.
    FakeRelay spy(rig.hub);
    std::string anaChannel = peerChannel(rig.code, jidOf(ana.secret));
    spy.subscribe(lobbyChannel(rig.code), "");
    spy.subscribe(anaChannel, "\"jsecret\":\"" + newSecret() + "\"");
    spy.subscribe(anaChannel, "\"hostToken\":\"guess\"");
    spy.subscribe(anaChannel, "");
    ana.core.submit({{OpKind::Create, makeId(ana.core.me(), 1), "1,8,2,30,3,15"}});
    CHECK(rig.until([&] { return rig.host->seq() == 1; }));
    std::vector<RelayEvent> seen;
    spy.poll(seen);
    int failed = 0, subscribed = 0, messages = 0;
    for (auto const& e : seen) {
        failed += e.kind == RelayEvent::Kind::SubscribeFailed;
        subscribed += e.kind == RelayEvent::Kind::Subscribed;
        messages += e.kind == RelayEvent::Kind::Message;
    }
    CHECK(failed == 3);     // wrong secret, wrong host token, no proof
    CHECK(subscribed == 1); // only the lobby
    CHECK(messages == 0);   // and the lobby carries no level data

    // The real owner of the channel (knows the secret) and the host are allowed.
    CHECK(allowed(rig.hub, anaChannel, "\"jsecret\":\"" + ana.secret + "\""));
    CHECK(allowed(rig.hub, anaChannel, "\"hostToken\":\"" + rig.hub.secretHostToken(rig.code) + "\""));
    CHECK(!allowed(rig.hub, "private-et-ZZZZZZZZ-p" + jidOf(ana.secret), "\"hostToken\":\"" + rig.hub.secretHostToken(rig.code) + "\""));
}

static void host_application_rules_still_apply() {
    std::printf("host_application_rules_still_apply\n");
    g_clock = 0;
    Rig rig(5, "hunter2");
    // Wrong password over the relay is refused by the host, exactly like on the LAN.
    auto& bad = rig.addPlayer("Mallory", {}, "wrong");
    CHECK(rig.until([&] { return bad.has(ClientCore::ActionKind::Rejected); }));
    CHECK(rig.host->activeCount() == 1 && rig.host->pendingCount() == 0);
    auto& good = rig.addPlayer("Ana", {}, "hunter2");
    CHECK(rig.until([&] { return good.has(ClientCore::ActionKind::Pending); }));
    CHECK(rig.host->pendingCount() == 1);
}

static void wrong_code_times_out_with_a_clear_error() {
    std::printf("wrong_code_times_out_with_a_clear_error\n");
    g_clock = 0;
    Rig rig(2);
    auto& lost = rig.addPlayer("Ana", "ZZZZ2222");
    CHECK(rig.until([&] { return !lost.link; }, 40.0));
    CHECK(!lost.has(ClientCore::ActionKind::Pending));
    FakeRelay r2(rig.hub);
    auto link = connectToRoom(r2, "ZZZZ2222", newSecret(), clk);
    double start = g_clock;
    std::vector<std::string> frames;
    while (link->update(frames) && g_clock < start + 60) {
        g_clock += 0.05;
        rig.hub.step();
    }
    CHECK(link->error().find("did not answer") != std::string::npos);
    CHECK(g_clock - start > 29 && g_clock - start < 32);
}

static void reconnect_with_token_needs_no_second_approval() {
    std::printf("reconnect_with_token_needs_no_second_approval\n");
    g_clock = 0;
    Rig rig(50);
    auto& ana = rig.addPlayer("Ana");
    CHECK(rig.until([&] { return ana.has(ClientCore::ActionKind::Pending); }));
    rig.host->accept(rig.host->peers().rbegin()->first, g_clock);
    CHECK(rig.until([&] { return ana.has(ClientCore::ActionKind::LoadSnapshot); }));

    // Ana's phone loses the connection to the relay: her link fails right away and the host's link dies with the silence.
    ana.relay->drop();
    CHECK(rig.until([&] { return !ana.link; }, 5.0));
    ana.relay->restore();
    rig.reconnect(ana);
    CHECK(rig.until([&] { return ana.has(ClientCore::ActionKind::LoadSnapshot); }, 40.0));
    CHECK(!ana.has(ClientCore::ActionKind::Pending));
    CHECK(rig.host->activeCount() == 2); // the stale connection was replaced, not duplicated
    CHECK(ana.core.store.hash() == rig.host->store().hash());

    // And the host's own relay connection dropping fails every player link, so everybody resyncs.
    rig.hostRelay.drop();
    rig.step();
    CHECK(rig.conns.empty() || rig.until([&] { return rig.conns.empty(); }, 5.0));
}

static void channel_flood_is_bounded() {
    std::printf("channel_flood_is_bounded\n");
    g_clock = 0;
    Rig rig(1);
    FakeRelay noisy(rig.hub);
    noisy.subscribe(lobbyChannel(rig.code), "");
    rig.step();
    for (int i = 0; i < 300; ++i) {
        std::string jid = jidOf("secret" + std::to_string(i));
        noisy.publish(lobbyChannel(rig.code), "client-j", "{\"j\":\"" + jid + "\"}");
    }
    noisy.publish(lobbyChannel(rig.code), "client-j", "{\"j\":\"nothex\"}");
    noisy.publish(lobbyChannel(rig.code), "client-j", "garbage");
    for (int i = 0; i < 10; ++i) rig.step();
    CHECK(rig.transport->channelCount() == RelayHost::kMaxPeerChannels);
    // Channels nobody ever used are released after the idle time.
    g_clock += RelayHost::kIdleChannelSeconds + 1;
    rig.step();
    CHECK(rig.transport->channelCount() == 0);
}

static void code_helpers() {
    std::printf("code_helpers\n");
    CHECK(validCode("ABCD2345") && !validCode("ABCD234") && !validCode("ABCDO345") && !validCode("abcd2345") && !validCode("ABCD2341"));
    CHECK(normalizeCode(" abcd-2345 ") == "ABCD2345");
    CHECK(jidOf("00112233445566778899aabbccddeeff").size() == 32);
    CHECK(newSecret().size() == 32 && newSecret() != newSecret());
    CHECK(lobbyChannel("ABCD2345") == "private-et-ABCD2345-l");
}

int main() {
    json_parsing();
    websocket_framing();
    http_parsing();
    code_helpers();
    full_session_over_relay();
    survives_duplicates_and_reordering();
    private_channels_keep_outsiders_out();
    host_application_rules_still_apply();
    wrong_code_times_out_with_a_clear_error();
    reconnect_with_token_needs_no_second_approval();
    channel_flood_is_bounded();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
