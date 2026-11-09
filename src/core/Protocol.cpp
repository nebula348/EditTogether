#include "Protocol.hpp"

#include "Bytes.hpp"

#include <bit>
#include <cctype>
#include <cmath>

namespace et {

char const* roleName(Role r) {
    switch (r) {
        case Role::Admin: return "Administrator";
        case Role::Editor: return "Editor";
        case Role::Viewer: return "Viewer";
    }
    return "Unknown";
}

namespace {

void writeOp(ByteWriter& w, Op const& op) {
    w.u8(static_cast<uint8_t>(op.kind));
    w.u64(op.id);
    w.str(op.data);
}

bool readOp(ByteReader& r, Op& op) {
    uint8_t k = r.u8();
    if (k < 1 || k > 4) return false;
    op.kind = static_cast<OpKind>(k);
    op.id = r.u64();
    op.data = r.str(op.kind == OpKind::Header ? kMaxHeaderString : kMaxObjectString);
    return r.ok;
}

void writeOps(ByteWriter& w, std::vector<Op> const& ops) {
    w.u32(static_cast<uint32_t>(ops.size()));
    for (auto const& op : ops) writeOp(w, op);
}

bool readOps(ByteReader& r, std::vector<Op>& ops) {
    uint32_t n = r.count(13, kMaxOpsPerMessage);
    ops.resize(n);
    for (auto& op : ops)
        if (!readOp(r, op)) return false;
    return r.ok;
}

void writeIds(ByteWriter& w, std::vector<uint64_t> const& ids) {
    w.u32(static_cast<uint32_t>(ids.size()));
    for (auto id : ids) w.u64(id);
}

bool readIds(ByteReader& r, std::vector<uint64_t>& ids) {
    uint32_t n = r.count(8, kMaxLockIds);
    ids.resize(n);
    for (auto& id : ids) id = r.u64();
    return r.ok;
}

void writeGroup(ByteWriter& w, LockGroup const& g) {
    w.u32(g.owner);
    writeIds(w, g.ids);
}

bool readGroup(ByteReader& r, LockGroup& g) {
    g.owner = r.u32();
    return readIds(r, g.ids);
}

void writePlay(ByteWriter& w, msg::PlayState const& x) {
    w.u32(x.peer);
    w.u8(x.playing ? 1 : 0);
    w.u64(x.time);
    w.u8(x.life);
    for (float f : {x.x, x.y, x.rotation, x.scale}) w.u32(std::bit_cast<uint32_t>(f));
    w.u8(x.mode);
    w.u16(x.icon);
    w.u16(x.cube);
    w.u8(x.color1);
    w.u8(x.color2);
    w.u8(x.glowColor);
    w.u8(x.flags);
}

void readPlay(ByteReader& r, msg::PlayState& x) {
    x.peer = r.u32();
    x.playing = r.u8() != 0;
    x.time = r.u64();
    x.life = r.u8();
    x.x = std::bit_cast<float>(r.u32());
    x.y = std::bit_cast<float>(r.u32());
    x.rotation = std::bit_cast<float>(r.u32());
    x.scale = std::bit_cast<float>(r.u32());
    x.mode = r.u8();
    x.icon = r.u16();
    x.cube = r.u16();
    x.color1 = r.u8();
    x.color2 = r.u8();
    x.glowColor = r.u8();
    x.flags = r.u8();
    if (!r.ok) return;
    if (!std::isfinite(x.x) || !std::isfinite(x.y) || !std::isfinite(x.rotation) || !(x.scale > 0.05f && x.scale < 5.f)
        || x.mode > 7 || x.icon == 0 || x.icon > 2000 || x.cube == 0 || x.cube > 2000)
        r.ok = false;
}

template <class... F> struct Overload : F... { using F::operator()...; };

void writeBody(ByteWriter& w, Message const& m) {
    std::visit(Overload{
        [&](msg::Hello const& x) { w.u16(x.protocol); w.u64(x.roomId); w.str(x.nickname); w.u64(x.token); w.str(x.password); },
        [&](msg::Ops const& x) { w.u32(x.clientSeq); writeOps(w, x.ops); },
        [&](msg::LockSet const& x) { writeIds(w, x.ids); },
        [&](msg::SnapshotRequest const&) {},
        [&](msg::Leave const&) {},
        [&](msg::Ping const& x) { w.u64(x.time); },
        [&](msg::Pong const& x) { w.u64(x.time); },
        [&](msg::JoinPending const&) {},
        [&](msg::JoinAccepted const& x) { w.u32(x.peerId); w.u64(x.token); w.str(x.roomName); },
        [&](msg::JoinRejected const& x) { w.str(x.reason); },
        [&](msg::PeerList const& x) {
            w.u32(static_cast<uint32_t>(x.peers.size()));
            for (auto const& p : x.peers) { w.u32(p.id); w.str(p.nickname); w.u8(static_cast<uint8_t>(p.role)); }
        },
        [&](msg::OpsBroadcast const& x) { w.u64(x.firstSeq); w.u32(x.fromPeer); w.u32(x.clientSeq); writeOps(w, x.ops); },
        [&](msg::Correction const& x) { writeOps(w, x.ops); w.str(x.reason); },
        [&](msg::LockState const& x) { writeGroup(w, x.group); },
        [&](msg::Checksum const& x) { w.u64(x.seq); w.u64(x.hash); w.u32(x.count); },
        [&](msg::Snapshot const& x) {
            w.u64(x.seq);
            w.str(x.header);
            w.u32(static_cast<uint32_t>(x.objects.size()));
            for (auto const& [id, s] : x.objects) { w.u64(id); w.str(s); }
            w.u32(static_cast<uint32_t>(x.locks.size()));
            for (auto const& g : x.locks) writeGroup(w, g);
        },
        [&](msg::Kicked const& x) { w.str(x.reason); },
        [&](msg::RoomClosed const& x) { w.str(x.reason); },
        [&](msg::PlayState const& x) { writePlay(w, x); },
        [&](msg::PlayStates const& x) {
            w.u32(static_cast<uint32_t>(x.states.size()));
            for (auto const& s : x.states) writePlay(w, s);
        },
    }, m);
}

template <size_t I = 0>
std::optional<Message> makeByIndex(size_t idx) {
    if constexpr (I < std::variant_size_v<Message>) {
        if (idx == I) return Message(std::in_place_index<I>);
        return makeByIndex<I + 1>(idx);
    } else {
        return std::nullopt;
    }
}

bool readBody(ByteReader& r, Message& m) {
    std::visit(Overload{
        [&](msg::Hello& x) { x.protocol = r.u16(); x.roomId = r.u64(); x.nickname = r.str(64); x.token = r.u64(); x.password = r.str(kMaxPassword); },
        [&](msg::Ops& x) { x.clientSeq = r.u32(); readOps(r, x.ops); },
        [&](msg::LockSet& x) { readIds(r, x.ids); },
        [&](msg::SnapshotRequest&) {},
        [&](msg::Leave&) {},
        [&](msg::Ping& x) { x.time = r.u64(); },
        [&](msg::Pong& x) { x.time = r.u64(); },
        [&](msg::JoinPending&) {},
        [&](msg::JoinAccepted& x) { x.peerId = r.u32(); x.token = r.u64(); x.roomName = r.str(kMaxRoomName * 4); },
        [&](msg::JoinRejected& x) { x.reason = r.str(256); },
        [&](msg::PeerList& x) {
            uint32_t n = r.count(9, kMaxPeers);
            x.peers.resize(n);
            for (auto& p : x.peers) {
                p.id = r.u32();
                p.nickname = r.str(64);
                uint8_t role = r.u8();
                if (role > 2) r.ok = false;
                p.role = static_cast<Role>(role);
            }
        },
        [&](msg::OpsBroadcast& x) { x.firstSeq = r.u64(); x.fromPeer = r.u32(); x.clientSeq = r.u32(); readOps(r, x.ops); },
        [&](msg::Correction& x) { readOps(r, x.ops); x.reason = r.str(256); },
        [&](msg::LockState& x) { readGroup(r, x.group); },
        [&](msg::Checksum& x) { x.seq = r.u64(); x.hash = r.u64(); x.count = r.u32(); },
        [&](msg::Snapshot& x) {
            x.seq = r.u64();
            x.header = r.str(kMaxHeaderString);
            uint32_t n = r.count(12, kMaxObjects);
            x.objects.resize(n);
            for (auto& [id, s] : x.objects) { id = r.u64(); s = r.str(kMaxObjectString); }
            uint32_t g = r.count(8, kMaxPeers);
            x.locks.resize(g);
            for (auto& grp : x.locks) readGroup(r, grp);
        },
        [&](msg::Kicked& x) { x.reason = r.str(256); },
        [&](msg::RoomClosed& x) { x.reason = r.str(256); },
        [&](msg::PlayState& x) { readPlay(r, x); },
        [&](msg::PlayStates& x) {
            uint32_t n = r.count(39, kMaxPeers * 64); // 39 = encoded size of one PlayState
            x.states.resize(n);
            for (auto& s : x.states) readPlay(r, s);
        },
    }, m);
    return r.done();
}

constexpr char kAnnounceMagic[4] = {'E', 'T', 'R', '1'};

} // namespace

std::string encode(Message const& m) {
    ByteWriter w;
    w.u8(static_cast<uint8_t>(m.index() + 1));
    writeBody(w, m);
    return std::move(w.buf);
}

std::optional<Message> decode(std::string_view payload) {
    ByteReader r(payload);
    uint8_t type = r.u8();
    if (!r.ok || type == 0) return std::nullopt;
    auto m = makeByIndex(type - 1);
    if (!m || !readBody(r, *m)) return std::nullopt;
    return m;
}

std::string encodeSnapshot(uint64_t seq, std::string const& header,
    std::unordered_map<uint64_t, std::string> const& objects, std::vector<LockGroup> const& locks) {
    size_t bytes = 32 + header.size();
    for (auto const& [id, s] : objects) bytes += 12 + s.size();
    ByteWriter w;
    w.buf.reserve(bytes);
    w.u8(static_cast<uint8_t>(Message(std::in_place_type<msg::Snapshot>).index() + 1));
    w.u64(seq);
    w.str(header);
    w.u32(static_cast<uint32_t>(objects.size()));
    for (auto const& [id, s] : objects) { w.u64(id); w.str(s); }
    w.u32(static_cast<uint32_t>(locks.size()));
    for (auto const& g : locks) writeGroup(w, g);
    return std::move(w.buf);
}

std::optional<uint16_t> peekHelloProtocol(std::string_view payload) {
    // Hello is the first message type and always starts with the protocol version.
    ByteReader r(payload);
    if (r.u8() != 1) return std::nullopt;
    uint16_t v = r.u16();
    if (!r.ok) return std::nullopt;
    return v;
}

std::string encodeAnnounce(Announce const& a) {
    ByteWriter w;
    w.buf.append(kAnnounceMagic, 4);
    w.u16(a.protocol);
    w.u64(a.roomId);
    w.u16(a.tcpPort);
    w.u8(a.players);
    w.u8(a.maxPlayers);
    w.u8((a.open ? 1 : 0) | (a.locked ? 2 : 0));
    w.str(a.roomName);
    w.str(a.hostName);
    w.str(a.levelName);
    return std::move(w.buf);
}

std::optional<Announce> decodeAnnounce(std::string_view data) {
    if (data.size() < 4 || data.substr(0, 4) != std::string_view(kAnnounceMagic, 4)) return std::nullopt;
    ByteReader r(data.substr(4));
    Announce a;
    a.protocol = r.u16(); // the announcement layout is the same in every version so far
    a.roomId = r.u64();
    a.tcpPort = r.u16();
    a.players = r.u8();
    a.maxPlayers = r.u8();
    uint8_t flags = r.u8();
    a.open = flags & 1;
    a.locked = flags & 2;
    a.roomName = sanitizeName(r.str(256), kMaxRoomName, "Room");
    a.hostName = sanitizeName(r.str(256), kMaxNickname, "Player");
    a.levelName = sanitizeName(r.str(256), kMaxLevelName, "");
    if (!r.done() || a.tcpPort == 0 || a.roomId == 0) return std::nullopt;
    return a;
}

bool isValidObjectString(std::string_view s) {
    if (s.empty() || s.size() > kMaxObjectString) return false;
    bool hasObjectId = false;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t kEnd = s.find(',', pos);
        if (kEnd == std::string_view::npos) return false; // key without value
        auto key = s.substr(pos, kEnd - pos);
        size_t vEnd = s.find(',', kEnd + 1);
        auto val = s.substr(kEnd + 1, (vEnd == std::string_view::npos ? s.size() : vEnd) - kEnd - 1);
        if (key.empty() || key.size() > 4) return false;
        for (char c : key)
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        for (char c : val)
            if (c == ';' || c == '\0') return false;
        if (key == "1") {
            if (val.empty() || val.size() > 5) return false;
            for (char c : val)
                if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            int objId = std::stoi(std::string(val));
            if (objId <= 0) return false;
            hasObjectId = true;
        }
        if (vEnd == std::string_view::npos) break;
        pos = vEnd + 1;
    }
    return hasObjectId;
}

bool isValidHeaderString(std::string_view s) {
    if (s.size() > kMaxHeaderString) return false;
    for (char c : s)
        if (c == ';' || c == '\0') return false;
    return true;
}

std::string normalizeHeader(std::string_view header) {
    std::string out;
    size_t pos = 0;
    while (pos < header.size()) {
        size_t kEnd = header.find(',', pos);
        if (kEnd == std::string_view::npos) break;
        size_t vEnd = header.find(',', kEnd + 1);
        if (vEnd == std::string_view::npos) vEnd = header.size();
        auto key = header.substr(pos, kEnd - pos);
        if (!key.starts_with("kI") && key != "kS39") { // kS39 = color page shown in the editor
            if (!out.empty()) out.push_back(',');
            out.append(header.substr(pos, vEnd - pos));
        }
        pos = vEnd + 1;
    }
    return out;
}

std::string sanitizeName(std::string_view s, size_t maxLen, std::string_view fallback) {
    std::string out;
    for (char c : s) {
        if (out.size() >= maxLen) break;
        if (c >= 32 && c < 127) out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string(fallback) : out;
}

} // namespace et
