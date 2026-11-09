// Hooks that feed the editor bridge: editor creation, object additions and removals.
#include "session/Session.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>

using namespace geode::prelude;

class $modify(ETLevelEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        et::Session::get()->onEditorInit(this);
        return true;
    }

    void removeObject(GameObject* obj, bool noUndo) {
        // Before the original: GD may release the object.
        et::Session::get()->syncMut().onObjectRemoved(this, obj);
        LevelEditorLayer::removeObject(obj, noUndo);
    }
};

class $modify(ETBaseGameLayer, GJBaseGameLayer) {
    void addToSection(GameObject* obj) {
        GJBaseGameLayer::addToSection(obj);
        // Cheap reject for PlayLayer and editors we're not attached to happens inside.
        et::Session::get()->syncMut().onObjectAdded(this, obj);
    }
};
