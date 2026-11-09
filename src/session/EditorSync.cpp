#include "EditorSync.hpp"

#include <algorithm>

using namespace geode::prelude;

namespace et {

namespace {
constexpr unsigned kScanPerTick = 150;  // background verification budget per frame
constexpr double kHeaderInterval = 1.0;
} // namespace

// ------------------------------------------------------------------ lifecycle

void EditorSync::attach(LevelEditorLayer* lel) {
    detach();
    m_lel = lel;
    m_lel->retain();
    m_scanCursor = 0;
    m_pass = 1;
    m_lastHeader = currentHeader();
}

void EditorSync::detach() {
    for (auto& [owner, label] : m_labels)
        if (label) label->removeFromParent();
    m_labels.clear();
    for (auto& [peer, marker] : m_playMarkers)
        if (marker) marker->removeFromParent();
    m_playMarkers.clear();
    m_playLooks.clear();
    if (m_playBanner) m_playBanner->removeFromParent();
    m_playBanner = nullptr;
    for (auto& [id, e] : m_byId) e.obj->release();
    m_byId.clear();
    m_byObj.clear();
    clearAdded();
    m_removed->removeAllObjects();
    m_lastSelected.clear();
    m_lastLockSet.clear();
    if (m_lel) m_lel->release();
    m_lel = nullptr;
}

bool EditorSync::alive() const {
    return m_lel && m_lel->isRunning() && m_lel->getParent();
}

bool EditorSync::ready() const {
    return alive() && m_lel->m_playbackMode == PlaybackMode::Not && m_lel->m_editorUI;
}

std::string EditorSync::serialize(GameObject* obj) const {
    gd::string s = obj->getSaveString(m_lel);
    return std::string(s.c_str(), s.size());
}

std::string EditorSync::currentHeader() const {
    if (!m_lel || !m_lel->m_levelSettings) return {};
    gd::string s = m_lel->m_levelSettings->getSaveString();
    return normalizeHeader(std::string_view(s.c_str(), s.size()));
}

void EditorSync::registerObj(GameObject* obj, uint64_t id, std::string str) {
    obj->retain();
    m_byId[id] = {obj, std::move(str), m_pass};
    m_byObj[obj] = id;
}

void EditorSync::unregister(uint64_t id) {
    auto it = m_byId.find(id);
    if (it == m_byId.end()) return;
    m_byObj.erase(it->second.obj);
    it->second.obj->release();
    m_byId.erase(it);
}

std::optional<uint64_t> EditorSync::idOf(GameObject* obj) const {
    auto it = m_byObj.find(obj);
    if (it == m_byObj.end()) return std::nullopt;
    return it->second;
}

void EditorSync::notice(std::string text) {
    m_notice = std::move(text);
}

void EditorSync::clearAdded() {
    for (auto obj : m_added) obj->release();
    m_added.clear();
}

// ------------------------------------------------------------------ adoption / binding

ObjectStore EditorSync::adoptAll(uint32_t me) {
    ObjectStore store;
    m_me = me;
    m_counter = 0;
    store.header = m_lastHeader = currentHeader();
    for (auto obj : CCArrayExt<GameObject*, false>(m_lel->m_objects)) {
        uint64_t id = makeId(me, ++m_counter);
        auto str = serialize(obj);
        store.set(id, str);
        registerObj(obj, id, std::move(str));
    }
    // Sanity check of the assumption that the level string header == LevelSettingsObject::getSaveString().
    gd::string full = m_lel->getLevelString();
    std::string_view fv(full.c_str(), full.size());
    gd::string settings = m_lel->m_levelSettings->getSaveString();
    m_headerCheck = fv.substr(0, fv.find(';')) == std::string_view(settings.c_str(), settings.size())
        ? "OK" : "MISMATCH (level settings sync may be unreliable)";
    log::info("EditTogether: adopted {} objects, header check {}", m_byId.size(), m_headerCheck);
    return store;
}

bool EditorSync::bindTo(ObjectStore const& store, uint32_t me, std::string& error) {
    m_me = me;
    m_counter = 0;
    m_fallbackMatches = 0;
    std::unordered_map<std::string_view, std::vector<uint64_t>> byStr;
    for (auto const& [id, s] : store.objects) {
        byStr[s].push_back(id);
        if (idOwner(id) == me) m_counter = std::max(m_counter, static_cast<uint32_t>(id)); // keep our IDs unique after a rebuild
    }
    for (auto& [s, ids] : byStr) std::sort(ids.rbegin(), ids.rend()); // pop_back yields the smallest

    std::vector<GameObject*> leftovers;
    for (auto obj : CCArrayExt<GameObject*, false>(m_lel->m_objects)) {
        auto str = serialize(obj);
        auto it = byStr.find(str);
        if (it != byStr.end() && !it->second.empty()) {
            uint64_t id = it->second.back();
            it->second.pop_back();
            registerObj(obj, id, std::move(str));
        } else {
            leftovers.push_back(obj);
        }
    }
    std::vector<uint64_t> remaining;
    for (auto& [s, ids] : byStr) remaining.insert(remaining.end(), ids.begin(), ids.end());
    if (remaining.size() != leftovers.size()) {
        error = fmt::format("{} objects in the editor, {} in the room state", m_byId.size() + leftovers.size(), store.objects.size());
        return false;
    }
    // Objects GD re-serializes differently: the level string was written sorted by ID and GD loads in order.
    std::sort(remaining.begin(), remaining.end());
    for (size_t i = 0; i < leftovers.size(); ++i) registerObj(leftovers[i], remaining[i], serialize(leftovers[i]));
    m_fallbackMatches = static_cast<uint32_t>(leftovers.size());
    m_lastHeader = currentHeader();
    if (m_fallbackMatches) log::warn("EditTogether: {} objects matched by load order", m_fallbackMatches);
    return true;
}

// ------------------------------------------------------------------ editor mutations

std::vector<GameObject*> EditorSync::selection() const {
    std::vector<GameObject*> out;
    auto ui = m_lel->m_editorUI;
    if (!ui) return out;
    if (ui->m_selectedObjects)
        for (auto obj : CCArrayExt<GameObject*, false>(ui->m_selectedObjects)) out.push_back(obj);
    if (ui->m_selectedObject && std::find(out.begin(), out.end(), ui->m_selectedObject) == out.end())
        out.push_back(ui->m_selectedObject);
    return out;
}

void EditorSync::removeFromEditor(GameObject* obj) {
    if (auto ui = m_lel->m_editorUI) {
        if (ui->m_selectedObject == obj || (ui->m_selectedObjects && ui->m_selectedObjects->containsObject(obj)))
            ui->deselectObject(obj);
    }
    m_lel->removeObject(obj, true);
}

GameObject* EditorSync::createFromString(std::string const& data) {
    auto arr = m_lel->createObjectsFromString(gd::string(data + ";"), true, true);
    if (!arr || arr->count() == 0) return nullptr;
    // A valid object string gives exactly one object.
    for (unsigned i = 1; i < arr->count(); ++i) m_lel->removeObject(static_cast<GameObject*>(arr->objectAtIndex(i)), true);
    return static_cast<GameObject*>(arr->objectAtIndex(0));
}

void EditorSync::replaceObject(uint64_t id, std::string const& data) {
    if (auto it = m_byId.find(id); it != m_byId.end()) {
        removeFromEditor(it->second.obj);
        unregister(id);
    }
    if (data.empty()) return;
    if (auto obj = createFromString(data)) registerObj(obj, id, serialize(obj));
    else log::error("EditTogether: GD could not create object {} from '{}'", id, data.substr(0, 80));
}

void EditorSync::revert(ClientCore const& core, uint64_t id, bool stillInEditor) {
    if (!stillInEditor) unregister(id); // GD already removed it; don't remove twice
    auto auth = core.store.get(id);
    m_applying = true;
    replaceObject(id, auth ? *auth : std::string());
    m_applying = false;
}

bool EditorSync::lockedByOther(ClientCore const& core, uint64_t id) const {
    uint32_t owner = core.lockOwner(id);
    return owner != 0 && owner != core.me();
}

// ------------------------------------------------------------------ hooks

void EditorSync::onObjectAdded(GJBaseGameLayer* layer, GameObject* obj) {
    // addToSection also fires when objects move between sections; known objects are ignored here.
    if (m_applying || layer != m_lel || !obj || m_byObj.contains(obj)) return;
    // GD adds temporary helper objects while playtesting; real edits can't happen then.
    if (m_lel->m_playbackMode != PlaybackMode::Not) return;
    if (m_added.insert(obj).second) obj->retain();
}

void EditorSync::onObjectRemoved(LevelEditorLayer* layer, GameObject* obj) {
    if (m_applying || layer != m_lel || !obj || m_lel->m_playbackMode != PlaybackMode::Not) return;
    if (m_byObj.contains(obj)) m_removed->addObject(obj);
    else if (m_added.erase(obj)) obj->release(); // added and removed before we ever announced it
}

// ------------------------------------------------------------------ capture

void EditorSync::tick(ClientCore& core, double now) {
    if (alive() && m_lel->m_playbackMode != PlaybackMode::Not) m_holdAddsUntil = now + 1.0;
    if (!ready() || core.state() != ClientCore::State::Active || core.awaitingSnapshot()) return;
    if (core.me() != m_me) { // new peer ID after reconnecting: new ID namespace
        m_me = core.me();
        m_counter = 0;
    }
    bool editor = canEdit(core.role());
    std::vector<Op> ops;
    auto ui = m_lel->m_editorUI;

    // 1. Objects that appeared (placed, pasted, duplicated, undo of a delete, ...).
    // Right after a playtest GD adds and then removes internal helper objects; let that settle first.
    bool settled = now >= m_holdAddsUntil;
    std::unordered_set<GameObject*> added;
    if (settled) added.swap(m_added);
    // Keep them alive until this pass is over, whatever happens to them in GD meanwhile.
    std::vector<Ref<GameObject>> keep(added.begin(), added.end());
    for (auto obj : added) obj->release(); // drop the queue's own reference; `keep` holds one
    for (auto obj : added) {
        if (m_byObj.contains(obj)) continue;
        if (!editor) {
            m_applying = true;
            m_lel->removeObject(obj, true);
            m_applying = false;
            notice("Viewers cannot edit the level");
            continue;
        }
        uint64_t id = makeId(m_me, ++m_counter);
        auto str = serialize(obj);
        ops.push_back({OpKind::Create, id, str});
        registerObj(obj, id, std::move(str));
    }

    // 2. Objects that were deleted.
    Ref<CCArray> removed = m_removed;
    m_removed = CCArray::create();
    for (auto obj : CCArrayExt<GameObject*, false>(removed)) {
        auto id = idOf(obj);
        if (!id) continue;
        if (!editor || lockedByOther(core, *id)) {
            revert(core, *id, false);
            notice(editor ? "That object is being edited by " + core.nicknameOf(core.lockOwner(*id)) : "Viewers cannot edit the level");
            continue;
        }
        ops.push_back({OpKind::Delete, *id, {}});
        unregister(*id);
    }

    // 3. The selection = objects being edited. Lock them; drop the ones someone else holds.
    auto sel = selection();
    if (!editor && !sel.empty()) {
        ui->deselectAll();
        sel.clear();
        notice("Viewers cannot edit the level");
    }
    std::unordered_set<uint64_t> selIds;
    std::unordered_set<GameObject*> selObjs;
    for (auto obj : sel) {
        auto id = idOf(obj);
        if (!id) continue;
        if (lockedByOther(core, *id)) {
            ui->deselectObject(obj);
            notice("Being edited by " + core.nicknameOf(core.lockOwner(*id)));
            continue;
        }
        selIds.insert(*id);
        selObjs.insert(obj);
    }
    std::vector<uint64_t> lockSet(selIds.begin(), selIds.end());
    std::sort(lockSet.begin(), lockSet.end());
    if (lockSet != m_lastLockSet) {
        core.setLocks(lockSet);
        m_lastLockSet = lockSet;
    }

    // Poll selected objects for changes; bigger selections are polled less often.
    double interval = std::clamp(selIds.size() / 2000.0, 0.1, 1.0);
    bool selectionChanged = selIds != m_lastSelected;
    if (selectionChanged || now - m_lastSelCheck >= interval) {
        m_lastSelCheck = now;
        auto check = selIds;
        if (selectionChanged) check.insert(m_lastSelected.begin(), m_lastSelected.end()); // final state of released objects
        for (auto id : check) {
            auto it = m_byId.find(id);
            if (it == m_byId.end()) continue;
            auto str = serialize(it->second.obj);
            if (str == it->second.localStr) continue;
            it->second.localStr = str;
            ops.push_back({OpKind::Modify, id, std::move(str)});
        }
        m_lastSelected = std::move(selIds);
    }

    // 4. Background verification of everything else.
    if (settled) scanChunk(core, ops, selObjs);

    // 5. Level settings (song, colors, background, ...).
    if (now - m_lastHeaderCheck >= kHeaderInterval) {
        m_lastHeaderCheck = now;
        auto h = currentHeader();
        if (h != m_lastHeader) {
            m_lastHeader = h;
            if (editor) ops.push_back({OpKind::Header, 0, h});
        }
    }

    core.submit(std::move(ops));
}

void EditorSync::scanChunk(ClientCore& core, std::vector<Op>& ops, std::unordered_set<GameObject*> const& selected) {
    auto arr = m_lel->m_objects;
    unsigned count = arr->count();
    bool editor = canEdit(core.role());
    for (unsigned n = 0; n < kScanPerTick; ++n) {
        if (m_scanCursor >= count) {
            finishPass(core, ops);
            return;
        }
        auto obj = static_cast<GameObject*>(arr->objectAtIndex(m_scanCursor++));
        auto id = idOf(obj);
        if (!id) {
            // Missed by the creation hook.
            if (editor) {
                uint64_t nid = makeId(m_me, ++m_counter);
                auto str = serialize(obj);
                ops.push_back({OpKind::Create, nid, str});
                registerObj(obj, nid, std::move(str));
            } else {
                m_applying = true;
                m_lel->removeObject(obj, true);
                m_applying = false;
                --m_scanCursor;
                --count;
            }
            continue;
        }
        auto& e = m_byId[*id];
        e.seenPass = m_pass;
        if (selected.contains(obj)) continue;
        auto str = serialize(obj);
        if (str == e.localStr) continue;
        if (editor && !lockedByOther(core, *id)) {
            e.localStr = str;
            ops.push_back({OpKind::Modify, *id, std::move(str)});
        } else {
            revert(core, *id, true);
            return; // the array changed under us; continue next frame
        }
    }
}

void EditorSync::finishPass(ClientCore& core, std::vector<Op>& ops) {
    m_scanCursor = 0;
    std::vector<uint64_t> missing;
    for (auto const& [id, e] : m_byId)
        if (e.seenPass != m_pass) missing.push_back(id);
    ++m_pass;
    if (missing.empty()) return;
    // Objects can shift in the array during a pass, so confirm against the full array before calling them deleted.
    std::unordered_set<GameObject*> present;
    for (auto obj : CCArrayExt<GameObject*, false>(m_lel->m_objects)) present.insert(obj);
    bool editor = canEdit(core.role());
    for (auto id : missing) {
        auto& e = m_byId[id];
        if (present.contains(e.obj)) {
            e.seenPass = m_pass;
            continue;
        }
        if (editor && !lockedByOther(core, id)) {
            ops.push_back({OpKind::Delete, id, {}});
            unregister(id);
        } else {
            revert(core, id, false);
        }
    }
}

// ------------------------------------------------------------------ apply

void EditorSync::applyOps(std::vector<Op> const& ops, bool own, bool force) {
    if (!m_lel) return;
    m_applying = true;
    std::unordered_set<GameObject*> selected;
    for (auto obj : selection()) selected.insert(obj);
    for (auto const& op : ops) {
        switch (op.kind) {
            case OpKind::Header:
                if (own && !force) break;
                if (normalizeHeader(op.data) != m_lastHeader) m_needsReload = true;
                break;
            case OpKind::Create:
            case OpKind::Modify: {
                auto it = m_byId.find(op.id);
                if (own && !force) {
                    // Echo of our own change: keep local state if we deleted it since, are still editing it,
                    // or nothing changed. Otherwise another op was ordered before ours and ours must win again.
                    if (it == m_byId.end() || selected.contains(it->second.obj) || it->second.localStr == op.data) break;
                }
                if (it != m_byId.end() && it->second.localStr == op.data) break;
                replaceObject(op.id, op.data);
                break;
            }
            case OpKind::Delete:
                if (m_byId.contains(op.id)) replaceObject(op.id, {});
                break;
        }
    }
    m_applying = false;
}

void EditorSync::updateLockLabels(ClientCore const& core) {
    if (!alive()) return;
    std::unordered_set<uint32_t> owners;
    for (auto const& [owner, ids] : core.locks()) {
        if (owner == core.me() || ids.empty()) continue;
        float minX = 1e9f, maxX = -1e9f, maxY = -1e9f;
        int found = 0;
        for (auto id : ids) {
            auto it = m_byId.find(id);
            if (it == m_byId.end()) continue;
            auto p = it->second.obj->getPosition();
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
            if (++found >= 500) break;
        }
        if (!found) continue;
        owners.insert(owner);
        auto& label = m_labels[owner];
        if (!label) {
            label = CCLabelBMFont::create(core.nicknameOf(owner).c_str(), "bigFont.fnt");
            label->setScale(0.45f);
            label->setColor({255, 220, 80});
            m_lel->m_objectLayer->addChild(label, 9999);
        }
        label->setString(core.nicknameOf(owner).c_str());
        label->setPosition({(minX + maxX) / 2.f, maxY + 25.f});
    }
    for (auto it = m_labels.begin(); it != m_labels.end();) {
        if (owners.contains(it->first)) { ++it; continue; }
        if (it->second) it->second->removeFromParent();
        it = m_labels.erase(it);
    }
}

namespace {
CCNode* playIcon() {
    // The same play button GD shows on a level's page.
    for (auto frame : {"GJ_playBtn2_001.png", "GJ_playBtn_001.png"})
        if (auto spr = CCSprite::createWithSpriteFrameName(frame)) return spr;
    return CCLabelBMFont::create(">", "bigFont.fnt");
}


// The look part of a play state; the player sprite is rebuilt only when this changes.
uint64_t lookKey(msg::PlayState const& s) {
    return ((uint64_t(s.cube) << 52) ^ (uint64_t(s.mode) << 48)) | (uint64_t(s.icon) << 32) | (uint64_t(s.color1) << 24)
         | (uint64_t(s.color2) << 16) | (uint64_t(s.glowColor) << 8) | (s.flags & msg::PlayState::Glow);
}

SimplePlayer* makeIcon(int id, IconType type, msg::PlayState const& s) {
    auto gm = GameManager::get();
    auto p = SimplePlayer::create(1);
    p->updatePlayerFrame(id, type);
    p->setColors(gm->colorForIdx(s.color1), gm->colorForIdx(s.color2));
    if (s.flags & msg::PlayState::Glow) p->setGlowOutline(gm->colorForIdx(s.glowColor));
    else p->disableGlowOutline();
    return p;
}

constexpr int kTagPlayer = 1, kTagTitle = 2;
} // namespace

void EditorSync::updatePlayMarkers(ClientCore const& core, double now) {
    if (!alive()) return;
    std::string names;
    std::unordered_set<uint32_t> shown;
    for (auto const& [peer, buffer] : core.plays()) {
        if (peer == core.me()) continue;
        auto state = buffer.at(now); // what GD reported, ~100 ms ago, interpolated between real samples
        if (!state) continue;
        auto const& play = *state;
        shown.insert(peer);
        auto& marker = m_playMarkers[peer];
        bool fresh = !marker;
        if (fresh) {
            marker = CCNode::create();
            // Name with the level play icon, floating above the player.
            auto title = CCNode::create();
            auto icon = playIcon();
            icon->setScale(16.f / std::max(1.f, icon->getContentHeight()));
            title->addChild(icon);
            auto name = CCLabelBMFont::create(core.nicknameOf(peer).c_str(), "goldFont.fnt");
            name->setScale(0.5f);
            name->setAnchorPoint({0.f, 0.5f});
            title->addChild(name);
            float w = 18.f + name->getScaledContentWidth();
            icon->setPosition({-w / 2.f + 8.f, 0.f});
            name->setPosition({-w / 2.f + 18.f, 0.f});
            title->setTag(kTagTitle);
            marker->addChild(title, 2);
            m_lel->m_objectLayer->addChild(marker, 10000);
        }
        auto player = static_cast<SimplePlayer*>(marker->getChildByTag(kTagPlayer));
        if (!player || m_playLooks[peer] != lookKey(play)) {
            if (player) player->removeFromParent();
            player = makeIcon(play.icon, static_cast<IconType>(play.mode), play);
            if (play.mode == 1 || play.mode == 3) {
                // Ship and UFO carry the player's cube, small and inside, like GD draws it.
                auto rider = makeIcon(play.cube, IconType::Cube, play);
                rider->setScale(0.55f);
                auto c = player->getContentSize();
                rider->setPosition({c.width / 2.f, c.height / 2.f + (play.mode == 1 ? 5.f : 6.f)});
                player->addChild(rider, -1);
            }
            player->setTag(kTagPlayer);
            marker->addChild(player, 1);
            m_playLooks[peer] = lookKey(play);
        }
        float sx = play.scale * ((play.flags & msg::PlayState::FacingLeft) ? -1.f : 1.f);
        float sy = play.scale * ((play.flags & msg::PlayState::UpsideDown) ? -1.f : 1.f);
        player->setScaleX(sx);
        player->setScaleY(sy);
        marker->setPosition({play.x, play.y});
        player->setRotation(play.rotation);
        marker->getChildByTag(kTagTitle)->setPosition({0.f, 26.f * play.scale + 8.f});
        if (!names.empty()) names += ", ";
        names += core.nicknameOf(peer);
    }
    for (auto it = m_playMarkers.begin(); it != m_playMarkers.end();) {
        if (shown.contains(it->first)) { ++it; continue; }
        if (it->second) it->second->removeFromParent();
        m_playLooks.erase(it->first);
        it = m_playMarkers.erase(it);
    }

    auto ui = m_lel->m_editorUI;
    if (names.empty() || !ui) {
        if (m_playBanner) m_playBanner->removeFromParent();
        m_playBanner = nullptr;
        return;
    }
    std::string text = names + (m_playMarkers.size() == 1 ? " is playing" : " are playing");
    if (!m_playBanner) {
        m_playBanner = CCNode::create();
        auto icon = playIcon();
        icon->setScale(22.f / std::max(1.f, icon->getContentHeight()));
        icon->setTag(1);
        m_playBanner->addChild(icon);
        auto label = CCLabelBMFont::create("", "goldFont.fnt");
        label->setAnchorPoint({0.f, 0.5f});
        label->setTag(2);
        m_playBanner->addChild(label);
        auto win = CCDirector::get()->getWinSize();
        m_playBanner->setPosition({win.width / 2.f, win.height - 62.f});
        ui->addChild(m_playBanner, 100);
    }
    auto label = static_cast<CCLabelBMFont*>(m_playBanner->getChildByTag(2));
    label->setString(text.c_str());
    label->setScale(std::min(0.6f, 260.f / std::max(1.f, label->getContentWidth())));
    float w = 26.f + label->getScaledContentWidth();
    m_playBanner->getChildByTag(1)->setPosition({-w / 2.f + 11.f, 0.f});
    label->setPosition({-w / 2.f + 26.f, 0.f});
}

} // namespace et