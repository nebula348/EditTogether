#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace et {

constexpr uint16_t kProtocolVersion = 7;
constexpr uint16_t kDiscoveryPort = 47811;
constexpr uint16_t kDefaultTcpPort = 47812;

// Frame size limits (bytes of payload after the 4-byte length prefix).
constexpr uint32_t kMaxFramePending = 4 * 1024;          // before the host approved the peer
constexpr uint32_t kMaxFrameClientToHost = 1024 * 1024;  // ops / locks from a member
constexpr uint32_t kMaxFrameHostToClient = 64 * 1024 * 1024; // snapshots of big levels

constexpr size_t kMaxNickname = 20;
constexpr size_t kMaxRoomName = 32;
constexpr size_t kMaxLevelName = 64;
constexpr size_t kMaxPassword = 32;
constexpr size_t kMaxObjectString = 64 * 1024;
constexpr size_t kMaxHeaderString = 512 * 1024;
constexpr uint32_t kMaxOpsPerMessage = 5000;
constexpr uint32_t kMaxLockIds = 20000;
constexpr uint32_t kMaxObjects = 400000;
// Hard cap on room size, including the host. Not configurable beyond this.
constexpr uint32_t kMaxPeers = 100;

enum class Role : uint8_t { Admin = 0, Editor = 1, Viewer = 2 };
char const* roleName(Role r);
inline bool canEdit(Role r) { return r != Role::Viewer; }

enum class OpKind : uint8_t { Create = 1, Modify = 2, Delete = 3, Header = 4 };

// One structured editing operation. `data` is the GD object save string (or level header for Header).
struct Op {
    OpKind kind{};
    uint64_t id = 0;
    std::string data;
    bool operator==(Op const&) const = default;
};

struct PeerEntry {
    uint32_t id = 0;
    std::string nickname;
    Role role = Role::Editor;
    bool operator==(PeerEntry const&) const = default;
};

struct LockGroup {
    uint32_t owner = 0;
    std::vector<uint64_t> ids;
    bool operator==(LockGroup const&) const = default;
};

namespace msg {
    // client -> host
    struct Hello { uint16_t protocol = kProtocolVersion; uint64_t roomId = 0; std::string nickname; uint64_t token = 0; std::string password; bool operator==(Hello const&) const = default; };
    struct Ops { uint32_t clientSeq = 0; std::vector<Op> ops; bool operator==(Ops const&) const = default; };
    struct LockSet { std::vector<uint64_t> ids; bool operator==(LockSet const&) const = default; };
    struct SnapshotRequest { bool operator==(SnapshotRequest const&) const = default; };
    struct Leave { bool operator==(Leave const&) const = default; };
    // both directions
    struct Ping { uint64_t time = 0; bool operator==(Ping const&) const = default; };
    struct Pong { uint64_t time = 0; bool operator==(Pong const&) const = default; };
    // host -> client
    struct JoinPending { bool operator==(JoinPending const&) const = default; };
    struct JoinAccepted { uint32_t peerId = 0; uint64_t token = 0; std::string roomName; bool operator==(JoinAccepted const&) const = default; };
    struct JoinRejected { std::string reason; bool operator==(JoinRejected const&) const = default; };
    struct PeerList { std::vector<PeerEntry> peers; bool operator==(PeerList const&) const = default; };
    // Accepted ops, in authoritative order. Op i has global sequence number firstSeq + i.
    struct OpsBroadcast { uint64_t firstSeq = 0; uint32_t fromPeer = 0; uint32_t clientSeq = 0; std::vector<Op> ops; bool operator==(OpsBroadcast const&) const = default; };
    // Authoritative state for ops of yours the host rejected. Not sequenced.
    struct Correction { std::vector<Op> ops; std::string reason; bool operator==(Correction const&) const = default; };
    struct LockState { LockGroup group; bool operator==(LockState const&) const = default; };
    struct Checksum { uint64_t seq = 0; uint64_t hash = 0; uint32_t count = 0; bool operator==(Checksum const&) const = default; };
    struct Snapshot {
        uint64_t seq = 0;
        std::string header;
        std::vector<std::pair<uint64_t, std::string>> objects;
        std::vector<LockGroup> locks;
        bool operator==(Snapshot const&) const = default;
    };
    struct Kicked { std::string reason; bool operator==(Kicked const&) const = default; };
    struct RoomClosed { std::string reason; bool operator==(RoomClosed const&) const = default; };
    // Someone is playing the level (editor playtest or the level itself). Client->host: peer is ignored;
    // host->clients: peer = who is playing. Sent ~30 times per second while playing, once with playing=false.
    // Carries what's needed to draw their actual player: game mode, icon, colors, rotation, size, gravity.
    struct PlayState {
        enum Flags : uint8_t { UpsideDown = 1, Glow = 2, FacingLeft = 4 };
        uint32_t peer = 0;
        bool playing = false;
        uint64_t time = 0; // sender's monotonic clock, ms; lets receivers undo network jitter
        uint8_t life = 0;  // bumped on every respawn / restart: never interpolate across a change
        float x = 0, y = 0;
        float rotation = 0;
        float scale = 1;
        uint8_t mode = 0;   // IconType: 0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing
        uint16_t icon = 1;
        uint16_t cube = 1; // cube icon, drawn riding the ship / UFO
        uint8_t color1 = 0, color2 = 3, glowColor = 0;
        uint8_t flags = 0;
        bool operator==(PlayState const&) const = default;
    };
    // Host -> clients: every player's samples since the last batch, sent ~20 times per second.
    struct PlayStates { std::vector<PlayState> states; bool operator==(PlayStates const&) const = default; };
}

