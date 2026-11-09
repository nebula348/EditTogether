#pragma once

#include "EditorSync.hpp"
#include "core/ClientCore.hpp"
#include "core/HostCore.hpp"
#include "net/Discovery.hpp"
#include "net/Socket.hpp"
#include "net/ApinatorClient.hpp"
#include "net/RelayTransport.hpp"

#include <Geode/Geode.hpp>

#include <atomic>
#include <deque>
#include <memory>

namespace et {

// The one collaborative session of this device: hosting a room or a member of one.
// Everything runs on the main thread with non-blocking sockets, ticked by the cocos scheduler.
class Session : public cocos2d::CCObject {
public:
    enum class Mode { None, Hosting, Connecting, Pending, Joined, Reconnecting };

    static Session* get();
    // Logged-in GD account username; empty when not logged in (rooms require an account).
    static std::string accountName();

    // Host
    struct RoomOptions {
        std::string name;
        int maxPlayers = 4;
        std::string password; // empty = none; never stored on disk
        bool online = false;  // also accept players anywhere in the world, through the relay (needs internet)
    };
    bool createRoom(RoomOptions const& opts, std::string& error);
    bool hasPassword() const { return !m_password.empty(); }
    // Online (worldwide) hosting: the room code players type to join; empty until the relay service has answered.
    bool onlineHosting() const { return m_onlineHosting; }
    std::string const& onlineCode() const { return m_onlineCode; }
    std::string onlineStatus() const; // relay connection state, for the UI
    void closeRoom();
    HostCore* host() { return m_host.get(); }
    uint16_t hostPort() const { return m_listener.port(); }

    // Client
    // saveCurrent: the player chose to save the level they have open before it's replaced by the room's copy.
    void join(net::Endpoint hostEp, uint64_t roomId, std::string const& roomName, std::string const& password, bool saveCurrent);
    // Joins a room by its online code (worldwide, through the relay).
    void joinOnline(std::string const& code, std::string const& password, bool saveCurrent);
    void leave();
    void requestResync();

    Mode mode() const { return m_mode; }
    bool active() const { return m_mode != Mode::None; }
    ClientCore const& client() const { return m_client; }
    EditorSync const& sync() const { return m_sync; }
    std::string const& roomName() const { return m_roomName; }
    std::string const& status() const { return m_status; }
    std::string adminName() const;
    uint64_t version() const { return m_version; }
    std::vector<std::string> diagnostics() const;

    // Hooks
    void onEditorInit(LevelEditorLayer* lel);
    EditorSync& syncMut() { return m_sync; }

    void update(float dt) override;

private:
    Session();
    double now() const;
    void bump() { ++m_version; }
    void setStatus(std::string s);
    void notify(std::string const& text, geode::NotificationIcon icon);
    void dbg(std::string const& text) const;
    void end(std::string const& reason, bool error);

    void updateHost(double t);
    void updateClient(double t);
    void pumpLocal(double t);
    void handleHostEvents();
    void handleActions(std::vector<ClientCore::Action> actions);
    void applyOrDefer(ClientCore::Action a);
    void startReload();
    void finishReload();
    void tryReconnect(double t);
    void saveCurrentLevel();
    std::unique_ptr<net::Link> openLink(std::string& error);
    void beginJoin(uint64_t roomId, std::string const& roomName, std::string const& password, bool saveCurrent);
    bool usesRelay() const;
    void rebind();
    bool playingRoomLevel() const;
    void publishPlayState(double t);
    void startOnlineHosting();
    void pollOnline(double t);
    void stopOnline();
    std::string relayUrl() const;

    Mode m_mode = Mode::None;
    std::string m_status;
    uint64_t m_version = 0;
    std::string m_roomName;
    std::string m_lastError;

    // hosting
    std::unique_ptr<HostCore> m_host;
    net::TcpListener m_listener;
    net::RoomAnnouncer m_announcer;
    std::map<uint32_t, std::unique_ptr<net::Link>> m_conns;
    std::vector<std::pair<std::unique_ptr<net::Link>, double>> m_closing; // flushed before closing

    // member (also the host's own editor, as peer 1)
    ClientCore m_client;
    EditorSync m_sync;
    std::unique_ptr<net::Link> m_conn;
    std::string m_password;
    // worldwide play (Apinator relay)
    bool m_online = false;         // member: joined by room code
    bool m_onlineHosting = false;  // host: also reachable by room code
    std::string m_onlineCode;
    std::string m_lastRelayStatus;
    std::string m_secret;          // member: proves ownership of its private channel; kept across reconnects
    std::unique_ptr<net::ApinatorClient> m_relay;
    std::unique_ptr<net::RelayHost> m_relayHost;
    struct RoomJob {
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        net::RoomGrant grant;
    };
    std::shared_ptr<RoomJob> m_roomJob; // asking the relay service for a room code (network thread)
    std::vector<std::pair<std::shared_ptr<void>, double>> m_graveyard; // relays kept alive while closing links flush
    net::Endpoint m_hostEp;
    uint64_t m_roomId = 0;
    double m_reconnectUntil = 0;
    double m_nextReconnect = 0;
    std::vector<PeerEntry> m_knownPeers;

    // editor rebuild from snapshot
    geode::Ref<GJGameLevel> m_level;
    std::deque<ClientCore::Action> m_deferred;
    std::optional<ObjectStore> m_reloadStore;
    geode::Ref<LevelEditorLayer> m_pendingEditor;
    bool m_reloading = false;
    bool m_reloadWanted = false;
    bool m_rebuildWanted = false;
    bool m_hadEditor = false;
    bool m_wasPlaying = false;
    double m_lastPlaySent = 0;
    double m_lastRotationLog = 0;
    bool m_wasDead = false;
    uint8_t m_playLife = 0;
    msg::PlayState m_lastPlay;

    double m_lastReloadAt = -100;
    int m_bindFailures = 0;
    double m_lastLabels = 0;

    double m_editorGoneSince = -1;
    uint64_t m_opsApplied = 0;
};

} // namespace et
