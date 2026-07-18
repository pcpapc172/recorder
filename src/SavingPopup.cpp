#include "SavingPopup.hpp"

using namespace geode::prelude;

namespace {
// At most one instance at a time. A Ref (not a raw pointer) so the object
// can't be freed out from under us between showIfNeeded()/closeIfShowing()
// calls.
Ref<SavingPopup> g_instance;
}

SavingPopup* SavingPopup::create() {
    auto ret = new SavingPopup();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool SavingPopup::init() {
    if (!Popup::init(220.f, 90.f)) return false;

    this->setTitle("GD Screen Recorder");
    this->setID("gd-screen-recorder-saving-popup");

    m_messageLabel = CCLabelBMFont::create("Saving recording", "bigFont.fnt");
    m_messageLabel->setScale(.3f);
    m_mainLayer->addChildAtPosition(m_messageLabel, Anchor::Center, { 0.f, 0.f });

    this->scheduleUpdate();

    return true;
}

void SavingPopup::update(float dt) {
    // There's no real progress percentage available from this pipeline
    // (writeFrame()/the FFmpeg mux calls don't report one - see
    // ffmpeg-api's events.hpp/audio_mixer.hpp), so rather than fabricate a
    // fake percentage, this just shows an honest, indeterminate "..."
    // animation while the background stop() finishes.
    m_elapsed += dt;
    auto dots = static_cast<int>(m_elapsed / 0.4f) % 4;
    m_messageLabel->setString(("Saving recording" + std::string(static_cast<std::size_t>(dots), '.')).c_str());
}

void SavingPopup::showIfNeeded() {
    if (g_instance) return;
    auto popup = SavingPopup::create();
    if (!popup) return;
    popup->m_noElasticity = true;
    popup->show();
    g_instance = popup;
}

void SavingPopup::closeIfShowing() {
    if (!g_instance) return;
    if (g_instance->getParent()) {
        g_instance->removeFromParentAndCleanup(true);
    }
    g_instance = nullptr;
}
