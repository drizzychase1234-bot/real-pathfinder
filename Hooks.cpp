#include "GdGame.hpp"
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

using namespace geode::prelude;
using namespace rpf;
using namespace rpf::game;

// Hook layout follows the Frame Window Analyzer mod (MIT), whose approach was
// verified live against GD 2.2081: step GJBaseGameLayer::update one tick at a
// time, inject inputs before processCommands, intercept deaths/completion.

class $modify(RPFPlayLayer, PlayLayer) {
    struct Fields { std::unique_ptr<Runtime> runtime; };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        m_fields->runtime = std::make_unique<Runtime>(this);
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto r = runtimeFor(this);
        if (r && r->controlling && r->game) {
            // GD's internal anti-cheat probe also calls destroyPlayer without
            // killing anyone; let the game handle that one normally.
            if (object && m_anticheatSpike && object == m_anticheatSpike) { PlayLayer::destroyPlayer(player, object); return; }
            r->game->onDeath();
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        auto r = runtimeFor(this);
        if (r && r->controlling && r->game) { r->game->onComplete(); return; }
        PlayLayer::levelComplete();
    }

    void resetLevel() {
        auto r = runtimeFor(this);
        // While searching, only the pathfinder may reset the level.
        if (r && r->running() && r->game && !r->game->resetting()) return;
        PlayLayer::resetLevel();
    }

    void updateAttempts() {
        auto r = runtimeFor(this);
        if (r && r->controlling) return;
        PlayLayer::updateAttempts();
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        // Practice auto-checkpoints would be loaded by our resets; keep them out.
        auto r = runtimeFor(this);
        if (r && r->controlling) return;
        PlayLayer::storeCheckpoint(checkpoint);
    }

    void onQuit() {
        if (auto r = m_fields->runtime.get()) { r->stop(); r->detach(); }
        PlayLayer::onQuit();
    }
};

namespace rpf::game {
Runtime* runtimeFor(PlayLayer* layer) {
    if (!layer) return nullptr;
    return static_cast<RPFPlayLayer*>(layer)->m_fields->runtime.get();
}
Runtime* active() { return runtimeFor(PlayLayer::get()); }
}

class $modify(RPFBaseLayer, GJBaseGameLayer) {
    struct Fields {
        Ref<CCLayerColor> backdrop;
        CCLabelBMFont* text = nullptr;
    };

    Runtime* mine() {
        auto r = active();
        return r && static_cast<GJBaseGameLayer*>(r->layer) == this ? r : nullptr;
    }

    void rpfStep(float dt) { GJBaseGameLayer::update(dt); }

    void update(float dt) {
        auto r = mine();
        if (!r || !r->controlling) { GJBaseGameLayer::update(dt); return; }
        // While the search owns the level, the normal game loop never runs.
        if (r->game && r->game->inStep()) { GJBaseGameLayer::update(dt); return; }
        if (r->layer->m_isPaused) return;
        r->frame();
        if (m_fields->text) {
            auto text = r->summary();
            m_fields->text->setString(text.c_str());
        }
    }

    void visit() {
        auto r = mine();
        if (!r || !r->controlling) { GJBaseGameLayer::visit(); return; }
        // Don't draw the level while searching (it would only slow things down).
        auto& f = *m_fields.operator->();
        if (!f.backdrop) {
            auto size = CCDirector::sharedDirector()->getWinSize();
            f.backdrop = CCLayerColor::create({14, 20, 32, 255}, size.width, size.height);
            f.text = CCLabelBMFont::create("", "chatFont.fnt");
            f.text->setAlignment(kCCTextAlignmentCenter);
            f.text->setAnchorPoint({0.5f, 0.5f});
            f.text->setPosition({size.width / 2, size.height / 2});
            f.text->setScale(0.65f);
            f.backdrop->addChild(f.text);
        }
        f.backdrop->visit();
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto r = mine();
        if (r && r->controlling && r->game && r->game->inStep()) r->game->injectInputs();
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto r = mine();
        // Ignore your own clicks while the search is controlling the player.
        if (r && r->controlling && r->game && !r->game->injecting()) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

namespace rpf::game {
void stepOriginal(GJBaseGameLayer* layer, float dt) { static_cast<RPFBaseLayer*>(layer)->rpfStep(dt); }
}

class $modify(GJGameLevel) {
    void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts, bool isChkValid) {
        auto r = active();
        if (r && r->controlling && r->layer->m_level == this) return; // never save pathfinder runs
        GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
    }
};

class $modify(PlayerObject) {
    void incrementJumps() {
        auto r = active();
        if (r && r->controlling && (r->layer->m_player1 == this || r->layer->m_player2 == this)) return;
        PlayerObject::incrementJumps();
    }
};
