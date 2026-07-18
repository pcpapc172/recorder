#include "MenuPopup.hpp"
#include "RecorderManager.hpp"
#include "SavingPopup.hpp"
#include "MicrophoneCapture.hpp"

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/CCMenuItemToggler.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Scrollbar.hpp>

#include <algorithm>
#include <array>
#include <vector>

using namespace geode::prelude;

namespace {
constexpr std::array<char const*, 3> kContainers = { "mp4", "mkv", "mov" };

std::vector<std::string> availableCodecs() {
    auto codecs = ffmpeg::events::Recorder::getAvailableCodecs();
    auto current = Mod::get()->getSettingValue<std::string>("codec");
    if (std::find(codecs.begin(), codecs.end(), current) == codecs.end() && !current.empty()) {
        codecs.push_back(current);
    }
    if (codecs.empty()) codecs = { "libx264", "mpeg4" };
    return codecs;
}

// bigFont.fnt is GD's default/large Pusab bitmap font (see handbook/vol1/chap1_4)
// - it's the only bitmap font the docs describe, so it's still the right font
// to use here, but a menu full of small info text needs a much smaller scale
// than the .4f this used previously, which is why labels were rendering far
// larger than the rest of the popup's UI. .3f brings it in line with the
// scale already used for the "Show REC"/"Rec Audio" toggle labels further
// down in this file.
//
// Note: cocos2d::CCLabelBMFont::limitLabelWidth is confirmed to exist (it's
// referenced by name in tutorials/hookpriority.md), and would be a good way
// to hard-cap the dynamic status label's width too, but its parameter
// signature isn't documented anywhere in the supplied docs, so it's
// deliberately not used here rather than guessing one.
CCLabelBMFont* smallLabel(std::string const& text) {
    auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    label->setScale(.3f);
    return label;
}

CCLabelBMFont* rowLabel(char const* text) {
    auto label = CCLabelBMFont::create(text, "bigFont.fnt");
    label->setScale(.3f);
    return label;
}

CCMenuItemSpriteExtra* arrowButton(char const* text, cocos2d::CCObject* target, cocos2d::SEL_MenuHandler selector, int tag) {
    // Equal absolute width and height makes this a real square instead of
    // the wide pill shown by the previous 28x22 sprite.
    auto sprite = ButtonSprite::create(text, 28, true, "bigFont.fnt", "GJ_button_01.png", 28.f, .6f);
    sprite->m_label->setScale(.3f);
    auto item = CCMenuItemSpriteExtra::create(sprite, target, selector);
    item->setTag(tag);
    return item;
}
}

void toggleRecorderMenu() {
    auto director = CCDirector::get();
    auto scene = director ? director->getRunningScene() : nullptr;
    if (!scene) return;

    if (auto existing = scene->getChildByID("gd-screen-recorder-menu-popup")) {
        existing->removeFromParentAndCleanup(true);
        return;
    }
    if (scene->getChildByID("gd-screen-recorder-saving-popup")) {
        SavingPopup::closeIfShowing();
        return;
    }
    if (auto popup = GDSRMenuPopup::create()) popup->show();
}

