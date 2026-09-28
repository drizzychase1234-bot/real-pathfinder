#pragma once
#include "Search.hpp"
#include "PlayerFix.hpp"
#include <Geode/Geode.hpp>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace rpf::game {

enum class RestoreMode { Direct, PracticeStyle };

// Drives Geometry Dash's own physics for the search. Every step is one real
// 240 TPS physics tick; clicks are injected through the game's normal button
// handling at the exact tick, the same convention the Frame Window Analyzer uses.
class GdGame final : public IGame {
public:
    GdGame(PlayLayer* layer, RestoreMode mode);
    ~GdGame() override;

    void restart() override;
    Tick tick() const override;
    void setHeld(std::uint8_t mask) override { m_desired = mask; }
    std::uint8_t held() const override { return m_desired; }
    StepResult step() override;
    double progress() const override;
    std::uint64_t stateKey() const override;
    Snap save() override;
    bool load(Snap snap) override;
    void drop(Snap snap) override;

    // Called from hooks.
    void injectInputs();          // processCommands, before the game's own
    void onDeath() { m_dead = true; }
    void onComplete() { m_completed = true; }
    bool inStep() const { return m_inStep; }
    bool injecting() const { return m_injecting; }
    bool resetting() const { return m_resetting; }
    std::string const& clockError() const { return m_clockError; }
    void detach();

private:
    struct SnapData {
        geode::Ref<CheckpointObject> checkpoint;
        PlayerFix p1, p2;
        cocos2d::CCPoint pos1, pos2;
        float rot1 = 0, rot2 = 0;
        std::vector<geode::Ref<cocos2d::CCObject>> rings1, rings2;
        gd::vector<PlayerButtonCommand> queued;
        double extraDelta = 0;
        std::uint64_t randomSeed = 0, fastSeed = 0;
        int raw = 0;
        double levelTime = 0;
        std::uint8_t applied = 0, desired = 0;
    };
    PlayLayer* m_layer;
    RestoreMode m_mode;
    std::unordered_map<Snap, SnapData> m_snaps;
    Snap m_next = 1;
    std::uint8_t m_desired = 0, m_applied = 0;
    bool m_dead = false, m_completed = false;
    bool m_inStep = false, m_injecting = false, m_resetting = false;
    std::string m_clockError;
    void press(std::uint8_t player, bool down);
    void releaseAll();
};

struct Runtime {
    PlayLayer* layer = nullptr;
    std::unique_ptr<GdGame> game;
    std::unique_ptr<Pathfinder> finder;
    bool controlling = false;   // the search owns the game loop
    bool exported = false;
    bool originalTestMode = false, originalPractice = false;
    std::string lastExport;
    std::chrono::steady_clock::time_point startedAt, rateAt;
    std::uint64_t rateSteps = 0;
    double stepsPerSecond = 0;
    cocos2d::CCLabelBMFont* status = nullptr;
    geode::Ref<cocos2d::CCLayerColor> backdrop;
    cocos2d::CCLabelBMFont* backdropText = nullptr;

    explicit Runtime(PlayLayer* l) : layer(l) {}
    ~Runtime();
    std::string start();        // returns an error message, or empty on success
    void stop();
    void frame();               // one rendered frame of search work
    void finishIfEnded();
    std::string exportMacro();  // writes files, returns a human-readable summary
    std::string summary() const;
    bool running() const {
        return controlling && finder && (finder->phase() == Phase::Searching || finder->phase() == Phase::Verifying);
    }
    void detach();
};

Runtime* active();                  // runtime of the current PlayLayer, if any
Runtime* runtimeFor(PlayLayer* layer);
void stepOriginal(GJBaseGameLayer* layer, float dt);  // defined in Hooks.cpp
void showPanel(PauseLayer* pause);                     // defined in UI.cpp

} // namespace rpf::game
