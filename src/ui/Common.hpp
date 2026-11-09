#pragma once

#include <Geode/Geode.hpp>

#include <chrono>

namespace et::ui {

inline double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

inline CCMenuItemSpriteExtra* textButton(
    char const* text, char const* bg, float scale, geode::Function<void(CCMenuItemSpriteExtra*)> cb
) {
    auto spr = ButtonSprite::create(text, "goldFont.fnt", bg, scale);
    return geode::cocos::CCMenuItemExt::createSpriteExtra(spr, std::move(cb));
}

inline cocos2d::CCLabelBMFont* label(std::string const& text, char const* font, float scale, float maxWidth = 0) {
    auto l = cocos2d::CCLabelBMFont::create(text.c_str(), font);
    l->setScale(scale);
    if (maxWidth > 0 && l->getScaledContentWidth() > maxWidth) l->setScale(scale * maxWidth / l->getScaledContentWidth());
    return l;
}

// A list row: dark strip with left-aligned text lines and a right-aligned button menu.
inline cocos2d::CCNode* row(float width, float height, std::vector<std::pair<std::string, char const*>> const& lines, cocos2d::CCMenu* buttons) {
    auto node = cocos2d::CCLayerColor::create({0, 0, 0, 70}, width, height);
    float y = height - 12.f;
    for (auto const& [text, font] : lines) {
        bool big = std::string_view(font) == "bigFont.fnt";
        auto l = label(text, font, big ? 0.4f : 0.55f, width * 0.62f);
        l->setAnchorPoint({0.f, 0.5f});
        l->setPosition({8.f, y});
        node->addChild(l);
        y -= big ? 15.f : 13.f;
    }
    if (buttons) {
        buttons->setContentSize({width * 0.36f, height});
        buttons->setAnchorPoint({1.f, 0.5f});
        buttons->ignoreAnchorPointForPosition(false);
        buttons->setPosition({width - 4.f, height / 2.f});
        buttons->setLayout(geode::RowLayout::create()->setGap(4.f)->setAxisAlignment(geode::AxisAlignment::End));
        node->addChild(buttons);
    }
    return node;
}

} // namespace et::ui