GDSRMenuPopup* GDSRMenuPopup::create() {
    auto ret = new GDSRMenuPopup();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool GDSRMenuPopup::init() {
    if (!Popup::init(300.f, 270.f)) return false;

    this->setTitle("GD Screen Recorder");
    this->setID("gd-screen-recorder-menu-popup");

    // Status label
    m_statusLabel = smallLabel("");
    m_mainLayer->addChildAtPosition(m_statusLabel, Anchor::Center, { 0.f, 82.f });

    // Start/Stop recording button
    auto recordSprite = ButtonSprite::create("Start Recording", 135, true, "bigFont.fnt", "GJ_button_01.png", 24.f, .6f);
    recordSprite->m_label->setScale(.3f);
    m_recordButton = CCMenuItemSpriteExtra::create(
        recordSprite, this, menu_selector(GDSRMenuPopup::onToggleRecording)
    );
    auto pauseSprite = ButtonSprite::create("Pause", 90, true, "bigFont.fnt", "GJ_button_01.png", 24.f, .6f);
    pauseSprite->m_label->setScale(.3f);
    auto pauseDisabledSprite = ButtonSprite::create("Pause", 90, true, "bigFont.fnt", "GJ_button_04.png", 24.f, .6f);
    pauseDisabledSprite->m_label->setScale(.3f);
    m_pauseButton = CCMenuItemSpriteExtra::create(
        pauseSprite, this, menu_selector(GDSRMenuPopup::onTogglePause)
    );
    m_pauseButton->setDisabledImage(pauseDisabledSprite);
    pauseDisabledSprite->setAnchorPoint(pauseSprite->getAnchorPoint());
    pauseDisabledSprite->setPosition(pauseSprite->getPosition());
    auto recordMenu = CCMenu::create();
    recordMenu->setLayout(RowLayout::create()->setGap(6.f)->setAutoScale(false));
    recordMenu->addChild(m_recordButton);
    recordMenu->addChild(m_pauseButton);
    recordMenu->updateLayout();
    m_mainLayer->addChildAtPosition(recordMenu, Anchor::Center, { 0.f, 50.f });

    // Keep the status and record controls fixed while the settings rows use
    // Geode's documented ScrollLayer + Scrollbar widgets. This keeps the
    // popup compact and prevents future settings from extending off-screen.
    auto settingsScroll = ScrollLayer::create({ 258.f, 150.f }, true, true);
    settingsScroll->setAnchorPoint({ .5f, .5f });
    settingsScroll->ignoreAnchorPointForPosition(false);
    settingsScroll->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(4.f));
    m_mainLayer->addChildAtPosition(settingsScroll, Anchor::Center, { -6.f, -53.f });

    auto finishSettingsRow = [&](CCMenu* row) {
        row->setContentSize({ 246.f, 30.f });
        row->updateLayout();
        settingsScroll->m_contentLayer->addChild(row);
    };

    // FPS row
    {
        auto row = CCMenu::create();
        row->setLayout(RowLayout::create()->setGap(8.f)->setAutoScale(false));
        row->addChild(rowLabel("FPS"));
        row->addChild(arrowButton("-", this, menu_selector(GDSRMenuPopup::onFpsDelta), -5));
        m_fpsLabel = smallLabel("");
        row->addChild(m_fpsLabel);
        row->addChild(arrowButton("+", this, menu_selector(GDSRMenuPopup::onFpsDelta), 5));
        finishSettingsRow(row);
    }

    // Bitrate row
    {
        auto row = CCMenu::create();
        row->setLayout(RowLayout::create()->setGap(8.f)->setAutoScale(false));
        row->addChild(rowLabel("Mbps"));
        row->addChild(arrowButton("-", this, menu_selector(GDSRMenuPopup::onBitrateDelta), -1));
        m_bitrateLabel = smallLabel("");
        row->addChild(m_bitrateLabel);
        row->addChild(arrowButton("+", this, menu_selector(GDSRMenuPopup::onBitrateDelta), 1));
        finishSettingsRow(row);
    }

    // Container cycle button
    {
        auto row = CCMenu::create();
        row->setLayout(RowLayout::create()->setGap(8.f)->setAutoScale(false));
        row->addChild(rowLabel("Container"));
    auto containerSprite = ButtonSprite::create("mp4", 70, true, "bigFont.fnt", "GJ_button_01.png", 22.f, .55f);
        containerSprite->m_label->setScale(.3f);
        m_containerLabel = containerSprite->m_label;
        auto containerBtn = CCMenuItemSpriteExtra::create(
            containerSprite, this, menu_selector(GDSRMenuPopup::onCycleContainer)
        );
        row->addChild(containerBtn);
        finishSettingsRow(row);
    }

    // Show indicator + record audio toggles
    {
        auto row = CCMenu::create();
        row->setLayout(RowLayout::create()->setGap(10.f)->setAutoScale(false));

        auto indicatorOff = rowLabel("Show REC");
        row->addChild(indicatorOff);
        auto indicatorToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(GDSRMenuPopup::onToggleIndicator), .6f
        );
        indicatorToggle->toggle(Mod::get()->getSettingValue<bool>("show-indicator"));
        row->addChild(indicatorToggle);

        auto audioOff = rowLabel("Rec Audio");
        row->addChild(audioOff);
        auto audioToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(GDSRMenuPopup::onToggleAudioCapture), .6f
        );
        audioToggle->toggle(Mod::get()->getSettingValue<bool>("capture-game-audio"));
        row->addChild(audioToggle);

        finishSettingsRow(row);
    }

    // Microphone toggle and device selector. The selector stores FMOD's GUID,
    // so changing the order of devices after a reboot does not select the
    // wrong microphone.
    {
        auto row = CCMenu::create();
        row->setLayout(RowLayout::create()->setGap(8.f)->setAutoScale(false));
        row->addChild(rowLabel("Mic"));
        auto deviceSprite = ButtonSprite::create("Mic", 90, true, "bigFont.fnt", "GJ_button_01.png", 20.f, .55f);
        deviceSprite->m_label->setScale(.3f);
        m_microphoneLabel = deviceSprite->m_label;
        auto deviceButton = CCMenuItemSpriteExtra::create(
            deviceSprite, this, menu_selector(GDSRMenuPopup::onCycleMicrophone)
        );
        row->addChild(deviceButton);
        auto microphoneToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(GDSRMenuPopup::onToggleMicrophone), .6f
        );
        microphoneToggle->toggle(Mod::get()->getSettingValue<bool>("capture-microphone"));
        row->addChild(microphoneToggle);
        finishSettingsRow(row);
    }

    // Encoder selector. The API supplies the actual codecs available in the
    // installed FFmpeg build; no guessed encoder name is sent to the API.
    {
        auto row = CCMenu::create();
        row->setLayout(RowLayout::create()->setGap(8.f)->setAutoScale(false));
        row->addChild(rowLabel("Encoder"));
        auto codecSprite = ButtonSprite::create("libx264", 110, true, "bigFont.fnt", "GJ_button_01.png", 20.f, .55f);
        codecSprite->m_label->setScale(.3f);
        m_codecLabel = codecSprite->m_label;
        auto codecButton = CCMenuItemSpriteExtra::create(
            codecSprite, this, menu_selector(GDSRMenuPopup::onCycleCodec)
        );
        row->addChild(codecButton);
        finishSettingsRow(row);
    }

    settingsScroll->m_contentLayer->updateLayout();
    settingsScroll->scrollToTop();
    auto scrollbar = Scrollbar::create(settingsScroll);
    scrollbar->setAnchorPoint({ .5f, .5f });
    scrollbar->ignoreAnchorPointForPosition(false);
    m_mainLayer->addChildAtPosition(scrollbar, Anchor::Center, { 137.f, -53.f });

    refreshFpsLabel();
    refreshBitrateLabel();
    refreshContainerLabel();
    refreshCodecLabel();
    refreshMicrophoneLabel();
    refreshStatusLabel();
    refreshRecordButton();
    refreshPauseButton();

    this->scheduleUpdate();

    return true;
}

