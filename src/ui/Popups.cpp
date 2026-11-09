#include "Popups.hpp"

#include "Common.hpp"
#include "session/Session.hpp"

#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/LoadingSpinner.hpp>
#include <Geode/ui/TextArea.hpp>

using namespace geode::prelude;

namespace et::ui {

namespace {


ScrollLayer* makeList(CCNode* parent, CCSize size, float dy) {
    auto list = ScrollLayer::create(size);
    list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(4.f));
    parent->addChildAtPosition(list, Anchor::Center, {-size.width / 2.f, -size.height / 2.f + dy});
    return list;
}

// Replaces the rows. keepScroll: stay the same distance from the top as before (periodic refreshes must not
// yank the list back while someone is reading or scrolling it).
void fillList(ScrollLayer* list, std::vector<CCNode*> const& rows, bool keepScroll = false) {
    auto content = list->m_contentLayer;
    float view = list->getContentHeight();
    float fromTop = content->getPositionY() - (view - content->getContentHeight()); // 0 = at the top
    content->removeAllChildren();
    for (auto r : rows) content->addChild(r);
    content->updateLayout();
    float top = view - content->getContentHeight();
    if (!keepScroll || top >= 0) return list->scrollToTop();
    content->setPositionY(std::clamp(top + fromTop, top, 0.f));
}

// True (and explains why) when the player can't start or join a room right now.
bool blocked(char const* what) {
    if (Session::accountName().empty()) {
        FLAlertLayer::create(what, "Log in to your <cy>Geometry Dash account</c> first (Settings > Account). Your username is your name in rooms.", "OK")->show();
        return true;
    }
    if (!Session::get()->active()) return false;
    FLAlertLayer::create(what, "You are already in a room. Leave it first from <cy>Current Session</c>.", "OK")->show();
    return true;
}

} // namespace

#define ET_CREATE(Cls)                                                  \
    Cls* Cls::create() {                                                \
        auto ret = new Cls();                                           \
        if (ret->init()) { ret->autorelease(); return ret; }            \
        delete ret;                                                     \
        return nullptr;                                                 \
    }

ET_CREATE(MainPopup)
ET_CREATE(CreateRoomPopup)
ET_CREATE(FindRoomsPopup)
ET_CREATE(ConnectByIpPopup)
ET_CREATE(SessionPopup)
ET_CREATE(DiagnosticsPopup)
ET_CREATE(JoinOnlinePopup)

// ------------------------------------------------------------------ main

bool MainPopup::init() {
    if (!Popup::init(260.f, 265.f)) return false;
    setTitle("EditTogether");
    auto name = Session::accountName();
    auto who = label(name.empty() ? "Not logged in to a GD account" : "Playing as " + name, "chatFont.fnt", 0.65f, 230.f);
    if (name.empty()) who->setColor({255, 120, 120});
    m_mainLayer->addChildAtPosition(who, Anchor::Top, {0.f, -42.f});
    auto menu = CCMenu::create();
    menu->setContentSize({200.f, 200.f});
    menu->setLayout(ColumnLayout::create()->setGap(6.f)->setAxisReverse(true));
    auto add = [&](char const* text, char const* bg, auto cb) {
        auto spr = ButtonSprite::create(text, 180, true, "goldFont.fnt", bg, 30.f, .8f);
        menu->addChild(CCMenuItemExt::createSpriteExtra(spr, cb));
    };
    add("Create Room", "GJ_button_01.png", [](auto) {
        if (!blocked("Create Room")) if (auto p = CreateRoomPopup::create()) p->show();
    });
    add("Find Rooms", "GJ_button_01.png", [](auto) {
        if (!blocked("Find Rooms")) if (auto p = FindRoomsPopup::create()) p->show();
    });
    add("Current Session", "GJ_button_02.png", [](auto) {
        if (auto p = SessionPopup::create()) p->show();
    });
    add("Settings", "GJ_button_05.png", [](auto) { openSettingsPopup(Mod::get()); });
    add("Network Diagnostics", "GJ_button_05.png", [](auto) {
        if (auto p = DiagnosticsPopup::create()) p->show();
    });
    menu->updateLayout();
    m_mainLayer->addChildAtPosition(menu, Anchor::Center, {0.f, -20.f});
    return true;
}

