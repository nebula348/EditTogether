#pragma once

#include "core/ClientCore.hpp"

#include <Geode/Geode.hpp>

#include <unordered_map>
#include <unordered_set>

namespace et {

// Bridges the GD editor and the collaborative state.
//  - Maps stable collab IDs <-> GameObject* (never array indices).
//  - Captures local edits: creation/removal hooks, polling of the selection (the objects
//    being edited), and an incremental background scan that catches anything else (undo, etc.).
//  - Applies authoritative ops coming from the room.
class EditorSync {
public:
    ~EditorSync() { detach(); }

    void attach(LevelEditorLayer* lel);
    void detach();
    bool attached() const { return m_lel != nullptr; }
    LevelEditorLayer* layer() const { return m_lel; }
    // Attached, editor still on screen, not playtesting.
    bool ready() const;
    bool alive() const;

    // Host: give every current object an ID. Returns the seed state for the room.
    ObjectStore adoptAll(uint32_t me);
    // Client: after the editor was rebuilt from `store`, find which object is which.
    bool bindTo(ObjectStore const& store, uint32_t me, std::string& error);

    void tick(ClientCore& core, double now);
    // `own` = echo of our own accepted ops; `force` = host correction that must win over local state.
    void applyOps(std::vector<Op> const& ops, bool own, bool force);
    void updateLockLabels(ClientCore const& core);
    // Shows who is playing the level and where, with the game's own play button.
    void updatePlayMarkers(ClientCore const& core, double now);
    bool headerMatches(std::string const& header) const { return normalizeHeader(header) == currentHeader(); }

    // Hook entry points.
    void onObjectAdded(GJBaseGameLayer* layer, GameObject* obj);
    void onObjectRemoved(LevelEditorLayer* layer, GameObject* obj);

    // A remote level-settings change arrived; the editor must be rebuilt to show it.
    bool takeReloadRequest() { return std::exchange(m_needsReload, false); }
    std::string takeNotice() { return std::exchange(m_notice, {}); }

    size_t objectCount() const { return m_byId.size(); }
    uint32_t fallbackMatches() const { return m_fallbackMatches; }
    std::string const& headerCheck() const { return m_headerCheck; }

private:
    struct Entry {
        GameObject* obj = nullptr; // retained
        std::string localStr;      // last serialization we know of
        uint32_t seenPass = 0;
    };

    std::string serialize(GameObject* obj) const;
    std::string currentHeader() const;
    void registerObj(GameObject* obj, uint64_t id, std::string str);
    void unregister(uint64_t id);
    std::optional<uint64_t> idOf(GameObject* obj) const;
    void removeFromEditor(GameObject* obj);
    GameObject* createFromString(std::string const& data);
    void replaceObject(uint64_t id, std::string const& data);
    void revert(ClientCore const& core, uint64_t id, bool stillInEditor);
    std::vector<GameObject*> selection() const;
    bool lockedByOther(ClientCore const& core, uint64_t id) const;
    void scanChunk(ClientCore& core, std::vector<Op>& ops, std::unordered_set<GameObject*> const& selected);
    void finishPass(ClientCore& core, std::vector<Op>& ops);
    void notice(std::string text);
    void clearAdded();

    LevelEditorLayer* m_lel = nullptr; // retained while attached
    std::unordered_map<uint64_t, Entry> m_byId;
    std::unordered_map<GameObject*, uint64_t> m_byObj;
    std::unordered_set<GameObject*> m_added; // retained; O(1) membership for big pastes
    geode::Ref<cocos2d::CCArray> m_removed = cocos2d::CCArray::create();
    std::unordered_set<uint64_t> m_lastSelected;
    std::vector<uint64_t> m_lastLockSet;
    uint32_t m_me = 0;
    uint32_t m_counter = 0;
    uint32_t m_pass = 1;
    unsigned m_scanCursor = 0;
    std::string m_lastHeader;
    std::string m_headerCheck;
    double m_lastSelCheck = 0;
    double m_lastHeaderCheck = 0;
    double m_holdAddsUntil = 0;
    bool m_applying = false;
    bool m_needsReload = false;
    uint32_t m_fallbackMatches = 0;
    std::string m_notice;
    std::unordered_map<uint32_t, geode::Ref<cocos2d::CCLabelBMFont>> m_labels;
    std::unordered_map<uint32_t, geode::Ref<cocos2d::CCNode>> m_playMarkers;
    std::unordered_map<uint32_t, uint64_t> m_playLooks;
    geode::Ref<cocos2d::CCNode> m_playBanner;
};

} // namespace et