void GDSRMenuPopup::update(float) {
    refreshStatusLabel();
    refreshRecordButton();
    refreshPauseButton();
}

void GDSRMenuPopup::refreshStatusLabel() {
    if (!m_statusLabel) return;
    if (RecorderManager::get().isStopping()) {
        m_statusLabel->setString("Saving recording...");
        return;
    }
    auto status = RecorderManager::get().status();
    if (status.recording) {
        auto state = RecorderManager::get().isPaused() ? "Paused" : "Recording";
        m_statusLabel->setString(fmt::format(
            "{}  {} frames  {} dropped", state, status.framesWritten, status.framesDropped
        ).c_str());
        m_statusLabel->setScale(.3f);
    } else {
        m_statusLabel->setString("Not recording");
        m_statusLabel->setScale(.3f);
    }
}

void GDSRMenuPopup::refreshRecordButton() {
    if (!m_recordButton) return;
    auto stopping = RecorderManager::get().isStopping();
    auto sprite = static_cast<ButtonSprite*>(m_recordButton->getNormalImage());
    if (sprite) {
        sprite->setString(
            stopping ? "Saving..." : (RecorderManager::get().isRecording() ? "Stop Recording" : "Start Recording")
        );
        sprite->m_label->setScale(.3f);
    }
    m_recordButton->setEnabled(!stopping);
}