// ------------------------------------------------------------------ create room

bool CreateRoomPopup::init() {
    if (!Popup::init(300.f, 286.f)) return false;
    setTitle("Create Room");
    m_mainLayer->addChildAtPosition(label("Room name", "goldFont.fnt", 0.6f), Anchor::Center, {0.f, 92.f});
    m_name = TextInput::create(220.f, "Room name");
    m_name->setMaxCharCount(kMaxRoomName);
    m_name->setFilter(" abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.'!-");
    m_name->setString(Mod::get()->getSettingValue<std::string>("default-room-name"));
    m_mainLayer->addChildAtPosition(m_name, Anchor::Center, {0.f, 68.f});

    m_mainLayer->addChildAtPosition(label("Max players", "goldFont.fnt", 0.5f), Anchor::Center, {-80.f, 38.f});
    m_max = TextInput::create(80.f, "4");
    m_max->setCommonFilter(CommonFilter::Uint);
    m_max->setMaxCharCount(3);
    m_max->setString(std::to_string(Mod::get()->getSettingValue<int64_t>("max-participants")));
    m_mainLayer->addChildAtPosition(m_max, Anchor::Center, {-80.f, 14.f});

    m_mainLayer->addChildAtPosition(label("Password (optional)", "goldFont.fnt", 0.5f), Anchor::Center, {50.f, 38.f});
    m_password = TextInput::create(150.f, "No password");
    m_password->setMaxCharCount(kMaxPassword);
    m_password->setCommonFilter(CommonFilter::Any);
    m_mainLayer->addChildAtPosition(m_password, Anchor::Center, {50.f, 14.f});

    // Online: players anywhere join with a room code, through the Apinator relay (no router setup).
    auto toggleMenu = CCMenu::create();
    m_online = CCMenuItemExt::createTogglerWithStandardSprites(0.6f, [](CCMenuItemToggler* t) {
        Mod::get()->setSavedValue<bool>("allow-online", !t->isToggled());
    });
    m_online->toggle(Mod::get()->getSavedValue<bool>("allow-online", false));
    toggleMenu->addChild(m_online);
    toggleMenu->setContentSize({24.f, 24.f});
    m_mainLayer->addChildAtPosition(toggleMenu, Anchor::Center, {-115.f, -24.f});
    auto netLabel = label("Online: players anywhere join with a room code", "bigFont.fnt", 0.33f, 220.f);
    netLabel->setAnchorPoint({0.f, 0.5f});
    m_mainLayer->addChildAtPosition(netLabel, Anchor::Center, {-98.f, -24.f});
    auto netHint = label("Needs internet. You'll see the code in Current Session. Same-Wi-Fi players can always join without it.", "chatFont.fnt", 0.45f, 230.f);
    netHint->setAnchorPoint({0.f, 1.f});
    netHint->setColor({200, 200, 200});
    m_mainLayer->addChildAtPosition(netHint, Anchor::Center, {-98.f, -34.f});
    // Consent: collaborators' edits are applied to this very level.
    auto notice = label("Players in the room will edit this level.", "chatFont.fnt", 0.5f, 260.f);
    notice->setColor({255, 220, 120});
    m_mainLayer->addChildAtPosition(notice, Anchor::Bottom, {0.f, 55.f});

    auto menu = CCMenu::create();
    menu->addChild(textButton("Create", "GJ_button_01.png", 0.8f, [this](auto) {
        auto parsed = numFromString<int>(std::string(m_max->getString().c_str()));
        int maxPlayers = parsed.isOk() ? parsed.unwrap() : 0;
        if (maxPlayers < 2 || maxPlayers > static_cast<int>(kMaxPeers)) {
            FLAlertLayer::create("Create Room", "Max players must be between <cy>2</c> and <cy>100</c>.", "OK")->show();
            return;
        }
        Mod::get()->setSettingValue<int64_t>("max-participants", maxPlayers);
        std::string err;
        Session::RoomOptions opts;
        opts.name = std::string(m_name->getString().c_str());
        opts.maxPlayers = maxPlayers;
        opts.password = std::string(m_password->getString().c_str());
        opts.online = m_online->isToggled();
        if (!Session::get()->createRoom(opts, err)) {
            FLAlertLayer::create("Create Room", err, "OK")->show();
            return;
        }
        this->onClose(nullptr);
        if (auto p = SessionPopup::create()) p->show();
    }));
    m_mainLayer->addChildAtPosition(menu, Anchor::Bottom, {0.f, 30.f});
    return true;
}

