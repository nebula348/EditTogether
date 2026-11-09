#include "ApinatorClient.hpp"

#include "Http.hpp"
#include "Json.hpp"
#include "Tls.hpp"
#include "WebSocket.hpp"

#include <mbedtls/entropy.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>

namespace et::net {

namespace {

double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint32_t randomU32() {
    uint32_t v = 0;
    mbedtls_entropy_context e;
    mbedtls_entropy_init(&e);
    mbedtls_entropy_func(&e, reinterpret_cast<unsigned char*>(&v), sizeof(v));
    mbedtls_entropy_free(&e);
    return v;
}

constexpr size_t kMaxQueued = 4096;     // commands waiting for the network thread
constexpr size_t kMaxInbox = 50000;     // events waiting for the game thread
constexpr double kPingEvery = 20.0;     // idle seconds before we ping; Apinator's default timeout is 120 s
constexpr double kPingPeriod = 30.0;    // also ping while busy, to keep the latency reading fresh
constexpr double kPongTimeout = 30.0;
constexpr double kHandshakeTimeout = 10.0;

} // namespace

struct ApinatorClient::Shared {
    Config cfg;
    int wakeRead = -1, wakeWrite = -1;
    std::atomic<bool> stop{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> dropRequested{false};
    std::atomic<double> pingMs{-1};
    std::atomic<uint64_t> in{0}, out{0};

    std::mutex m;
    struct Cmd {
        enum Kind { Subscribe, Unsubscribe, Publish } kind;
        std::string channel, a, b; // Subscribe: a=authFields; Publish: a=event, b=data
    };
    std::deque<Cmd> cmds;
    std::vector<RelayEvent> inbox;
    std::string status = "Connecting";

    void wake() {
        char c = 1;
        if (wakeWrite >= 0) (void)!::write(wakeWrite, &c, 1);
    }
    void emit(RelayEvent e) {
        std::lock_guard lock(m);
        if (inbox.size() < kMaxInbox) inbox.push_back(std::move(e));
    }
    void setStatus(std::string s) {
        std::lock_guard lock(m);
        status = std::move(s);
    }
    // Sleeps up to `seconds`, returning early when stopping.
    void sleep(double seconds) {
        double end = nowSec() + seconds;
        while (!stop && nowSec() < end) {
            waitFds(wakeRead, -1, static_cast<int>(std::min(250.0, (end - nowSec()) * 1000) + 1));
            drainWake();
        }
    }
    void drainWake() {
        char buf[64];
        while (::read(wakeRead, buf, sizeof(buf)) > 0) {}
    }

    void run();
};

namespace {

// State of one WebSocket connection; the wanted channels carry over to the next one.
struct Connection {
    ApinatorClient::Shared& s;
    HttpClient& http;
    Connection(ApinatorClient::Shared& shared, HttpClient& client) : s(shared), http(client) {}
    Stream ws;
    ws::Reader reader;
    std::string socketId;
    double lastRecv = 0, pingSent = 0, lastPing = 0;
    std::map<std::string, std::string> wanted; // channel -> auth fields
    std::set<std::string> live;                // subscription requested on this socket

    bool send(ws::Opcode op, std::string_view payload) {
        std::string f = ws::encodeFrame(op, payload, randomU32());
        return ws.write(f.data(), f.size());
    }

    bool sendEvent(std::string const& event, std::string const& channel, std::string const& data) {
        std::string f = "{\"event\":\"" + json::escape(event) + "\"";
        if (!channel.empty()) f += ",\"channel\":\"" + json::escape(channel) + "\"";
        f += ",\"data\":\"" + json::escape(data) + "\"}";
        return send(ws::Text, f);
    }

