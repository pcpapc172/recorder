#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>

// A small "Saving recording..." popup shown while
// RecorderManager::stopAsync() drains the encoder and muxes audio in the
// background, so the caller isn't stuck freezing the game for that whole
// time. It lives at the scene level (created via show(), like any other
// Popup/FLAlertLayer), independent of GDSRMenuPopup, so closing the
// settings popup while a save is still running has no effect on it.
//
// showIfNeeded()/closeIfShowing() are guarded by a single static
// reference, so at most one instance ever exists - this is what stops a
// stale instance from lingering or a duplicate from stacking on top if the
// F8 menu is closed and reopened mid-save.
class SavingPopup final : public geode::Popup {
public:
    // No-op if an instance is already showing (including one the user
    // manually closed via the popup's own close button - once dismissed,
    // it stays dismissed rather than popping back up).
    static void showIfNeeded();

    // Removes the currently showing instance, if any. Safe to call even if
    // none exists, or if the user already closed it themselves.
    static void closeIfShowing();

protected:
    bool init();
    void update(float dt) override;

private:
    static SavingPopup* create();

    cocos2d::CCLabelBMFont* m_messageLabel = nullptr;
    float m_elapsed = 0.f;
};
