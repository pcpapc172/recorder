#pragma once

#include <Geode/Geode.hpp>

// A persistent overlay node that captures the finished framebuffer every
// frame and shows a small "REC" indicator, regardless of which scene/layer
// is currently on screen (gameplay, pause menu, level list, editor, etc).
//
// This is added once to geode::OverlayManager (see main.cpp), which Geode
// installs as CCDirector's m_pNotificationNode - cocos2d-x always draws the
// notification node last, after the running scene has been fully drawn, so
// draw() here sees the complete composited frame no matter what's showing.
class CaptureNode final : public cocos2d::CCNode {
public:
    static CaptureNode* create();
    bool init() override;
    void draw() override;

private:
    void onOpenMenu(cocos2d::CCObject*);
    void updateIndicator();

    cocos2d::CCNode* m_indicatorPanel = nullptr;
    cocos2d::CCLabelBMFont* m_indicatorLabel = nullptr;
};
