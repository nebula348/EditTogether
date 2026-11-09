#pragma once

#include "net/Discovery.hpp"

#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>

#include <functional>

namespace et::ui {

class MainPopup : public geode::Popup {
protected:
    bool init();
public:
    static MainPopup* create();
};

class CreateRoomPopup : public geode::Popup {
protected:
    geode::TextInput* m_name = nullptr;
    geode::TextInput* m_max = nullptr;
    geode::TextInput* m_password = nullptr;
    CCMenuItemToggler* m_online = nullptr;
    bool init();
public:
    static CreateRoomPopup* create();
};

class FindRoomsPopup : public geode::Popup {
protected:
    net::RoomBrowser m_browser;
    std::string m_error;
    geode::ScrollLayer* m_list = nullptr;
    cocos2d::CCLabelBMFont* m_state = nullptr;
    cocos2d::CCNode* m_spinner = nullptr;
    double m_searchStarted = 0;
    double m_lastRebuild = 0;
    bool m_dirty = true;
    bool init();
    void onTick(float);
    void rebuild(double now);
    void joinRoom(net::FoundRoom const& room);
public:
    static FindRoomsPopup* create();
};

class PasswordPopup : public geode::Popup {
protected:
    geode::TextInput* m_input = nullptr;
    std::function<void(std::string)> m_done;
    bool init(std::function<void(std::string)> done);
public:
    static PasswordPopup* create(std::function<void(std::string)> done);
};

class JoinOnlinePopup : public geode::Popup {
protected:
    geode::TextInput* m_code = nullptr;
    geode::TextInput* m_password = nullptr;
    bool init();
public:
    static JoinOnlinePopup* create();
};

class ConnectByIpPopup : public geode::Popup {
protected:
    geode::TextInput* m_ip = nullptr;
    geode::TextInput* m_password = nullptr;
    bool init();
public:
    static ConnectByIpPopup* create();
};

class SessionPopup : public geode::Popup {
protected:
    geode::ScrollLayer* m_list = nullptr;
    cocos2d::CCNode* m_info = nullptr;
    cocos2d::CCMenu* m_bottom = nullptr;
    uint64_t m_shownVersion = ~0ull;
    bool m_listBuilt = false;
    bool init();
    void onTick(float);
    void rebuild();
public:
    static SessionPopup* create();
};

class DiagnosticsPopup : public geode::Popup {
protected:
    geode::ScrollLayer* m_list = nullptr;
    bool m_listBuilt = false;
    bool init();
    void onTick(float);
public:
    static DiagnosticsPopup* create();
};

// Confirms (saving the current level) and starts joining. Shared by Find Rooms and Connect by IP.
// Asks whether to save the level that's open, then runs `join(save)` and `onJoined` and shows the session.
class JoinConfirmPopup : public geode::Popup {
protected:
    std::function<void(bool)> m_join;
    std::function<void()> m_onJoined;
    bool init(std::function<void(bool)> join, std::function<void()> onJoined);
    void finish(bool save);
public:
    static JoinConfirmPopup* create(std::function<void(bool)> join, std::function<void()> onJoined);
};
void confirmJoin(std::function<void(bool)> join, std::function<void()> onJoined);

} // namespace et::ui
