#include "Session.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

using namespace geode::prelude;

namespace et {

namespace {

std::string nickname() {
    return sanitizeName(Session::accountName(), kMaxNickname, "Player");
}

uint64_t random64() {
    std::random_device rd;
    uint64_t v = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    return v ? v : 1;
}

} // namespace

std::string Session::accountName() {
    auto am = GJAccountManager::get();
    if (!am || am->m_accountID <= 0) return {};
    return sanitizeName(std::string(am->m_username.c_str()), kMaxNickname, "");
}

Session* Session::get() {
    static Session* s = new Session(); // lives for the whole process
    return s;
}

Session::Session() {
    CCDirector::get()->getScheduler()->scheduleUpdateForTarget(this, 0, false);
}

double Session::now() const {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Session::setStatus(std::string s) {
    m_status = std::move(s);
    bump();
}

void Session::notify(std::string const& text, NotificationIcon icon) {
    log::info("EditTogether: {}", text);
    Notification::create(text, icon)->show();
}

void Session::dbg(std::string const& text) const {
    if (Mod::get()->getSettingValue<bool>("debug-logging")) log::info("EditTogether [debug]: {}", text);
}

std::string Session::adminName() const {
    for (auto const& p : m_client.peers())
        if (p.role == Role::Admin) return p.nickname;
    return m_mode == Mode::Hosting ? nickname() : "-";
}

// ------------------------------------------------------------------ hosting

bool Session::createRoom(RoomOptions const& opts, std::string& error) {
    if (active()) { error = "You are already in a room."; return false; }
    if (accountName().empty()) { error = "Log in to your Geometry Dash account first. Your username is your name in rooms."; return false; }
    auto lel = LevelEditorLayer::get();
    if (!lel || !lel->m_level) { error = "Open a level in the editor first."; return false; }
    if (lel->m_playbackMode != PlaybackMode::Not) { error = "Stop playtesting first."; return false; }
    if (opts.online && relayUrl().empty()) { error = "Online play needs the relay service address (Online Relay URL in the mod settings)."; return false; }

    if (!m_listener.open(kDefaultTcpPort, error)) {
        error = "Could not open a network port: " + error;
        return false;
    }
    std::string announceErr;
    if (!m_announcer.start(announceErr)) {
        m_lastError = "Room announcements unavailable (" + announceErr + "); players must use Connect by IP.";
        notify(m_lastError, NotificationIcon::Warning);
    }

    m_sync.attach(lel);
    ObjectStore seed = m_sync.adoptAll(HostCore::kLocalPeer);
    m_roomId = random64();
    m_roomName = sanitizeName(opts.name, kMaxRoomName, "Room");
    m_password = opts.password.substr(0, kMaxPassword);
    double t = now();
    m_host = std::make_unique<HostCore>(
        HostCore::Config{m_roomId, m_roomName, nickname(), opts.maxPlayers, m_password}, seed, t, random64()
    );
    m_client.startLocal(HostCore::kLocalPeer, std::move(seed), m_roomName, t);
    m_level = lel->m_level;
    m_hadEditor = true;
    m_mode = Mode::Hosting;
    m_editorGoneSince = -1;
    setStatus("Hosting");
    notify("Room created: " + m_roomName, NotificationIcon::Success);
    if (opts.online) startOnlineHosting();
    return true;
}

// ------------------------------------------------------------------ online hosting (Apinator relay)

std::string Session::relayUrl() const {
    return Mod::get()->getSettingValue<std::string>("relay-url");
}

void Session::startOnlineHosting() {
    auto job = std::make_shared<RoomJob>();
    m_roomJob = job;
    m_onlineHosting = true;
    m_onlineCode.clear();
    std::string url = relayUrl();
    // The relay service is contacted off the main thread; the answer is picked up in pollOnline().
    std::thread([job, url] {
        auto grant = net::requestRoom(url);
        job->grant = std::move(grant);
        job->done = true;
    }).detach();
    bump();
}

void Session::pollOnline(double t) {
    (void)t;
    if (m_roomJob && m_roomJob->done) {
        auto job = std::exchange(m_roomJob, nullptr);
        if (!job->grant.error.empty()) {
            m_lastError = "Online: " + job->grant.error;
            notify("Online play unavailable: " + job->grant.error, NotificationIcon::Warning);
            m_onlineHosting = false;
        } else {
            m_onlineCode = job->grant.code;
            m_relay = std::make_unique<net::ApinatorClient>(net::ApinatorClient::Config{relayUrl()});
            m_relayHost = std::make_unique<net::RelayHost>(*m_relay, m_onlineCode, job->grant.hostToken, [this] { return now(); });
            m_host->setPlayFlush(0.1); // 10 Hz over the relay (the free plan has a message budget)
            notify("Online room code: " + m_onlineCode, NotificationIcon::Success);
        }
        bump();
    }
    if (m_relayHost) {
        m_relayHost->pump();
        while (auto c = m_relayHost->accept()) {
            if (m_conns.size() >= kMaxPeers + HostCore::kMaxPending) continue; // dropped: closes on destruction
            c->setMaxFrame(kMaxFramePending);
            uint32_t id = m_host->onConnect(now());
            dbg("online connection -> peer " + std::to_string(id));
            m_conns[id] = std::move(c);
        }
    }
}

void Session::stopOnline() {
    if (m_roomJob) m_roomJob->cancelled = true;
    m_roomJob = nullptr;
    // The relay and transport stay alive for a moment so the goodbye messages of closing links still go out.
    struct Holder {
        std::unique_ptr<net::ApinatorClient> relay;
        std::unique_ptr<net::RelayHost> host;
    };
    if (m_relay || m_relayHost) {
        auto h = std::make_shared<Holder>();
        h->relay = std::move(m_relay);
        h->host = std::move(m_relayHost);
        m_graveyard.emplace_back(std::move(h), now());
    }
    m_onlineHosting = false;
    m_online = false;
    m_onlineCode.clear();
    m_secret.clear();
}

std::string Session::onlineStatus() const {
    if (!m_onlineHosting && !m_online) return {};
    if (m_roomJob) return "Asking the relay service for a room code";
    if (!m_relay) return m_lastError.empty() ? "Not connected" : m_lastError;
    std::string s = m_relay->status();
    if (m_relay->connected() && m_relay->pingMs() >= 0) s += fmt::format(" ({:.0f} ms to Apinator)", m_relay->pingMs());
    return s;
}

void Session::closeRoom() {
    if (m_mode != Mode::Hosting) return;
    m_host->close("The host closed the room");
    pumpLocal(now());
    end("Room closed", false);
}

void Session::pumpLocal(double t) {
    for (int i = 0; i < 16; ++i) {
        bool any = false;
        for (auto& m : m_client.takeOutbox()) {
            any = true;
            m_host->onMessage(HostCore::kLocalPeer, m, t);
        }
        for (auto& o : m_host->takeOutbox()) {
            any = true;
            if (o.peer == HostCore::kLocalPeer) {
                if (!o.bytes.empty()) m_client.onMessage(o.bytes, t);
                continue;
            }
            auto it = m_conns.find(o.peer);
            if (it == m_conns.end()) continue;
            if (!o.bytes.empty()) it->second->send(o.bytes);
            if (o.closeAfter) {
                m_closing.emplace_back(std::move(it->second), t);
                m_conns.erase(it);
            }
        }
        if (!any) break;
    }
}

void Session::updateHost(double t) {
    auto admit = [&](std::unique_ptr<net::Link> c) {
        if (m_conns.size() >= kMaxPeers + HostCore::kMaxPending) return; // dropped: closes on destruction
        c->setMaxFrame(kMaxFramePending);
        uint32_t id = m_host->onConnect(t);
        dbg("connection from " + c->describe() + " -> peer " + std::to_string(id));
        m_conns[id] = std::move(c);
    };
    while (auto c = m_listener.accept()) admit(std::move(c));
    std::vector<std::string> frames;
    for (auto it = m_conns.begin(); it != m_conns.end();) {
        frames.clear();
        bool ok = it->second->update(frames);
        uint32_t id = it->first;
        for (auto& f : frames) m_host->onMessage(id, f, t);
        if (!ok) {
            dbg("peer " + std::to_string(id) + " transport closed: " + it->second->error());
            m_host->onDisconnect(id, it->second->error());
            it = m_conns.erase(it);
            continue;
        }
        auto p = m_host->peers().find(id);
        if (p != m_host->peers().end() && p->second.active) it->second->setMaxFrame(kMaxFrameClientToHost);
        ++it;
    }

    m_sync.tick(m_client, t);
    pumpLocal(t);
    m_host->tick(t);
    m_client.tick(t);
    pumpLocal(t);
    handleActions(m_client.takeActions());
    handleHostEvents();

    Announce a;
    a.roomId = m_roomId;
    a.tcpPort = m_listener.port();
    a.players = static_cast<uint8_t>(m_host->activeCount());
    a.maxPlayers = static_cast<uint8_t>(m_host->config().maxPlayers);
    a.open = m_host->activeCount() < m_host->config().maxPlayers;
    a.locked = !m_password.empty();
    a.roomName = m_roomName;
    a.hostName = m_host->config().hostName;
    if (m_level) a.levelName = sanitizeName(std::string(m_level->m_levelName.c_str()), kMaxLevelName, "");
    m_announcer.update(t, a);
    pollOnline(t);
}

void Session::handleHostEvents() {
    for (auto& e : m_host->takeEvents()) {
        switch (e.kind) {
            case HostCore::EventKind::JoinRequest:
                notify(e.nickname + " wants to join your room", NotificationIcon::Info);
                break;
            case HostCore::EventKind::PeerJoined:
                notify(e.nickname + (e.detail == "reconnected" ? " reconnected" : " joined the room"), NotificationIcon::Success);
                break;
            case HostCore::EventKind::PeerLeft:
                notify(e.nickname + " disconnected (" + e.detail + ")", NotificationIcon::Warning);
                break;
            case HostCore::EventKind::RateLimited:
                notify(e.nickname + " is sending changes too fast; resynchronizing them", NotificationIcon::Warning);
                break;
        }
        bump();
    }
}

// ------------------------------------------------------------------ joining

void Session::saveCurrentLevel() {
    auto lel = LevelEditorLayer::get();
    if (!lel || !lel->m_level) return;
    // Same data GD writes when saving from the pause menu, so the level the player was editing isn't lost.
    lel->m_level->m_levelString = ZipUtils::compressString(lel->getLevelString(), false, 0);
}

std::unique_ptr<net::Link> Session::openLink(std::string& error) {
    if (m_online) {
        if (!m_relay) {
            error = "No relay connection";
            return nullptr;
        }
        return net::connectToRoom(*m_relay, m_onlineCode, m_secret, [this] { return now(); });
    }
    return net::TcpConn::connect(m_hostEp, error);
}

bool Session::usesRelay() const {
    if (m_conn && m_conn->viaRelay()) return true;
    return std::any_of(m_conns.begin(), m_conns.end(), [](auto const& kv) { return kv.second->viaRelay(); });
}

void Session::join(net::Endpoint hostEp, uint64_t roomId, std::string const& roomName, std::string const& password, bool saveCurrent) {
    m_hostEp = hostEp;
    m_online = false;
    beginJoin(roomId, roomName, password, saveCurrent);
}

void Session::joinOnline(std::string const& code, std::string const& password, bool saveCurrent) {
    if (active()) return;
    std::string url = relayUrl();
    if (url.empty()) {
        FLAlertLayer::create("EditTogether", "Online play needs the relay service address (Online Relay URL in the mod settings).", "OK")->show();
        return;
    }
    if (accountName().empty()) { // same message as beginJoin, but before any connection is opened
        FLAlertLayer::create("EditTogether", "Log in to your Geometry Dash account first. Your username is your name in rooms.", "OK")->show();
        return;
    }
    m_hostEp = {};
    m_online = true;
    m_onlineCode = net::normalizeCode(code);
    m_secret = net::newSecret();
    m_relay = std::make_unique<net::ApinatorClient>(net::ApinatorClient::Config{url});
    beginJoin(0, "Room " + m_onlineCode, password, saveCurrent);
}

void Session::beginJoin(uint64_t roomId, std::string const& roomName, std::string const& password, bool saveCurrent) {
    if (active()) return;
    if (accountName().empty()) {
        FLAlertLayer::create("EditTogether", "Log in to your Geometry Dash account first. Your username is your name in rooms.", "OK")->show();
        return;
    }
    // Only with the player's explicit consent; otherwise their level stays exactly as last saved.
    if (saveCurrent) saveCurrentLevel();
    std::string err;
    m_conn = openLink(err);
    if (!m_conn) {
        notify("Could not connect: " + err, NotificationIcon::Error);
        stopOnline();
        return;
    }
    m_conn->setMaxFrame(kMaxFrameHostToClient);
    m_roomId = roomId;
    m_roomName = roomName;
    m_password = password.substr(0, kMaxPassword);
    m_bindFailures = 0;
    m_knownPeers.clear();
    m_level = nullptr;
    m_client.start(roomId, nickname(), 0, now(), m_password);
    m_mode = Mode::Connecting;
    setStatus("Connecting to " + m_conn->describe() + "...");
}

void Session::leave() {
    if (m_mode == Mode::Hosting) return closeRoom();
    if (!active()) return;
    m_client.leave();
    if (m_conn)
        for (auto& m : m_client.takeOutbox()) m_conn->send(m);
    end("You left the room", false);
}

void Session::requestResync() {
    if (m_mode != Mode::Joined) return;
    m_client.requestSnapshot();
    notify("Resynchronizing with the host...", NotificationIcon::Loading);
}

void Session::tryReconnect(double t) {
    if (t > m_reconnectUntil) return end("Connection lost. Could not reconnect to the host.", true);
    if (m_conn || t < m_nextReconnect) return;
    m_nextReconnect = t + 2.0;
    std::string err;
    m_conn = openLink(err);
    if (!m_conn) return dbg("reconnect failed: " + err);
    m_conn->setMaxFrame(kMaxFrameHostToClient);
    uint64_t token = m_client.token();
    m_client.start(m_roomId, nickname(), token, t, m_password);
}

void Session::updateClient(double t) {
    if (m_mode == Mode::Reconnecting) tryReconnect(t);
    if (!active()) return;
    if (m_conn) {
        std::vector<std::string> frames;
        bool ok = m_conn->update(frames);
        for (auto& f : frames) m_client.onMessage(f, t);
        if (!ok) {
            std::string err = m_conn->error();
            m_conn.reset();
            if (m_client.state() != ClientCore::State::Closed || m_mode == Mode::Reconnecting) {
                // Transport died without a goodbye from the host.
                handleActions(m_client.takeActions());
                if (!active()) return;
                handleActions({{ClientCore::ActionKind::ConnectionLost, {}, false, err}});
                return;
            }
        }
    }
    if (m_mode == Mode::Joined && !m_reloading) m_sync.tick(m_client, t);
    m_client.tick(t);
    auto out = m_client.takeOutbox();
    if (m_conn)
        for (auto& m : out) m_conn->send(m);
    handleActions(m_client.takeActions());
}

// ------------------------------------------------------------------ actions

void Session::handleActions(std::vector<ClientCore::Action> actions) {
    using K = ClientCore::ActionKind;
    for (auto& a : actions) {
        if (!active()) return;
        switch (a.kind) {
            case K::Pending:
                if (m_mode == Mode::Connecting) {
                    m_mode = Mode::Pending;
                    setStatus("Waiting for the administrator to accept your request");
                    notify("Join request sent", NotificationIcon::Info);
                }
                break;
            case K::Joined:
                notify(m_mode == Mode::Reconnecting ? "Reconnected" : "Request accepted", NotificationIcon::Success);
                m_mode = Mode::Joined;
                m_roomName = m_client.roomName();
                m_knownPeers.clear();
                setStatus("Connected");
                break;
            case K::Rejected:
                return end("Join request rejected: " + a.text, true);
            case K::Kicked:
                return end("Removed from the room: " + a.text, true);
            case K::RoomClosed:
                return end("Room closed: " + a.text, true);
            case K::ConnectionLost:
                if ((m_mode == Mode::Joined || m_mode == Mode::Reconnecting) && m_client.token() != 0) {
                    if (m_mode == Mode::Joined) {
                        notify("Connection lost. Reconnecting...", NotificationIcon::Warning);
                        m_mode = Mode::Reconnecting;
                        m_reconnectUntil = now() + 60.0;
                        m_nextReconnect = now() + 1.0;
                        setStatus("Connection lost. Reconnecting...");
                    }
                    m_conn.reset();
                    break;
                }
                return end((m_mode == Mode::Hosting ? "Room error: " : "Could not connect: ") + a.text, true);
            case K::LoadSnapshot:
                // While the player is in the level itself (not the editor), the store is already up to date;
                // the editor is rebuilt from it when they come back.
                if (m_mode != Mode::Hosting && (m_sync.attached() || !m_hadEditor)) startReload();
                break;
            case K::ApplyOps:
            case K::Correction:
                applyOrDefer(std::move(a));
                break;
            case K::SyncError:
                notify("Synchronization error: " + a.text + ". Resynchronizing...", NotificationIcon::Warning);
                break;
            case K::LocksChanged:
                bump();
                break;
            case K::PeersChanged: {
                if (m_mode != Mode::Hosting && !m_knownPeers.empty()) {
                    auto has = [](auto const& v, uint32_t id) {
                        return std::any_of(v.begin(), v.end(), [&](auto const& p) { return p.id == id; });
                    };
                    for (auto const& p : m_client.peers())
                        if (!has(m_knownPeers, p.id)) notify(p.nickname + " joined the room", NotificationIcon::Info);
                    for (auto const& p : m_knownPeers)
                        if (!has(m_client.peers(), p.id)) notify(p.nickname + " disconnected", NotificationIcon::Warning);
                }
                m_knownPeers = m_client.peers();
                bump();
                break;
            }
            case K::RoleChanged:
                notify(std::string("Your role is now ") + a.text, NotificationIcon::Info);
                bump();
                break;
        }
    }
}

void Session::applyOrDefer(ClientCore::Action a) {
    if (!m_deferred.empty() || m_reloading || !m_sync.ready()) {
        m_deferred.push_back(std::move(a));
        if (m_deferred.size() > 20000) { // away for long: rebuild from the (always current) store instead
            m_deferred.clear();
            m_rebuildWanted = true;
        }
        return;
    }
    bool correction = a.kind == ClientCore::ActionKind::Correction;
    m_sync.applyOps(a.ops, a.own, correction);
    m_opsApplied += a.ops.size();
    if (correction && !a.text.empty()) notify("Change rejected: " + a.text, NotificationIcon::Warning);
    if (m_sync.takeReloadRequest()) m_reloadWanted = true;
}

// ------------------------------------------------------------------ editor rebuild

void Session::startReload() {
    m_lastReloadAt = now();
    if (!m_level) {
        auto level = GameLevelManager::get()->createNewLevel();
        if (!level) return end("Could not create a local copy of the shared level", true);
        level->m_levelName = gd::string(sanitizeName("ET " + m_roomName, 20, "EditTogether").c_str());
        m_level = level;
    }
    m_reloadStore = m_client.store;
    std::vector<std::pair<uint64_t, std::string const*>> objs;
    objs.reserve(m_reloadStore->objects.size());
    for (auto const& [id, s] : m_reloadStore->objects) objs.emplace_back(id, &s);
    std::sort(objs.begin(), objs.end()); // bindTo relies on this order for objects GD re-serializes differently
    std::string str = m_reloadStore->header + ";";
    for (auto const& [id, s] : objs) {
        str += *s;
        str += ';';
    }
    m_level->m_levelString = ZipUtils::compressString(gd::string(str.c_str(), str.size()), false, 0);

    m_sync.detach();
    m_reloading = true;
    m_pendingEditor = nullptr;
    setStatus("Synchronizing level...");

    auto scene = CCScene::create();
    auto win = CCDirector::get()->getWinSize();
    auto bg = CCLayerColor::create({0, 0, 0, 255});
    scene->addChild(bg);
    auto label = CCLabelBMFont::create("Synchronizing level...", "goldFont.fnt");
    label->setPosition({win.width / 2.f, win.height / 2.f});
    scene->addChild(label);
    Ref<GJGameLevel> level = m_level;
    // Let the old editor scene be fully destroyed before the new one is created.
    scene->runAction(CCSequence::create(
        CCDelayTime::create(0.3f),
        CallFuncExt::create([level] { CCDirector::get()->replaceScene(LevelEditorLayer::scene(level, false)); }),
        nullptr
    ));
    CCDirector::get()->replaceScene(scene);
}

void Session::onEditorInit(LevelEditorLayer* lel) {
    // Our own rebuild, or the player coming back to the editor after playing the room's level.
    if (m_reloading || ((m_mode == Mode::Hosting || m_mode == Mode::Joined) && m_level && lel->m_level == m_level))
        m_pendingEditor = lel;
}

void Session::rebind() {
    // Back from playing: the editor loaded the level as saved before playing. Try to match it to the
    // current room state; if anything changed meanwhile, rebuild from the room state.
    m_sync.attach(m_pendingEditor);
    m_pendingEditor = nullptr;
    m_hadEditor = true;
    m_deferred.clear(); // already reflected in the store
    std::string err;
    bool headerSame = m_sync.headerMatches(m_client.store.header);
    if (m_rebuildWanted || !headerSame || !m_sync.bindTo(m_client.store, m_client.me(), err)) {
        m_rebuildWanted = false;
        dbg("rebuilding editor after returning: " + (err.empty() ? std::string("level changed") : err));
        startReload();
        return;
    }
    setStatus(m_mode == Mode::Hosting ? "Hosting" : "Connected");
}

void Session::finishReload() {
    m_sync.attach(m_pendingEditor);
    m_pendingEditor = nullptr;
    m_reloading = false;
    m_hadEditor = true;
    std::string err;
    if (!m_sync.bindTo(*m_reloadStore, m_client.me(), err)) {
        m_reloadStore.reset();
        ++m_bindFailures;
        log::error("EditTogether: binding failed: {}", err);
        if (m_mode == Mode::Hosting)
            return closeRoom(); // the host's own level is the source of truth; nothing to resync from
        if (m_bindFailures >= 3) return end("Could not synchronize the level (" + err + ")", true);
        notify("Synchronization error (" + err + "). Retrying...", NotificationIcon::Error);
        m_deferred.clear();
        m_client.requestSnapshot();
        return;
    }
    m_bindFailures = 0;
    m_reloadStore.reset();
    setStatus(m_mode == Mode::Hosting ? "Hosting" : "Connected");
}

// ------------------------------------------------------------------ teardown

void Session::end(std::string const& reason, bool error) {
    if (m_conn) m_closing.emplace_back(std::move(m_conn), now());
    for (auto& [id, c] : m_conns) m_closing.emplace_back(std::move(c), now());
    m_conns.clear();
    stopOnline();
    m_password.clear();
    m_listener = net::TcpListener();
    m_announcer = net::RoomAnnouncer();
    m_host.reset();
    m_client = ClientCore();
    m_sync.detach();
    m_deferred.clear();
    m_reloadStore.reset();
    m_reloading = false;
    m_reloadWanted = false;
    m_rebuildWanted = false;
    m_hadEditor = false;
    m_wasPlaying = false;
    m_pendingEditor = nullptr;
    m_level = nullptr;
    m_knownPeers.clear();
    // Session-specific diagnostics start fresh next time.
    m_opsApplied = 0;
    m_bindFailures = 0;
    m_lastError.clear();
    m_mode = Mode::None;
    setStatus("Not in a room");
    notify(reason, error ? NotificationIcon::Error : NotificationIcon::Info);
}

// ------------------------------------------------------------------ main loop

void Session::update(float) {
    double t = now();
    if (m_relay) { // refresh the Current Session window when the relay connection changes state
        auto st = m_relay->status();
        if (st != m_lastRelayStatus) {
            m_lastRelayStatus = st;
            bump();
        }
    }
    for (auto it = m_graveyard.begin(); it != m_graveyard.end();) {
        if (t - it->second > 3.0) it = m_graveyard.erase(it);
        else ++it;
    }
    for (auto it = m_closing.begin(); it != m_closing.end();) {
        std::vector<std::string> ignored;
        bool ok = it->first->update(ignored);
        if (!ok || it->first->outEmpty() || t - it->second > 2.0) it = m_closing.erase(it);
        else ++it;
    }
    if (!active()) return;

    if (m_mode == Mode::Hosting) updateHost(t);
    else updateClient(t);
    if (!active()) return;

    if (m_pendingEditor && m_pendingEditor->isRunning()) {
        if (m_reloading) finishReload();
        else rebind();
    }
    if (!active()) return;

    // Release a dead editor right away: holding it would delay its destructor until after GD built the next one.
    if (m_sync.attached() && !m_sync.alive() && !m_reloading) m_sync.detach();

    publishPlayState(t);

    // Leaving the editor ends the session (host: closes the room), except to play the room's level.
    bool inRoom = m_mode == Mode::Hosting || m_mode == Mode::Joined;
    bool away = inRoom && m_hadEditor && !m_reloading && !m_pendingEditor && !m_sync.attached() && !playingRoomLevel();
    if (away) {
        if (m_editorGoneSince < 0) m_editorGoneSince = t;
        else if (t - m_editorGoneSince > 1.5) {
            if (m_mode == Mode::Hosting) {
                m_host->close("The host left the editor");
                pumpLocal(t);
                return end("Room closed because you left the editor", false);
            }
            return leave();
        }
    } else {
        m_editorGoneSince = -1;
    }

    while (!m_deferred.empty() && !m_reloading && m_sync.ready()) {
        auto a = std::move(m_deferred.front());
        m_deferred.pop_front();
        bool correction = a.kind == ClientCore::ActionKind::Correction;
        m_sync.applyOps(a.ops, a.own, correction);
        m_opsApplied += a.ops.size();
        if (m_sync.takeReloadRequest()) m_reloadWanted = true;
    }
    // Level-settings changes rebuild the editor; cap that so a chatty setting can't cause a reload storm.
    if (m_reloadWanted && !m_reloading && t - m_lastReloadAt >= 10.0) {
        m_reloadWanted = false;
        startReload();
    }

    if (t - m_lastLabels > 0.25 && m_sync.ready()) {
        m_lastLabels = t;
        m_sync.updateLockLabels(m_client);
    }
    if (m_sync.alive()) m_sync.updatePlayMarkers(m_client, t);
    if (auto n = m_sync.takeNotice(); !n.empty()) {
        static double lastNotice = 0;
        if (t - lastNotice > 2.0) {
            lastNotice = t;
            notify(n, NotificationIcon::Warning);
        }
    }
}

// ------------------------------------------------------------------ playing

bool Session::playingRoomLevel() const {
    auto pl = PlayLayer::get();
    return pl && m_level && pl->m_level == m_level;
}

// Everything others need to draw this player the way GD draws them: mode, own icon and colors, motion.
static msg::PlayState describePlayer(PlayerObject* p) {
    auto gm = GameManager::get();
    msg::PlayState s;
    s.playing = true;
    auto pos = p->getPosition();
    s.x = pos.x;
    s.y = pos.y;
    s.rotation = p->getRotation() + (p->m_mainLayer ? p->m_mainLayer->getRotation() : 0.f);
    s.scale = p->m_vehicleSize > 0.05f ? p->m_vehicleSize : 1.f;
    int icon = gm->m_playerFrame;
    s.mode = 0;
    if (p->m_isShip) { s.mode = 1; icon = gm->m_playerShip; }
    else if (p->m_isBall) { s.mode = 2; icon = gm->m_playerBall; }
    else if (p->m_isBird) { s.mode = 3; icon = gm->m_playerBird; }
    else if (p->m_isDart) { s.mode = 4; icon = gm->m_playerDart; }
    else if (p->m_isRobot) { s.mode = 5; icon = gm->m_playerRobot; }
    else if (p->m_isSpider) { s.mode = 6; icon = gm->m_playerSpider; }
    else if (p->m_isSwing) { s.mode = 7; icon = gm->m_playerSwing; }
    s.icon = static_cast<uint16_t>(std::clamp(icon, 1, 2000));
    s.cube = static_cast<uint16_t>(std::clamp(static_cast<int>(gm->m_playerFrame), 1, 2000));
    s.color1 = static_cast<uint8_t>(static_cast<int>(gm->m_playerColor));
    s.color2 = static_cast<uint8_t>(static_cast<int>(gm->m_playerColor2));
    s.glowColor = static_cast<uint8_t>(static_cast<int>(gm->m_playerGlowColor));
    s.flags = (p->m_isUpsideDown ? msg::PlayState::UpsideDown : 0) | (gm->m_playerGlow ? msg::PlayState::Glow : 0)
            | (p->m_isGoingLeft ? msg::PlayState::FacingLeft : 0);
    return s;
}

void Session::publishPlayState(double t) {
    PlayerObject* player = nullptr;
    // Paused playtests count as playing: the player stays visible where they paused.
    if (m_sync.alive() && m_sync.layer()->m_playbackMode != PlaybackMode::Not) player = m_sync.layer()->m_player1;
    else if (playingRoomLevel()) player = PlayLayer::get()->m_player1;
    if (!player) {
        if (m_wasPlaying) m_client.sendPlayState(msg::PlayState{});
        m_wasPlaying = false;
        m_wasDead = false;
        return;
    }
    if (player->m_isDead) { // keep showing where they died until the respawn
        m_wasDead = true;
        return;
    }
    if (!m_wasPlaying || m_wasDead) ++m_playLife; // a new run or a respawn: receivers must not interpolate across it
    m_wasDead = false;
    auto s = describePlayer(player);
    s.time = static_cast<uint64_t>(t * 1000.0);
    s.life = m_playLife;
    bool moved = !m_wasPlaying || s.x != m_lastPlay.x || s.y != m_lastPlay.y || s.rotation != m_lastPlay.rotation
              || s.mode != m_lastPlay.mode || s.flags != m_lastPlay.flags || s.life != m_lastPlay.life;
    // 30 Hz while moving (10 Hz over the relay, which has a message budget); a 2 Hz keepalive while still.
    double interval = !moved ? 0.5 : usesRelay() ? 1.0 / 10.0 : 1.0 / 30.0;
    if (m_wasPlaying && t - m_lastPlaySent < interval) return;
    m_lastPlaySent = t;
    m_lastPlay = s;
    m_wasPlaying = true;
    m_client.sendPlayState(s);
    if (Mod::get()->getSettingValue<bool>("debug-logging") && t - m_lastRotationLog >= 0.5) {
        m_lastRotationLog = t;
        dbg(fmt::format("player rotation: node {:.1f} + mainLayer {:.1f}, scale {:.2f}/{:.2f}, mainLayer scale {:.2f}/{:.2f}",
            player->getRotation(), player->m_mainLayer ? player->m_mainLayer->getRotation() : 0.f,
            player->getScaleX(), player->getScaleY(),
            player->m_mainLayer ? player->m_mainLayer->getScaleX() : 0.f, player->m_mainLayer ? player->m_mainLayer->getScaleY() : 0.f));
    }
}

// ------------------------------------------------------------------ diagnostics

std::vector<std::string> Session::diagnostics() const {
    std::vector<std::string> out;
    char const* modes[] = {"None", "Hosting", "Connecting", "Waiting for approval", "Joined", "Reconnecting"};
    out.push_back(std::string("Mode: ") + modes[static_cast<int>(m_mode)]);
    auto itfs = net::localInterfaces();
    if (itfs.empty()) out.push_back("Local IPv4: none found (is Wi-Fi connected?)");
    for (auto const& i : itfs)
        out.push_back("Interface " + i.name + ": " + net::ipToString(i.ip) + " (broadcast " + net::ipToString(i.broadcast) + ")");
    out.push_back("Discovery: UDP broadcast on port " + std::to_string(kDiscoveryPort));
    out.push_back("Multicast lock: not used (the Geode launcher lacks CHANGE_WIFI_MULTICAST_STATE)");
    if (m_onlineHosting || m_online) {
        out.push_back("Online room code: " + (m_onlineCode.empty() ? std::string("(requesting)") : m_onlineCode));
        out.push_back("Relay: " + onlineStatus());
        if (m_relay) out.push_back("Relay messages in/out: " + std::to_string(m_relay->messagesIn()) + " / " + std::to_string(m_relay->messagesOut()));
        if (m_relayHost) out.push_back("Online player channels: " + std::to_string(m_relayHost->channelCount()));
    }
    if (m_mode == Mode::Hosting) {
        out.push_back("Room TCP port: " + std::to_string(m_listener.port()));
        out.push_back("Announcements sent: " + std::to_string(m_announcer.sent()));
        out.push_back("Authoritative sequence: " + std::to_string(m_host->seq()));
        out.push_back("Level settings check: " + m_sync.headerCheck());
    }
    if (m_mode == Mode::Joined || m_mode == Mode::Reconnecting) {
        out.push_back("Host: " + (m_conn ? m_conn->describe() : (m_online ? "online room " + m_onlineCode : m_hostEp.str())));
        out.push_back("Round trip: " + std::to_string(static_cast<int>(m_client.rttMs())) + " ms");
        out.push_back("Checksum failures: " + std::to_string(m_client.checksumFailures()));
        out.push_back("Objects matched by load order: " + std::to_string(m_sync.fallbackMatches()));
        if (m_conn) out.push_back("Bytes in/out: " + std::to_string(m_conn->bytesIn()) + " / " + std::to_string(m_conn->bytesOut()));
    }
    if (active()) {
        out.push_back("Peer ID: " + std::to_string(m_client.me()) + ", role: " + roleName(m_client.role()));
        out.push_back("Sequence: " + std::to_string(m_client.seq()) + ", room objects: " + std::to_string(m_client.store.objects.size()));
        out.push_back("Tracked editor objects: " + std::to_string(m_sync.objectCount()));
        out.push_back("Ops applied: " + std::to_string(m_opsApplied));
    }
    if (!m_lastError.empty()) out.push_back("Last error: " + m_lastError);
    return out;
}

} // namespace et
