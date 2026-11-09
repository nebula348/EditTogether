#include "RelayTransport.hpp"

#include "Json.hpp"
#include "WebSocket.hpp"

#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <memory>

namespace et::net {

namespace {

constexpr double kRelayBytesPerSecond = 256 * 1024;
constexpr double kAnnounceEvery = 1.0;
constexpr double kHostAnswerTimeout = 30.0;

std::string hexOf(unsigned char const* p, size_t n) {
    static char const* d = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out += d[p[i] >> 4];
        out += d[p[i] & 15];
    }
    return out;
}

bool isHex32(std::string const& s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c)); });
}

// The data string of every chunk event: {"b":"<base64 of the encoded chunk>"}.
std::string chunkData(std::string const& bytes) { return "{\"b\":\"" + ws::base64(bytes) + "\"}"; }

std::optional<Chunk> chunkFromData(std::string const& data) {
    auto obj = json::parseObject(data);
    if (!obj) return std::nullopt;
    bool ok = false;
    std::string raw = ws::base64Decode(json::str(*obj, "b"), ok);
    if (!ok) return std::nullopt;
    return decodeChunk(raw);
}

uint32_t newEpoch() {
    uint32_t v = 0;
    mbedtls_entropy_context e;
    mbedtls_entropy_init(&e);
    mbedtls_entropy_func(&e, reinterpret_cast<unsigned char*>(&v), sizeof(v));
    mbedtls_entropy_free(&e);
    return v | 1;
}

} // namespace

std::string lobbyChannel(std::string const& code) { return "private-et-" + code + "-l"; }
std::string peerChannel(std::string const& code, std::string const& jid) { return "private-et-" + code + "-p" + jid; }

std::string jidOf(std::string const& jsecretHex) {
    unsigned char digest[32];
    mbedtls_sha256(reinterpret_cast<unsigned char const*>(jsecretHex.data()), jsecretHex.size(), digest, 0);
    return hexOf(digest, 16);
}

std::string newSecret() {
    unsigned char raw[16];
    mbedtls_entropy_context e;
    mbedtls_entropy_init(&e);
    mbedtls_entropy_func(&e, raw, sizeof(raw));
    mbedtls_entropy_free(&e);
    return hexOf(raw, sizeof(raw));
}

bool validCode(std::string const& code) {
    return code.size() == 8 && std::all_of(code.begin(), code.end(), [](char c) {
        return (c >= 'A' && c <= 'Z' && c != 'I' && c != 'O') || (c >= '2' && c <= '9');
    });
}

std::string normalizeCode(std::string text) {
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '-') out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// ------------------------------------------------------------------ host

namespace {
class PeerLink;
}

struct RelayHost::Impl : std::enable_shared_from_this<RelayHost::Impl> {
    Relay& relay;
    std::string code, hostToken, lobby;
    ClockFn clock;
    bool accepting = true;
    bool lobbyReady = false;

    struct Channel {
        bool subscribed = false;
        double lastActive = 0;
        int links = 0;
    };
    std::map<std::string, Channel> channels; // by jid
    std::map<std::pair<std::string, uint32_t>, PeerLink*> links;
    std::deque<std::unique_ptr<Link>> unclaimed;

    Impl(Relay& r, std::string c, std::string t, ClockFn k)
      : relay(r), code(std::move(c)), hostToken(std::move(t)), lobby(lobbyChannel(code)), clock(std::move(k)) {}

    std::string proof() const { return "\"hostToken\":\"" + hostToken + "\""; }
    void sendReady(std::string const& jid) { relay.publish(peerChannel(code, jid), "client-r", "{}"); }
    void dropChannel(std::string const& jid);
    void onEvent(RelayEvent const& e);
};

