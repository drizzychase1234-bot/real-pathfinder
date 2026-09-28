#include "GdGame.hpp"
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;
using namespace rpf;
using namespace rpf::game;

class $modify(RPFPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!active()) return;
        auto size = CCDirector::sharedDirector()->getWinSize();
        // Reuse the pause layer's own menu so touches are not swallowed.
        auto menu = this->getChildByType<CCMenu>(0);
        if (!menu) {
            menu = CCMenu::create();
            menu->setPosition({0, 0});
            this->addChild(menu, 100);
            handleTouchPriority(this);
        }
        auto sprite = ButtonSprite::create("Pathfinder", 110, true, "bigFont.fnt", "GJ_button_02.png", 28, 0.45f);
        auto button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(RPFPauseLayer::onPathfinder));
        button->setID("real-pathfinder-button"_spr);
        menu->addChild(button, 100);
        button->setPosition(menu->convertToNodeSpace(this->convertToWorldSpace({size.width - 75, size.height - 70})));
    }

    void onPathfinder(CCObject*) { showPanel(this); }
};

namespace rpf::game {

void showPanel(PauseLayer* pause) {
    auto r = active();
    if (!r) return;
    Ref<PauseLayer> keep(pause);

    if (r->running()) {
        createQuickPopup("Real Pathfinder", r->summary() + "\n\nStop now and save what it has so far?",
            "Keep going", "Stop & save", 380.f, [r, keep](FLAlertLayer*, bool stop) {
                if (!stop || active() != r) return;
                r->stop();
                FLAlertLayer::create("Saved", r->lastExport + "\n\nFolder opens now.", "OK")->show();
                file::openFolder(Mod::get()->getSaveDir() / "macros");
            });
        return;
    }

    if (r->finder && r->exported) {
        createQuickPopup("Real Pathfinder", r->lastExport +
            "\n\nThe level is frozen after a search. Exit and re-open the level to play normally, or search again.",
            "Open folder", "Search again", 380.f, [r, keep](FLAlertLayer*, bool again) {
                if (active() != r) return;
                if (!again) { file::openFolder(Mod::get()->getSaveDir() / "macros"); return; }
                FLAlertLayer::create("Re-open the level", "To search again, exit and re-open the level first "
                    "so it starts from a clean state.", "OK")->show();
            });
        return;
    }

    createQuickPopup("Real Pathfinder",
        "Find a route through this level automatically using the game's own physics.\n\n"
        "The level runs hidden at high speed. Progress is shown on screen; press Esc any time to stop and save.\n"
        "Turn off noclip, speedhack, TPS bypass and Click Between Frames first.",
        "Cancel", "Start", 380.f, [r, keep](FLAlertLayer*, bool start) {
            if (!start || active() != r) return;
            auto error = r->start();
            if (!error.empty()) { FLAlertLayer::create("Can't start", error, "OK")->show(); return; }
            if (keep) keep->onResume(nullptr);
        });
}

} // namespace rpf::game
