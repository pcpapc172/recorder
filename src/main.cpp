#include <Geode/Geode.hpp>
#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/OverlayManager.hpp>

#include "CaptureNode.hpp"
#include "MenuPopup.hpp"
#include "RecorderManager.hpp"
#include "SavingPopup.hpp"

using namespace geode::prelude;

// Recording, the REC indicator, and the F8 menu are all handled by a single
// persistent overlay node + a keybind listener, set up once here.
//
// CaptureNode is added to OverlayManager instead of PlayLayer, which is what
// makes recording and the indicator work in every layer (menus, the pause
// screen, the editor, etc), not just gameplay - Geode installs
// OverlayManager as CCDirector's notification node, which is always drawn
// last, after the entire running scene. This is also Geode's documented way
// to keep a node alive across scene changes (see mods/guidelines-tips.md).
//
// $on_game(Loaded) (rather than $on_mod(Loaded)) matches Geode's own
// documented pattern for registering keybind listeners (mods/settings.md).
$on_game(Loaded) {
    auto capture = CaptureNode::create();
    if (capture) {
        OverlayManager::get()->addChild(capture);
    } else {
        log::error("Failed to create GD Screen Recorder's capture node.");
    }

    listenForKeybindSettingPresses("toggle-menu-keybind", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;
        toggleRecorderMenu();
    });
}