void GDSRMenuPopup::refreshPauseButton() {
    if (!m_pauseButton) return;
    auto text = RecorderManager::get().isPaused() ? "Resume" : "Pause";
    auto normal = static_cast<ButtonSprite*>(m_pauseButton->getNormalImage());
    auto disabled = static_cast<ButtonSprite*>(m_pauseButton->getDisabledImage());
    if (normal) {
        normal->setString(text);
        normal->m_label->setScale(.3f);
    }
    if (disabled) {
        disabled->setString(text);
        disabled->m_label->setScale(.3f);
        if (normal) {
            disabled->setAnchorPoint(normal->getAnchorPoint());
            disabled->setPosition(normal->getPosition());
        }
    }
    auto enabled = RecorderManager::get().isRecording() && !RecorderManager::get().isStopping();
    m_pauseButton->setEnabled(enabled);
}

void GDSRMenuPopup::refreshFpsLabel() {
    if (!m_fpsLabel) return;
    auto fps = Mod::get()->getSettingValue<int64_t>("fps");
    m_fpsLabel->setString(fmt::format("{}", fps).c_str());
    m_fpsLabel->setScale(.3f);
}

void GDSRMenuPopup::refreshBitrateLabel() {
    if (!m_bitrateLabel) return;
    auto bitrate = Mod::get()->getSettingValue<int64_t>("bitrate-mbps");
    m_bitrateLabel->setString(fmt::format("{}", bitrate).c_str());
    m_bitrateLabel->setScale(.3f);
}

void GDSRMenuPopup::refreshContainerLabel() {
    if (!m_containerLabel) return;
    auto container = Mod::get()->getSettingValue<std::string>("container");
    m_containerLabel->setString(container.c_str());
    m_containerLabel->setScale(.3f);
}

void GDSRMenuPopup::refreshCodecLabel() {
    if (!m_codecLabel) return;
    m_codecLabel->setString(Mod::get()->getSettingValue<std::string>("codec").c_str());
    m_codecLabel->setScale(.3f);
}

void GDSRMenuPopup::refreshMicrophoneLabel() {
    if (!m_microphoneLabel) return;
    auto name = MicrophoneCapture::selectedDeviceName();
    if (name.size() > 14) name = name.substr(0, 13) + "...";
    m_microphoneLabel->setString(name.c_str());
    m_microphoneLabel->setScale(.3f);
}

