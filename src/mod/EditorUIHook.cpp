#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>

#include "session/Session.hpp"
#include "ui/Popups.hpp"

using namespace geode::prelude;

class $modify(ETEditorUI, EditorUI) {
    struct Fields {
        Ref<CCNode> badge;
        Ref<CCLabelBMFont> badgeCount;
    };

    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;

        // Sit next to the trash button (top-left undo/redo row) so we follow GD's own layout.
        auto anchor = m_trashBtn;
        auto menu = anchor ? anchor->getParent() : nullptr;
        if (!menu) {
            log::warn("EditTogether: trash button menu not found, editor button not added");
            return true;
        }

        CCNode* icon = CCSprite::createWithSpriteFrameName("accountBtn_friends_001.png");
        if (!icon) icon = CCLabelBMFont::create("ET", "bigFont.fnt");
        auto spr = EditorButtonSprite::create(icon, EditorBaseColor::Cyan);
        spr->setScale(anchor->getScaledContentHeight() / spr->getContentHeight());

        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](auto) {
            // With join requests waiting, go straight to where they're accepted.
            auto host = et::Session::get()->host();
            if (host && host->pendingCount() > 0) {
                if (auto p = et::ui::SessionPopup::create()) p->show();
            } else if (auto p = et::ui::MainPopup::create()) {
                p->show();
            }
        });
        btn->setID("edittogether-button"_spr);
        btn->setPosition(anchor->getPosition() + ccp(
            (anchor->getScaledContentWidth() + btn->getScaledContentWidth()) / 2.f + 6.f, 0.f
        ));
        menu->addChild(btn);
        if (menu->getLayout()) menu->updateLayout();

        // Red count badge for pending join requests (top-right corner of the button).
        auto badge = CCNode::create();
        auto dot = CCDrawNode::create();
        // drawDot renders a square in GD's cocos build, so draw the circle as a polygon.
        CCPoint circle[24];
        for (int i = 0; i < 24; ++i) circle[i] = ccp(9.f * std::cos(i * M_PI / 12.f), 9.f * std::sin(i * M_PI / 12.f));
        dot->drawPolygon(circle, 24, {0.9f, 0.1f, 0.1f, 1.f}, 1.f, {1.f, 1.f, 1.f, 1.f});
        badge->addChild(dot);
        auto count = CCLabelBMFont::create("1", "bigFont.fnt");
        count->setScale(0.4f);
        badge->addChild(count);
        // The button sits at the top edge of the screen; keep the badge fully on screen.
        badge->setPosition({btn->getContentWidth() - 4.f, btn->getContentHeight() - 11.f});
        badge->setScale(0.9f);
        badge->setVisible(false);
        btn->addChild(badge, 10);
        m_fields->badge = badge;
        m_fields->badgeCount = count;
        this->schedule(schedule_selector(ETEditorUI::updateJoinBadge), 0.25f);
        return true;
    }

    void updateJoinBadge(float) {
        auto host = et::Session::get()->host();
        int pending = host ? host->pendingCount() : 0;
        auto badge = m_fields->badge.data();
        if (!badge) return;
        badge->setVisible(pending > 0);
        if (pending > 0) m_fields->badgeCount->setString(pending > 9 ? "9+" : std::to_string(pending).c_str());
    }
};