// ------------------------------------------------------------------ find rooms

bool FindRoomsPopup::init() {
    if (!Popup::init(390.f, 270.f)) return false;
    setTitle("Find Rooms");
    m_list = makeList(m_mainLayer, {350.f, 150.f}, 8.f);

    m_state = label("", "chatFont.fnt", 0.7f);
    m_mainLayer->addChildAtPosition(m_state, Anchor::Center, {0.f, 8.f});
    m_spinner = LoadingSpinner::create(30.f);
    m_mainLayer->addChildAtPosition(m_spinner, Anchor::Center, {0.f, 35.f});

    auto menu = CCMenu::create();
    menu->setContentSize({340.f, 40.f});
    menu->setLayout(RowLayout::create()->setGap(10.f));
    menu->addChild(textButton("Refresh", "GJ_button_04.png", 0.7f, [this](auto) {
        m_browser.clear();
        m_searchStarted = nowSeconds();
        m_dirty = true;
    }));
    menu->addChild(textButton("Connect by IP", "GJ_button_05.png", 0.7f, [](auto) {
        if (auto p = ConnectByIpPopup::create()) p->show();
    }));
    menu->addChild(textButton("Join Online", "GJ_button_02.png", 0.7f, [](auto) {
        if (auto p = JoinOnlinePopup::create()) p->show();
    }));
    menu->updateLayout();
    m_mainLayer->addChildAtPosition(menu, Anchor::Bottom, {0.f, 28.f});

    if (!m_browser.start(m_error))
        m_error = "Room discovery is unavailable (" + m_error + ").\nAsk the host for their address and use Connect by IP.";
    m_searchStarted = nowSeconds();
    schedule(schedule_selector(FindRoomsPopup::onTick), 0.2f);
    onTick(0);
    return true;
}

void FindRoomsPopup::onTick(float) {
    double t = nowSeconds();
    m_browser.update(t);
    bool autoRefresh = Mod::get()->getSettingValue<bool>("auto-discovery");
    if (m_dirty || (autoRefresh && t - m_lastRebuild >= 1.0)) rebuild(t);
}

void FindRoomsPopup::rebuild(double t) {
    bool searching = t - m_searchStarted < 3.0;
    if (m_dirty && searching) {
        // Manual refresh: keep the spinner up for a moment instead of flashing "no rooms".
        m_spinner->setVisible(true);
        m_state->setString("Searching for rooms...");
        fillList(m_list, {});
        if (t - m_searchStarted < 1.5) return;
    }
    m_dirty = false;
    m_lastRebuild = t;

    std::vector<CCNode*> rows;
    for (auto const& [id, room] : m_browser.rooms()) {
        auto buttons = CCMenu::create();
        bool full = !room.info.open || room.info.players >= room.info.maxPlayers;
        bool otherVersion = room.info.protocol != kProtocolVersion;
        auto copy = room;
        char const* text = otherVersion ? "Version" : full ? "Full" : "Join";
        buttons->addChild(textButton(text, (full || otherVersion) ? "GJ_button_04.png" : "GJ_button_01.png", 0.6f,
            [this, copy, full, otherVersion](auto) {
                if (otherVersion)
                    return FLAlertLayer::create("Different Version",
                        fmt::format("This room uses a different EditTogether version (room protocol {}, yours {}). "
                                    "Everyone needs the same version of the mod.", copy.info.protocol, kProtocolVersion), "OK")->show();
                if (full) return FLAlertLayer::create("Room Full", "This room has no free slots.", "OK")->show();
                joinRoom(copy);
            }));
        buttons->updateLayout();
        std::string details = fmt::format("Admin: {}   Players: {}/{}   {}{}", room.info.hostName, room.info.players,
            room.info.maxPlayers, otherVersion ? "Different version" : full ? "Full" : "Open", room.info.locked ? "   Password" : "");
        std::string level = room.info.levelName.empty() ? "Level: (unnamed)" : "Level: " + room.info.levelName;
        auto r = row(350.f, 50.f, {{room.info.roomName, "bigFont.fnt"}, {details, "chatFont.fnt"}, {level, "chatFont.fnt"}}, buttons);
        rows.push_back(r);
    }
    fillList(m_list, rows, true);

    m_spinner->setVisible(rows.empty() && m_error.empty() && searching);
    if (!m_error.empty()) m_state->setString(m_error.c_str());
    else if (!rows.empty()) m_state->setString("");
    else if (searching) m_state->setString("Searching for rooms...");
    else m_state->setString("No rooms found on this Wi-Fi network.\nSome routers block discovery: try Connect by IP.");
}