using Message = std::variant<
    msg::Hello, msg::Ops, msg::LockSet, msg::SnapshotRequest, msg::Leave,
    msg::Ping, msg::Pong,
    msg::JoinPending, msg::JoinAccepted, msg::JoinRejected, msg::PeerList,
    msg::OpsBroadcast, msg::Correction, msg::LockState, msg::Checksum,
    msg::Snapshot, msg::Kicked, msg::RoomClosed, msg::PlayState, msg::PlayStates>;

std::string encode(Message const& m);
// Same bytes as encode(msg::Snapshot{...}) but straight from the store, without copying every object first.
std::string encodeSnapshot(uint64_t seq, std::string const& header,
    std::unordered_map<uint64_t, std::string> const& objects, std::vector<LockGroup> const& locks);
// Returns nullopt on any malformed, truncated, oversized or trailing-garbage payload.
std::optional<Message> decode(std::string_view payload);
// The protocol version of a Hello, readable even when the rest of it is another version's layout.
std::optional<uint16_t> peekHelloProtocol(std::string_view payload);

// UDP room announcement.
struct Announce {
    uint16_t protocol = kProtocolVersion; // rooms of other versions are listed but can't be joined
    uint64_t roomId = 0;
    uint16_t tcpPort = 0;
    uint8_t players = 0;
    uint8_t maxPlayers = 0;
    bool open = true;
    bool locked = false; // needs a password
    std::string roomName;
    std::string hostName;
    std::string levelName;
    bool operator==(Announce const&) const = default;
};
std::string encodeAnnounce(Announce const& a);
std::optional<Announce> decodeAnnounce(std::string_view data);

// Validation of GD strings coming from the network.
bool isValidObjectString(std::string_view s);
bool isValidHeaderString(std::string_view s);
// Strips editor-view keys (camera position, zoom, build tab page: "kI*"; color page: "kS39")
// so scrolling around isn't treated as a level change.
std::string normalizeHeader(std::string_view header);
// Printable ASCII only, trimmed to maxLen; falls back when empty.
std::string sanitizeName(std::string_view s, size_t maxLen, std::string_view fallback);

inline uint32_t idOwner(uint64_t id) { return static_cast<uint32_t>(id >> 32); }
inline uint64_t makeId(uint32_t peer, uint32_t counter) { return (static_cast<uint64_t>(peer) << 32) | counter; }

} // namespace et