void GDSRMenuPopup::onToggleRecording(CCObject*) {
    if (RecorderManager::get().isStopping()) {
        // Already saving a previous stop in the background; ignore repeat
        // clicks rather than starting a second stopAsync().
        return;
    }

    if (RecorderManager::get().isRecording()) {
        SavingPopup::showIfNeeded();
        RecorderManager::get().stopAsync([self = Ref(this)](Result<std::filesystem::path> result) {
            SavingPopup::closeIfShowing();
            if (result.isErr()) {
                FLAlertLayer::create("Recorder", result.unwrapErr(), "OK")->show();
            } else {
                FLAlertLayer::create(
                    "Recording saved",
                    fmt::format("Saved to:\n<cy>{}</c>", result.unwrap().string()),
                    "OK"
                )->show();
            }
            // self keeps the settings popup alive for this callback even if
            // the player already closed it; refreshing it here is harmless
            // either way, since a removed popup just isn't visible.
            self->refreshRecordButton();
            self->refreshStatusLabel();
        });
    } else {
        auto result = RecorderManager::get().start();
        if (result.isErr()) {
            FLAlertLayer::create("Recorder", result.unwrapErr(), "OK")->show();
            return;
        }
        Notification::create("Recording started", NotificationIcon::Success, 1.5f)->show();
    }
    refreshRecordButton();
    refreshStatusLabel();
    refreshPauseButton();
}

void GDSRMenuPopup::onTogglePause(CCObject*) {
    if (RecorderManager::get().isStopping() || !RecorderManager::get().isRecording()) return;

    auto result = RecorderManager::get().isPaused()
        ? RecorderManager::get().resume()
        : RecorderManager::get().pause();
    if (result.isErr()) {
        FLAlertLayer::create("Recorder", result.unwrapErr(), "OK")->show();
        return;
    }
    refreshStatusLabel();
    refreshPauseButton();
}

void GDSRMenuPopup::onFpsDelta(CCObject* sender) {
    auto delta = static_cast<CCNode*>(sender)->getTag();
    auto current = Mod::get()->getSettingValue<int64_t>("fps");
    auto next = std::clamp<int64_t>(current + delta, 15, 360);
    Mod::get()->setSettingValue<int64_t>("fps", next);
    refreshFpsLabel();
}

void GDSRMenuPopup::onBitrateDelta(CCObject* sender) {
    auto delta = static_cast<CCNode*>(sender)->getTag();
    auto current = Mod::get()->getSettingValue<int64_t>("bitrate-mbps");
    auto next = std::clamp<int64_t>(current + delta, 1, 200);
    Mod::get()->setSettingValue<int64_t>("bitrate-mbps", next);
    refreshBitrateLabel();
}

void GDSRMenuPopup::onCycleContainer(CCObject*) {
    auto current = Mod::get()->getSettingValue<std::string>("container");
    auto it = std::find(kContainers.begin(), kContainers.end(), current);
    std::size_t index = (it == kContainers.end()) ? 0 : static_cast<std::size_t>(it - kContainers.begin());
    index = (index + 1) % kContainers.size();
    Mod::get()->setSettingValue<std::string>("container", kContainers[index]);
    refreshContainerLabel();
}

void GDSRMenuPopup::onCycleCodec(CCObject*) {
    auto codecs = availableCodecs();
    auto current = Mod::get()->getSettingValue<std::string>("codec");
    auto it = std::find(codecs.begin(), codecs.end(), current);
    auto index = it == codecs.end() ? 0u : static_cast<std::size_t>(it - codecs.begin());
    index = (index + 1) % codecs.size();
    Mod::get()->setSettingValue<std::string>("codec", codecs[index]);
    refreshCodecLabel();
}

void GDSRMenuPopup::onCycleMicrophone(CCObject*) {
    MicrophoneCapture::cycleDevice();
    refreshMicrophoneLabel();
}

void GDSRMenuPopup::onToggleIndicator(CCObject*) {
    auto current = Mod::get()->getSettingValue<bool>("show-indicator");
    Mod::get()->setSettingValue<bool>("show-indicator", !current);
}

void GDSRMenuPopup::onToggleAudioCapture(CCObject*) {
    auto current = Mod::get()->getSettingValue<bool>("capture-game-audio");
    Mod::get()->setSettingValue<bool>("capture-game-audio", !current);
}

void GDSRMenuPopup::onToggleMicrophone(CCObject*) {
    auto current = Mod::get()->getSettingValue<bool>("capture-microphone");
    Mod::get()->setSettingValue<bool>("capture-microphone", !current);
}