    // Gets signatures and subscribes. Channels with the same proof share one auth request.
    bool subscribeAll(std::vector<std::string> const& channels, std::string& error) {
        std::map<std::string, std::vector<std::string>> byProof;
        for (auto const& c : channels) {
            auto it = wanted.find(c);
            if (it != wanted.end()) byProof[it->second].push_back(c);
        }
        for (auto& [proof, list] : byProof) {
            for (size_t at = 0; at < list.size(); at += 32) { // the service accepts up to 64; keep requests small
                std::string body = "{\"socket_id\":\"" + json::escape(socketId) + "\",\"channels\":[";
                size_t end = std::min(list.size(), at + 32);
                for (size_t i = at; i < end; ++i) body += (i > at ? "," : "") + std::string("\"") + json::escape(list[i]) + "\"";
                body += "]";
                if (!proof.empty()) body += "," + proof;
                body += "}";
                HttpResponse r = http.post("/v1/auth", body);
                if (r.status == 0) { error = "Auth service unreachable: " + r.error; return false; }
                auto obj = json::parseObject(r.body);
                std::optional<json::Object> auth;
                if (obj && r.status == 200) {
                    auto it = obj->find("auth");
                    if (it != obj->end()) auth = json::parseObject(it->second.text);
                }
                for (size_t i = at; i < end; ++i) {
                    std::string signature = auth ? json::str(*auth, list[i]) : std::string();
                    if (signature.empty()) {
                        std::string why = obj ? json::str(*obj, "error") : std::string();
                        s.emit({RelayEvent::Kind::SubscribeFailed, list[i], {},
                                why.empty() ? "auth service answered " + std::to_string(r.status) : why});
                        wanted.erase(list[i]);
                        continue;
                    }
                    std::string inner = "{\"channel\":\"" + json::escape(list[i]) + "\",\"auth\":\"" + json::escape(signature) + "\"}";
                    if (!sendEvent("realtime:subscribe", {}, inner)) { error = ws.error(); return false; }
                    live.insert(list[i]);
                }
            }
        }
        return true;
    }

