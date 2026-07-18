#include "CaptureNode.hpp"
#include "MenuPopup.hpp"
#include "RecorderManager.hpp"

#ifdef GEODE_IS_ANDROID
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#endif

#include <limits>
#include <string>

using namespace geode::prelude;

namespace {
CCScale9Sprite* makeIndicatorPanel() {
    auto panel = CCScale9Sprite::create("square02_001.png");
    panel->setContentSize({ 72.f, 26.f });
    panel->setOpacity(210);
    return panel;
}
}

CaptureNode* CaptureNode::create() {
    auto node = new CaptureNode();
    if (node && node->init()) {
        node->autorelease();
        return node;
    }
    CC_SAFE_DELETE(node);
    return nullptr;
}

bool CaptureNode::init() {
    if (!CCNode::init()) return false;
    setID("gd-screen-recorder-capture-node");

    // Draw as close to "last" as possible within this node's own parent
    // (the overlay manager), so nothing this mod adds shows up in the
    // captured frame itself.
    setZOrder(std::numeric_limits<int>::max());

    auto panel = makeIndicatorPanel();
    panel->setPosition({ 60.f, CCDirector::get()->getWinSize().height - 20.f });
    panel->setID("gd-screen-recorder-indicator-panel");
    panel->setVisible(false);
    addChild(panel, std::numeric_limits<int>::max());
    m_indicatorPanel = panel;

    m_indicatorLabel = CCLabelBMFont::create("REC", "bigFont.fnt");
    m_indicatorLabel->setScale(.3f);
    m_indicatorLabel->setPosition(panel->getContentSize() / 2.f);
    m_indicatorLabel->setID("gd-screen-recorder-indicator-label");
    panel->addChild(m_indicatorLabel);

#ifdef GEODE_IS_ANDROID
    // Android has no physical F8 key on normal touch devices. Keep a compact
    // touch entry point in the persistent overlay; CaptureNode captures in
    // draw() before its children are visited, so this control is not burned
    // into the recorded video.
    auto touchMenu = CCMenu::create();
    touchMenu->setPosition({ 0.f, 0.f });
    touchMenu->setContentSize(CCDirector::get()->getWinSize());
    auto touchSprite = ButtonSprite::create(
        "REC", 42, true, "bigFont.fnt", "GJ_button_01.png", 24.f, .45f
    );
    touchSprite->m_label->setScale(.3f);
    auto touchButton = CCMenuItemSpriteExtra::create(
        touchSprite, this, menu_selector(CaptureNode::onOpenMenu)
    );
    auto size = CCDirector::get()->getWinSize();
    touchButton->setPosition({ size.width - 28.f, size.height - 24.f });
    touchMenu->addChild(touchButton);
    addChild(touchMenu, std::numeric_limits<int>::max());
#endif

    return true;
}

void CaptureNode::onOpenMenu(CCObject*) {
    toggleRecorderMenu();
}

void CaptureNode::updateIndicator() {
    if (!m_indicatorPanel || !m_indicatorLabel) return;

    bool show = RecorderManager::get().isRecording()
        && Mod::get()->getSettingValue<bool>("show-indicator");

    m_indicatorPanel->setVisible(show);
    if (show) {
        auto text = RecorderManager::get().isPaused()
            ? std::string("PAUSED")
            : RecorderManager::get().statusText();
        m_indicatorLabel->setString(text.c_str());
    }
}

void CaptureNode::draw() {
    // Update the indicator's text/visibility right before capturing, so the
    // indicator itself is included in the recorded frame like GD's own UI.
    updateIndicator();

    CCNode::draw();

    auto captured = RecorderManager::get().captureFrame();
    if (captured.isErr()) {
        log::error("Frame capture failed: {}", captured.unwrapErr());
        if (!RecorderManager::get().isStopping()) {
            RecorderManager::get().stopAsync([](Result<std::filesystem::path>) {});
        }
        Notification::create("Recording stopped: capture error", NotificationIcon::Error, 3.f)->show();
    }
}
