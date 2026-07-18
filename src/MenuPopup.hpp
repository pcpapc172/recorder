#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>

// Opens/closes the recorder popup for both the desktop keybind and the
// Android touch button.
void toggleRecorderMenu();

// The popup opened by the "Open Recorder Menu" keybind (default F8).
// Replaces the old pause-screen button; this works from any layer, since
// it's spawned directly on the currently running scene rather than being
// tied to PauseLayer.
class GDSRMenuPopup final : public geode::Popup {
public:
    static GDSRMenuPopup* create();

protected:
    // Not a virtual override - this mirrors Geode's own popup tutorial
    // (MyPopup), where a same-named init() calls the protected
    // Popup::init(width, height, ...) internally, then builds the UI.
    bool init();
    void update(float dt) override;

private:
    void onToggleRecording(cocos2d::CCObject*);
    void onTogglePause(cocos2d::CCObject*);
    void onFpsDelta(cocos2d::CCObject* sender);
    void onBitrateDelta(cocos2d::CCObject* sender);
    void onCycleContainer(cocos2d::CCObject*);
    void onCycleCodec(cocos2d::CCObject*);
    void onCycleMicrophone(cocos2d::CCObject*);
    void onToggleIndicator(cocos2d::CCObject*);
    void onToggleAudioCapture(cocos2d::CCObject*);
    void onToggleMicrophone(cocos2d::CCObject*);

    void refreshStatusLabel();
    void refreshFpsLabel();
    void refreshBitrateLabel();
    void refreshContainerLabel();
    void refreshCodecLabel();
    void refreshMicrophoneLabel();
    void refreshRecordButton();
    void refreshPauseButton();

    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    cocos2d::CCLabelBMFont* m_fpsLabel = nullptr;
    cocos2d::CCLabelBMFont* m_bitrateLabel = nullptr;
    cocos2d::CCLabelBMFont* m_containerLabel = nullptr;
    cocos2d::CCLabelBMFont* m_codecLabel = nullptr;
    cocos2d::CCLabelBMFont* m_microphoneLabel = nullptr;
    CCMenuItemSpriteExtra* m_recordButton = nullptr;
    CCMenuItemSpriteExtra* m_pauseButton = nullptr;
};