namespace {

class PeerLink final : public Link {
public:
    PeerLink(std::shared_ptr<RelayHost::Impl> host, std::string jid, uint32_t epoch)
      : m_host(host), m_jid(std::move(jid)), m_epoch(epoch),
        m_inner(epoch, "Online player", [host = std::weak_ptr(host), jid = m_jid](std::string bytes) {
            if (auto h = host.lock()) h->relay.publish(peerChannel(h->code, jid), "client-c", chunkData(bytes));
        }, host->clock, kRelayBytesPerSecond) {
        host->links[{m_jid, m_epoch}] = this;
        ++host->channels[m_jid].links;
    }
    ~PeerLink() override {
        if (auto h = m_host.lock()) {
            auto it = h->links.find({m_jid, m_epoch});
            if (it != h->links.end() && it->second == this) h->links.erase(it);
            auto ch = h->channels.find(m_jid);
            if (ch != h->channels.end() && --ch->second.links <= 0) {
                ch->second.links = 0;
                ch->second.lastActive = h->clock();
            }
        }
    }
    bool update(std::vector<std::string>& frames) override { return m_inner.update(frames); }
    void send(std::string_view p) override { m_inner.send(p); }
    void setMaxFrame(uint32_t n) override { m_inner.setMaxFrame(n); }
    bool outEmpty() const override { return m_inner.outEmpty(); }
    std::string const& error() const override { return m_inner.error(); }
    std::string describe() const override { return m_inner.describe(); }
    uint64_t bytesIn() const override { return m_inner.bytesIn(); }
    uint64_t bytesOut() const override { return m_inner.bytesOut(); }
    bool viaRelay() const override { return true; }
    ChunkLink& inner() { return m_inner; }

private:
    std::weak_ptr<RelayHost::Impl> m_host;
    std::string m_jid;
    uint32_t m_epoch;
    ChunkLink m_inner;
};

} // namespace

void RelayHost::Impl::dropChannel(std::string const& jid) {
    relay.unsubscribe(peerChannel(code, jid));
    channels.erase(jid);
}

void RelayHost::Impl::onEvent(RelayEvent const& e) {
    using K = RelayEvent::Kind;
    double now = clock();
    if (e.kind == K::Disconnected) {
        lobbyReady = false;
        for (auto& [jid, ch] : channels) ch.subscribed = false;
        // Anything in flight is gone, so every link has to start over.
        for (auto& [key, link] : links) link->inner().fail("Lost the connection to the relay");
        return;
    }
    if (e.kind == K::Subscribed) {
        if (e.channel == lobby) { lobbyReady = true; return; }
        for (auto& [jid, ch] : channels)
            if (e.channel == peerChannel(code, jid)) {
                ch.subscribed = true;
                ch.lastActive = now;
                sendReady(jid);
            }
        return;
    }
    if (e.kind == K::SubscribeFailed) {
        for (auto it = channels.begin(); it != channels.end(); ++it)
            if (e.channel == peerChannel(code, it->first)) { channels.erase(it); break; }
        return;
    }
    if (e.kind != K::Message) return;

    if (e.channel == lobby) {
        if (e.event != "client-j" || !accepting) return;
        auto obj = json::parseObject(e.data);
        std::string jid = obj ? json::str(*obj, "j") : std::string();
        if (!isHex32(jid)) return;
        auto it = channels.find(jid);
        if (it == channels.end()) {
            if (channels.size() >= RelayHost::kMaxPeerChannels) return; // flood guard
            channels[jid].lastActive = now;
            relay.subscribe(peerChannel(code, jid), proof());
        } else if (it->second.subscribed) {
            it->second.lastActive = now;
            sendReady(jid); // the announce repeats until the player has heard us
        }
        return;
    }

    // peer channel: private-et-CODE-p<jid>
    std::string prefix = "private-et-" + code + "-p";
    if (e.channel.rfind(prefix, 0) != 0 || e.event != "client-c") return;
    std::string jid = e.channel.substr(prefix.size());
    auto ch = channels.find(jid);
    if (ch == channels.end()) return;
    ch->second.lastActive = now;
    auto chunk = chunkFromData(e.data);
    if (!chunk) return;
    auto it = links.find({jid, chunk->epoch});
    if (it != links.end()) {
        it->second->inner().deliver(std::move(*chunk));
        return;
    }
    // A new connection starts with chunk 0, and only while the room accepts players.
    if (!accepting || chunk->seq != 0 || unclaimed.size() >= 16) return;
    auto link = std::make_unique<PeerLink>(shared_from_this(), jid, chunk->epoch);
    link->inner().deliver(std::move(*chunk));
    unclaimed.push_back(std::move(link));
}

RelayHost::RelayHost(Relay& relay, std::string code, std::string hostToken, ClockFn clock)
  : m(std::make_shared<Impl>(relay, std::move(code), std::move(hostToken), std::move(clock))) {
    relay.subscribe(m->lobby, m->proof());
}

RelayHost::~RelayHost() {
    m->unclaimed.clear();
    m->relay.unsubscribe(m->lobby);
    for (auto& [jid, ch] : m->channels) m->relay.unsubscribe(peerChannel(m->code, jid));
}

void RelayHost::pump() {
    std::vector<RelayEvent> events;
    m->relay.poll(events);
    for (auto const& e : events) m->onEvent(e);
    // Channels nobody used for a while (aborted joins) are released.
    double now = m->clock();
    for (auto it = m->channels.begin(); it != m->channels.end();) {
        if (it->second.links == 0 && now - it->second.lastActive > kIdleChannelSeconds) {
            m->relay.unsubscribe(peerChannel(m->code, it->first));
            it = m->channels.erase(it);
        } else {
            ++it;
        }
    }
}