    bool handle(ws::Message const& msg, std::string& error) {
        lastRecv = nowSec();
        if (msg.op == ws::Ping) return send(ws::Pong, msg.data);
        if (msg.op == ws::Pong) return true;
        if (msg.op == ws::Close) { error = "Closed by the server"; return false; }
        auto frame = json::parseObject(msg.data);
        if (!frame) return true; // not ours; ignore
        ++s.in;
        std::string event = json::str(*frame, "event");
        std::string channel = json::str(*frame, "channel");
        std::string data = json::str(*frame, "data");
        if (event.rfind("client-", 0) == 0) {
            s.emit({RelayEvent::Kind::Message, channel, event, data});
        } else if (event == "realtime:subscription_succeeded") {
            s.emit({RelayEvent::Kind::Subscribed, channel, {}, {}});
        } else if (event == "realtime:subscription_error") {
            live.erase(channel);
            wanted.erase(channel);
            s.emit({RelayEvent::Kind::SubscribeFailed, channel, {}, data});
        } else if (event == "realtime:pong") {
            if (pingSent > 0) s.pingMs = (nowSec() - pingSent) * 1000.0;
            pingSent = 0;
        } else if (event == "realtime:ping") {
            return sendEvent("realtime:pong", {}, "{}");
        } else if (event == "realtime:error") {
            auto d = json::parseObject(data);
            error = d ? json::str(*d, "message") : data;
            if (error.empty()) error = "Apinator reported an error";
            return false;
        }
        return true;
    }
};

} // namespace

void ApinatorClient::Shared::run() {
    auto url = parseUrl(cfg.authUrl);
    if (!url) {
        setStatus("Bad relay URL");
        emit({RelayEvent::Kind::Disconnected, {}, {}, "The relay URL in the mod settings is not valid"});
        return;
    }
    HttpClient http(*url);
    std::string key, cluster;
    std::map<std::string, std::string> wanted; // survives reconnects
    double backoff = 1.0;

    while (!stop) {
        std::string error;
        // 1. App key and region from the auth service (public values).
        if (key.empty()) {
            setStatus("Contacting relay service");
            HttpResponse r = http.get("/v1/config");
            auto obj = r.status == 200 ? json::parseObject(r.body) : std::nullopt;
            if (!obj || json::str(*obj, "key").empty()) {
                error = r.status == 0 ? "Relay service unreachable: " + r.error : "Relay service answered " + std::to_string(r.status);
                emit({RelayEvent::Kind::Disconnected, {}, {}, error});
                setStatus(error);
                sleep(backoff);
                backoff = std::min(backoff * 2, 15.0);
                continue;
            }
            key = json::str(*obj, "key");
            cluster = json::str(*obj, "cluster");
        }

        // 2. WebSocket to Apinator.
        setStatus("Connecting to Apinator (" + cluster + ")");
        Connection c{*this, http};
        c.wanted = std::move(wanted);
        std::string host = "ws-" + cluster + ".apinator.io";
        std::array<uint32_t, 4> nonce{randomU32(), randomU32(), randomU32(), randomU32()};
        std::string wsKey = ws::base64(std::string_view(reinterpret_cast<char const*>(nonce.data()), 16));
        bool ok = c.ws.connect(host, 443, true, kHandshakeTimeout, error);
        std::string request = ws::handshakeRequest(host, "/app/" + key, wsKey);
        if (ok) ok = c.ws.write(request.data(), request.size());
        std::string buf;
        double deadline = nowSec() + kHandshakeTimeout;
        while (ok) { // handshake reply
            char tmp[2048];
            int n = c.ws.read(reinterpret_cast<uint8_t*>(tmp), sizeof(tmp));
            if (n < 0) { error = c.ws.error(); ok = false; break; }
            buf.append(tmp, static_cast<size_t>(n));
            auto done = ws::checkHandshakeResponse(buf, wsKey, error);
            if (!done) { ok = false; break; }
            if (*done > 0) { c.reader.feed(buf.data() + *done, buf.size() - *done); break; }
            if (stop || nowSec() > deadline) { error = "WebSocket handshake timed out"; ok = false; break; }
            waitFds(c.ws.fd(), wakeRead, 200);
        }

        // 3. Wait for connection_established, then live.
        bool established = false;
        while (ok && !established) {
            auto msg = c.reader.next();
            if (!msg) {
                if (!c.reader.error().empty()) { error = c.reader.error(); ok = false; break; }
                if (stop || nowSec() > deadline) { error = "Apinator did not answer"; ok = false; break; }
                waitFds(c.ws.fd(), wakeRead, 200);
                char tmp[4096];
                int n = c.ws.read(reinterpret_cast<uint8_t*>(tmp), sizeof(tmp));
                if (n < 0) { error = c.ws.error(); ok = false; break; }
                if (n > 0) c.reader.feed(tmp, static_cast<size_t>(n));
                continue;
            }
            if (msg->op != ws::Text) continue;
            auto frame = json::parseObject(msg->data);
            if (!frame) continue;
            std::string event = json::str(*frame, "event");
            if (event == "realtime:connection_established") {
                auto d = json::parseObject(json::str(*frame, "data"));
                c.socketId = d ? json::str(*d, "socket_id") : std::string();
                if (c.socketId.empty()) { error = "Apinator sent no socket id"; ok = false; break; }
                established = true;
            } else if (event == "realtime:error") {
                auto d = json::parseObject(json::str(*frame, "data"));
                error = d ? json::str(*d, "message") : "Apinator refused the connection";
                ok = false;
            }
        }

        if (ok) {
            c.lastRecv = nowSec();
            c.lastPing = c.lastRecv - kPingPeriod + 1; // first ping one second after connecting, to measure the latency
            connected = true;
            backoff = 1.0;
            setStatus("Connected");
            emit({RelayEvent::Kind::Connected, {}, {}, {}});
            std::vector<std::string> all;
            for (auto const& [ch, proof] : c.wanted) all.push_back(ch);
            ok = c.subscribeAll(all, error);

            // 4. Main loop.
            while (ok && !stop) {
                std::deque<Cmd> work;
                {
                    std::lock_guard lock(m);
                    work.swap(cmds);
                }
                std::vector<std::string> fresh;
                for (auto& cmd : work) {
                    if (cmd.kind == Cmd::Subscribe) {
                        c.wanted[cmd.channel] = cmd.a;
                        if (!c.live.count(cmd.channel)) fresh.push_back(cmd.channel);
                    } else if (cmd.kind == Cmd::Unsubscribe) {
                        c.wanted.erase(cmd.channel);
                        if (c.live.erase(cmd.channel)) ok = ok && c.sendEvent("realtime:unsubscribe", {}, "{\"channel\":\"" + json::escape(cmd.channel) + "\"}");
                    } else if (c.live.count(cmd.channel)) {
                        ++out;
                        ok = ok && c.sendEvent(cmd.a, cmd.channel, cmd.b);
                    } // publishing to a channel we're not subscribed to is refused by Apinator, so drop it
                }
                if (ok && !fresh.empty()) ok = c.subscribeAll(fresh, error);
                if (ok) ok = c.ws.flush();
                if (!ok && error.empty()) error = c.ws.error();

                // Read everything available.
                char tmp[16384];
                for (int n; ok && (n = c.ws.read(reinterpret_cast<uint8_t*>(tmp), sizeof(tmp))) != 0;) {
                    if (n < 0) { error = c.ws.error(); ok = false; break; }
                    c.reader.feed(tmp, static_cast<size_t>(n));
                }
                while (ok) {
                    auto msg = c.reader.next();
                    if (!msg) {
                        if (!c.reader.error().empty()) { error = c.reader.error(); ok = false; }
                        break;
                    }
                    ok = c.handle(*msg, error);
                }
                if (!ok) break;
                if (dropRequested.exchange(false)) { error = "Simulated network failure"; ok = false; break; }

                // Keepalive: ping after idle time, give up when nothing comes back.
                double t = nowSec();
                if (c.pingSent > 0 && t - c.pingSent > kPongTimeout) { error = "Connection timed out"; ok = false; break; }
                if (c.pingSent == 0 && (t - c.lastRecv > kPingEvery || t - c.lastPing > kPingPeriod)) {
                    c.pingSent = c.lastPing = t;
                    ok = c.sendEvent("realtime:ping", {}, "{}");
                }
                if (!ok) { error = c.ws.error(); break; }

                if (!c.ws.hasBuffered()) {
                    waitFds(c.ws.fd(), wakeRead, 250, c.ws.wantsWrite());
                    drainWake();
                }
            }
            if (stop && c.ws.open()) {
                c.send(ws::Close, std::string("\x03\xe8", 2)); // 1000 normal closure
                c.ws.flush();
            }
        }

        connected = false;
        pingMs = -1;
        wanted = std::move(c.wanted);
        c.ws.close();
        if (stop) break;
        if (error.empty()) error = "Connection lost";
        setStatus("Reconnecting: " + error);
        emit({RelayEvent::Kind::Disconnected, {}, {}, error});
        // Commands queued while offline: subscriptions are kept, publishes are dropped (the link layer resyncs).
        {
            std::lock_guard lock(m);
            for (auto& cmd : cmds)
                if (cmd.kind == Cmd::Subscribe) wanted[cmd.channel] = cmd.a;
                else if (cmd.kind == Cmd::Unsubscribe) wanted.erase(cmd.channel);
            cmds.clear();
        }
        sleep(backoff);
        backoff = std::min(backoff * 2, 15.0);
    }
}

ApinatorClient::ApinatorClient(Config cfg) : m_s(std::make_shared<Shared>()) {
    m_s->cfg = std::move(cfg);
    int fds[2];
    if (::pipe(fds) == 0) {
        for (int fd : fds) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        m_s->wakeRead = fds[0];
        m_s->wakeWrite = fds[1];
    }
    // The thread keeps its own reference so destroying the client never waits on a slow connect.
    std::thread([s = m_s] {
        s->run();
        ::close(s->wakeRead);
        ::close(s->wakeWrite);
    }).detach();
}

ApinatorClient::~ApinatorClient() {
    m_s->stop = true;
    m_s->wake();
}

void ApinatorClient::subscribe(std::string channel, std::string authFields) {
    {
        std::lock_guard lock(m_s->m);
        if (m_s->cmds.size() >= kMaxQueued) return;
        m_s->cmds.push_back({Shared::Cmd::Subscribe, std::move(channel), std::move(authFields), {}});
    }
    m_s->wake();
}

void ApinatorClient::unsubscribe(std::string channel) {
    {
        std::lock_guard lock(m_s->m);
        m_s->cmds.push_back({Shared::Cmd::Unsubscribe, std::move(channel), {}, {}});
    }
    m_s->wake();
}

void ApinatorClient::publish(std::string channel, std::string event, std::string data) {
    {
        std::lock_guard lock(m_s->m);
        if (m_s->cmds.size() >= kMaxQueued) return; // overloaded or offline: the link layer detects the gap and resyncs
        m_s->cmds.push_back({Shared::Cmd::Publish, std::move(channel), std::move(event), std::move(data)});
    }
    m_s->wake();
}

void ApinatorClient::poll(std::vector<RelayEvent>& out) {
    std::lock_guard lock(m_s->m);
    if (out.empty()) out.swap(m_s->inbox);
    else {
        for (auto& e : m_s->inbox) out.push_back(std::move(e));
        m_s->inbox.clear();
    }
}

void ApinatorClient::simulateDisconnect() {
    m_s->dropRequested = true;
    m_s->wake();
}

bool ApinatorClient::connected() const { return m_s->connected; }
double ApinatorClient::pingMs() const { return m_s->pingMs; }
uint64_t ApinatorClient::messagesIn() const { return m_s->in; }
uint64_t ApinatorClient::messagesOut() const { return m_s->out; }

std::string ApinatorClient::status() const {
    std::lock_guard lock(m_s->m);
    return m_s->status;
}

RoomGrant requestRoom(std::string const& authUrl) {
    RoomGrant g;
    auto url = parseUrl(authUrl);
    if (!url) { g.error = "The relay URL in the mod settings is not valid"; return g; }
    HttpClient http(*url);
    HttpResponse r = http.post("/v1/room", "{}");
    if (r.status != 200) {
        g.error = r.status == 0 ? "Relay service unreachable: " + r.error : "Relay service answered " + std::to_string(r.status);
        return g;
    }
    auto obj = json::parseObject(r.body);
    if (!obj) { g.error = "Relay service sent an unreadable reply"; return g; }
    g.code = json::str(*obj, "code");
    g.hostToken = json::str(*obj, "hostToken");
    if (g.code.empty() || g.hostToken.empty()) g.error = "Relay service sent an incomplete reply";
    return g;
}

} // namespace et::net