void FindRoomsPopup::joinRoom(net::FoundRoom const& room) {
    Ref<FindRoomsPopup> self = this;
    auto join = [self, room](std::string password) {
        confirmJoin([room, password](bool save) { Session::get()->join(room.host, room.info.roomId, room.info.roomName, password, save); },
            [self] { self->onClose(nullptr); });
    };
    if (!room.info.locked) return join("");
    if (auto p = PasswordPopup::create(join)) p->show();
}

// ------------------------------------------------------------------ password prompt

PasswordPopup* PasswordPopup::create(std::function<void(std::string)> done) {
    auto ret = new PasswordPopup();
    if (ret->init(std::move(done))) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool PasswordPopup::init(std::function<void(std::string)> done) {
    if (!Popup::init(260.f, 150.f)) return false;
    setTitle("Room Password");
    m_done = std::move(done);
    m_input = TextInput::create(190.f, "Password");
    m_input->setMaxCharCount(kMaxPassword);
    m_input->setCommonFilter(CommonFilter::Any);
    m_mainLayer->addChildAtPosition(m_input, Anchor::Center, {0.f, 6.f});
    auto menu = CCMenu::create();
    menu->addChild(textButton("Join", "GJ_button_01.png", 0.8f, [this](auto) {
        auto done = m_done;
        std::string pw(m_input->getString().c_str());
        this->onClose(nullptr);
        done(pw);
    }));
    m_mainLayer->addChildAtPosition(menu, Anchor::Bottom, {0.f, 26.f});
    return true;
}

// ------------------------------------------------------------------ join confirmation

JoinConfirmPopup* JoinConfirmPopup::create(std::function<void(bool)> join, std::function<void()> onJoined) {
    auto ret = new JoinConfirmPopup();
    if (ret->init(std::move(join), std::move(onJoined))) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool JoinConfirmPopup::init(std::function<void(bool)> join, std::function<void()> onJoined) {
    if (!Popup::init(340.f, 190.f)) return false;
    setTitle("Join Room");
    m_join = std::move(join);
    m_onJoined = std::move(onJoined);
    auto text = SimpleTextArea::create(
        "When the administrator accepts, a shared copy of the room's level opens in the editor.\n"
        "Save the level you have open first?",
        "chatFont.fnt", 0.7f, 300.f);
    text->setAlignment(kCCTextAlignmentCenter);
    m_mainLayer->addChildAtPosition(text, Anchor::Center, {0.f, 18.f});
    auto menu = CCMenu::create();
    menu->setContentSize({320.f, 40.f});
    menu->setLayout(RowLayout::create()->setGap(8.f));
    menu->addChild(textButton("Cancel", "GJ_button_04.png", 0.65f, [this](auto) { this->onClose(nullptr); }));
    menu->addChild(textButton("Don't Save", "GJ_button_06.png", 0.65f, [this](auto) { finish(false); }));
    menu->addChild(textButton("Save & Join", "GJ_button_01.png", 0.65f, [this](auto) { finish(true); }));
    menu->updateLayout();
    m_mainLayer->addChildAtPosition(menu, Anchor::Bottom, {0.f, 28.f});
    return true;
}

void JoinConfirmPopup::finish(bool save) {
    Ref<JoinConfirmPopup> self = this; // callbacks may close other popups
    auto join = m_join;
    auto onJoined = m_onJoined;
    this->onClose(nullptr);
    join(save);
    onJoined();
    if (auto p = SessionPopup::create()) p->show();
}

void confirmJoin(std::function<void(bool)> join, std::function<void()> onJoined) {
    if (auto p = JoinConfirmPopup::create(std::move(join), std::move(onJoined))) p->show();
}

// ------------------------------------------------------------------ connect by IP

bool ConnectByIpPopup::init() {
    if (!Popup::init(280.f, 200.f)) return false;
    setTitle("Connect by IP");
    m_mainLayer->addChildAtPosition(label("Same-Wi-Fi address from the host's Current Session", "chatFont.fnt", 0.6f, 250.f),
        Anchor::Center, {0.f, 46.f});
    m_ip = TextInput::create(200.f, "192.168.1.20");
    m_ip->setFilter("0123456789.:");
    m_ip->setMaxCharCount(21);
    m_ip->setString(Mod::get()->getSavedValue<std::string>("last-ip", ""));
    m_mainLayer->addChildAtPosition(m_ip, Anchor::Center, {0.f, 22.f});
    m_password = TextInput::create(200.f, "Password (if the room has one)");
    m_password->setMaxCharCount(kMaxPassword);
    m_password->setCommonFilter(CommonFilter::Any);
    m_mainLayer->addChildAtPosition(m_password, Anchor::Center, {0.f, -14.f});
    auto menu = CCMenu::create();
    menu->addChild(textButton("Connect", "GJ_button_01.png", 0.8f, [this](auto) {
        std::string text(m_ip->getString().c_str());
        auto colon = text.find(':');
        auto ip = net::parseIPv4(text.substr(0, colon));
        int port = kDefaultTcpPort;
        if (colon != std::string::npos) {
            auto p = numFromString<int>(text.substr(colon + 1));
            port = p.isOk() ? p.unwrap() : 0;
        }
        if (!ip || port <= 0 || port > 65535) {
            FLAlertLayer::create("Connect by IP", "Enter an address like <cy>192.168.1.20</c> or <cy>192.168.1.20:47812</c>.", "OK")->show();
            return;
        }
        Mod::get()->setSavedValue<std::string>("last-ip", text);
        Ref<ConnectByIpPopup> self = this;
        net::Endpoint ep{*ip, static_cast<uint16_t>(port)};
        std::string pw(m_password->getString().c_str());
        confirmJoin([ep, pw](bool save) { Session::get()->join(ep, 0, "Room", pw, save); }, [self] { self->onClose(nullptr); });
    }));
    m_mainLayer->addChildAtPosition(menu, Anchor::Bottom, {0.f, 28.f});
    return true;
}

// ------------------------------------------------------------------ join online

bool JoinOnlinePopup::init() {
    if (!Popup::init(290.f, 210.f)) return false;
    setTitle("Join Online");
    m_mainLayer->addChildAtPosition(label("Room code (shown in the host's Current Session)", "chatFont.fnt", 0.55f, 260.f),
        Anchor::Center, {0.f, 52.f});
    m_code = TextInput::create(180.f, "ABCD2345");
    m_code->setFilter("ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjklmnpqrstuvwxyz23456789 -");
    m_code->setMaxCharCount(12);
    m_code->setString(Mod::get()->getSavedValue<std::string>("last-online-code", ""));
    m_mainLayer->addChildAtPosition(m_code, Anchor::Center, {0.f, 28.f});
    m_password = TextInput::create(180.f, "Password (if any)");
    m_password->setMaxCharCount(kMaxPassword);
    m_password->setCommonFilter(CommonFilter::Any);
    m_mainLayer->addChildAtPosition(m_password, Anchor::Center, {0.f, -8.f});
    auto menu = CCMenu::create();
    menu->addChild(textButton("Join", "GJ_button_01.png", 0.8f, [this](auto) {
        std::string code = net::normalizeCode(std::string(m_code->getString().c_str()));
        if (!net::validCode(code)) {
            FLAlertLayer::create("Join Online", "Enter the 8-character room code the host sees in <cy>Current Session</c>.", "OK")->show();
            return;
        }
        Mod::get()->setSavedValue<std::string>("last-online-code", code);
        std::string pw(m_password->getString().c_str());
        Ref<JoinOnlinePopup> self = this;
        confirmJoin([code, pw](bool save) { Session::get()->joinOnline(code, pw, save); }, [self] { self->onClose(nullptr); });
    }));
    m_mainLayer->addChildAtPosition(menu, Anchor::Bottom, {0.f, 26.f});
    return true;
}

// ------------------------------------------------------------------ session

bool SessionPopup::init() {
    if (!Popup::init(400.f, 300.f)) return false;
    setTitle("Current Session");
    m_info = CCNode::create();
    m_info->setContentSize({360.f, 96.f});
    m_info->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(m_info, Anchor::Top, {0.f, -88.f});
    m_list = makeList(m_mainLayer, {360.f, 110.f}, -40.f);
    m_bottom = CCMenu::create();
    m_bottom->setContentSize({370.f, 40.f});
    m_bottom->setLayout(RowLayout::create()->setGap(8.f));
    m_mainLayer->addChildAtPosition(m_bottom, Anchor::Bottom, {0.f, 24.f});
    schedule(schedule_selector(SessionPopup::onTick), 0.25f);
    rebuild();
    return true;
}

void SessionPopup::onTick(float) {
    if (Session::get()->version() != m_shownVersion) rebuild();
}

void SessionPopup::rebuild() {
    auto s = Session::get();
    m_shownVersion = s->version();
    auto host = s->host();
    bool hosting = s->mode() == Session::Mode::Hosting;

    // Info block
    m_info->removeAllChildren();
    std::vector<std::string> lines;
    if (!s->active()) {
        lines = {"You are not in a room.", "Use Create Room or Find Rooms to start."};
    } else {
        int players = hosting ? host->activeCount() : static_cast<int>(s->client().peers().size());
        lines.push_back(fmt::format("Room: {}    Administrator: {}", s->roomName(), s->adminName()));
        lines.push_back(fmt::format("Status: {}", s->status()));
        std::string third = fmt::format("Participants: {}", players);
        if (hosting) third += fmt::format("/{}", host->config().maxPlayers);
        if (s->client().state() == ClientCore::State::Active) third += fmt::format("    Your role: {}", roleName(s->client().role()));
        lines.push_back(third);
        if (hosting) {
            std::string addr;
            for (auto const& i : net::localInterfaces()) addr += net::ipToString(i.ip) + ":" + std::to_string(s->hostPort()) + "  ";
            lines.push_back("Same Wi-Fi address: " + (addr.empty() ? std::string("unknown") : addr));
            if (s->onlineHosting()) lines.push_back(fmt::format("Online room code: {}", s->onlineCode().empty() ? std::string("requesting...") : s->onlineCode()));
            if (s->hasPassword()) lines.push_back("Password protected");
            if (s->onlineHosting()) lines.push_back("Online: " + s->onlineStatus());
        } else if (s->mode() != Session::Mode::None && !s->onlineStatus().empty()) {
            lines.push_back("Online: " + s->onlineStatus());
        }
    }
    float y = 96.f;
    for (auto const& l : lines) {
        auto lab = label(l, "chatFont.fnt", 0.65f, 360.f);
        lab->setPosition({180.f, y});
        m_info->addChild(lab);
        y -= 13.f;
    }

    // Participants and pending requests
    std::vector<CCNode*> rows;
    if (hosting) {
        for (auto const& [id, p] : host->peers()) {
            if (p.active || !p.helloReceived) continue;
            auto b = CCMenu::create();
            uint32_t pid = id;
            b->addChild(textButton("Accept", "GJ_button_01.png", 0.55f, [pid](auto) {
                if (auto h = Session::get()->host()) h->accept(pid, nowSeconds());
            }));
            b->addChild(textButton("Reject", "GJ_button_06.png", 0.55f, [pid](auto) {
                if (auto h = Session::get()->host()) h->reject(pid, "The administrator rejected your request");
            }));
            b->updateLayout();
            rows.push_back(row(360.f, 36.f, {{p.nickname, "bigFont.fnt"}, {"Wants to join", "chatFont.fnt"}}, b));
        }
    }
    if (s->active()) {
        for (auto const& p : s->client().peers()) {
            CCMenu* b = nullptr;
            uint32_t pid = p.id;
            if (hosting && p.role != Role::Admin) {
                b = CCMenu::create();
                bool viewer = p.role == Role::Viewer;
                b->addChild(textButton(viewer ? "Make Editor" : "Make Viewer", "GJ_button_04.png", 0.45f, [pid, viewer](auto) {
                    if (auto h = Session::get()->host()) h->setRole(pid, viewer ? Role::Editor : Role::Viewer);
                }));
                b->addChild(textButton("Unlock", "GJ_button_05.png", 0.5f, [pid](auto) {
                    if (auto h = Session::get()->host()) h->releaseLocks(pid);
                }));
                std::string nick = p.nickname;
                b->addChild(textButton("Kick", "GJ_button_06.png", 0.5f, [pid, nick](auto) {
                    createQuickPopup("Remove Player", "Remove <cy>" + nick + "</c> from the room?", "Cancel", "Remove",
                        [pid](FLAlertLayer*, bool yes) {
                            if (yes) if (auto h = Session::get()->host()) h->kick(pid, "Removed by the administrator");
                        });
                }));
                b->updateLayout();
            }
            std::string who = p.nickname + (p.id == s->client().me() ? " (you)" : "");
            size_t locks = 0;
            if (auto it = s->client().locks().find(p.id); it != s->client().locks().end()) locks = it->second.size();
            std::string sub = roleName(p.role);
            if (locks) sub += fmt::format("  -  editing {} object{}", locks, locks == 1 ? "" : "s");
            rows.push_back(row(360.f, 36.f, {{who, "bigFont.fnt"}, {sub, "chatFont.fnt"}}, b));
        }
    }
    fillList(m_list, rows, m_listBuilt);
    m_listBuilt = true;

    // Bottom buttons
    m_bottom->removeAllChildren();
    if (s->active()) {
        m_bottom->addChild(textButton(hosting ? "Close Room" : "Leave Room", "GJ_button_06.png", 0.7f, [hosting](auto) {
            createQuickPopup(hosting ? "Close Room" : "Leave Room",
                hosting ? "Close the room for everyone?" : "Leave this room? You keep your local copy of the level.",
                "Cancel", hosting ? "Close" : "Leave",
                [](FLAlertLayer*, bool yes) {
                    if (yes) Session::get()->leave();
                });
        }));
        if (s->mode() == Session::Mode::Joined)
            m_bottom->addChild(textButton("Resync", "GJ_button_04.png", 0.7f, [](auto) { Session::get()->requestResync(); }));
        if (s->onlineHosting() && !s->onlineCode().empty())
            m_bottom->addChild(textButton("Copy Code", "GJ_button_02.png", 0.6f, [](auto) {
                if (geode::utils::clipboard::write(Session::get()->onlineCode()))
                    Notification::create("Room code copied", NotificationIcon::Success)->show();
            }));
    }
    m_bottom->addChild(textButton("Diagnostics", "GJ_button_05.png", 0.7f, [](auto) {
        if (auto p = DiagnosticsPopup::create()) p->show();
    }));
    m_bottom->updateLayout();
}

// ------------------------------------------------------------------ diagnostics

bool DiagnosticsPopup::init() {
    if (!Popup::init(400.f, 260.f)) return false;
    setTitle("Network Diagnostics");
    m_list = makeList(m_mainLayer, {370.f, 200.f}, -12.f);
    schedule(schedule_selector(DiagnosticsPopup::onTick), 1.0f);
    onTick(0);
    return true;
}

void DiagnosticsPopup::onTick(float) {
    std::vector<CCNode*> rows;
    for (auto const& line : Session::get()->diagnostics()) {
        auto l = label(line, "chatFont.fnt", 0.6f, 360.f);
        auto holder = CCNode::create();
        holder->setContentSize({370.f, 14.f});
        l->setAnchorPoint({0.f, 0.5f});
        l->setPosition({4.f, 7.f});
        holder->addChild(l);
        rows.push_back(holder);
    }
    fillList(m_list, rows, m_listBuilt);
    m_listBuilt = true;
}

} // namespace et::ui