std::unique_ptr<Link> RelayHost::accept() {
    if (m->unclaimed.empty()) return nullptr;
    auto link = std::move(m->unclaimed.front());
    m->unclaimed.pop_front();
    return link;
}

void RelayHost::setAccepting(bool on) {
    m->accepting = on;
    if (!on) m->unclaimed.clear();
}
bool RelayHost::lobbyReady() const { return m->lobbyReady; }
size_t RelayHost::channelCount() const { return m->channels.size(); }
std::string const& RelayHost::code() const { return m->code; }

// ------------------------------------------------------------------ player

namespace {

class RoomLink final : public Link {
public:
    RoomLink(Relay& relay, std::string code, std::string secret, ClockFn clock)
      : m_relay(relay), m_clock(std::move(clock)), m_lobby(lobbyChannel(code)), m_jid(jidOf(secret)), m_peer(peerChannel(code, m_jid)),
        m_inner(newEpoch(), "Online room " + code, [this](std::string bytes) {
            if (m_ready) m_relay.publish(m_peer, "client-c", chunkData(bytes));
            else m_held.push_back(std::move(bytes));
        }, m_clock, kRelayBytesPerSecond) {
        m_startedAt = m_clock();
        m_relay.subscribe(m_peer, "\"jsecret\":\"" + secret + "\"");
        m_relay.subscribe(m_lobby, "");
    }
    ~RoomLink() override {
        m_relay.unsubscribe(m_peer);
        m_relay.unsubscribe(m_lobby);
    }

    bool update(std::vector<std::string>& frames) override {
        using K = RelayEvent::Kind;
        std::vector<RelayEvent> events;
        m_relay.poll(events);
        for (auto const& e : events) {
            if (e.kind == K::Disconnected && (m_peerSub || m_ready)) {
                m_inner.fail("Lost the connection to the relay");
            } else if (e.kind == K::Subscribed) {
                if (e.channel == m_peer) m_peerSub = true;
                if (e.channel == m_lobby) m_lobbySub = true;
            } else if (e.kind == K::SubscribeFailed && (e.channel == m_peer || e.channel == m_lobby)) {
                m_inner.fail("The relay refused the room: " + e.data);
            } else if (e.kind == K::Message && e.channel == m_peer) {
                if (e.event == "client-r") {
                    if (!m_ready) {
                        m_ready = true;
                        for (auto& b : m_held) m_relay.publish(m_peer, "client-c", chunkData(b));
                        m_held.clear();
                    }
                } else if (e.event == "client-c") {
                    if (auto c = chunkFromData(e.data)) m_inner.deliver(std::move(*c));
                }
            }
        }
        double now = m_clock();
        if (!m_ready) {
            if (m_peerSub && m_lobbySub && now - m_lastAnnounce >= kAnnounceEvery) {
                m_lastAnnounce = now;
                m_relay.publish(m_lobby, "client-j", "{\"j\":\"" + m_jid + "\"}");
            }
            if (now - m_startedAt > kHostAnswerTimeout) m_inner.fail("The host did not answer. Check the room code and that the room is still open");
        }
        return m_inner.update(frames);
    }
    void send(std::string_view p) override { m_inner.send(p); }
    void setMaxFrame(uint32_t n) override { m_inner.setMaxFrame(n); }
    bool outEmpty() const override { return m_inner.outEmpty() && m_held.empty(); }
    std::string const& error() const override { return m_inner.error(); }
    std::string describe() const override { return m_inner.describe(); }
    uint64_t bytesIn() const override { return m_inner.bytesIn(); }
    uint64_t bytesOut() const override { return m_inner.bytesOut(); }
    bool viaRelay() const override { return true; }

private:
    Relay& m_relay;
    ClockFn m_clock;
    std::string m_lobby, m_jid, m_peer;
    ChunkLink m_inner;
    std::vector<std::string> m_held; // chunks waiting for the host's "ready"
    bool m_peerSub = false, m_lobbySub = false, m_ready = false;
    double m_startedAt = 0, m_lastAnnounce = -100;
};

} // namespace

std::unique_ptr<Link> connectToRoom(Relay& relay, std::string const& code, std::string const& jsecretHex, ClockFn clock) {
    return std::make_unique<RoomLink>(relay, code, jsecretHex, std::move(clock));
}

} // namespace et::net
