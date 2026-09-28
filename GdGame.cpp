#include "GdGame.hpp"
#include "Export.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace geode::prelude;

namespace rpf::game {

static std::uint64_t hashMix(std::uint64_t h, double v) {
    auto q = static_cast<std::int64_t>(std::llround(v * 10000.0));
    h ^= static_cast<std::uint64_t>(q) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

GdGame::GdGame(PlayLayer* layer, RestoreMode mode) : m_layer(layer), m_mode(mode) {}
GdGame::~GdGame() { detach(); }

void GdGame::detach() {
    m_snaps.clear();
    if (!m_layer) return;
    releaseAll();
    m_layer = nullptr;
}

Tick GdGame::tick() const {
    // In GD 2.2081 m_currentProgress counts half ticks: +2 per full physics step.
    return m_layer ? static_cast<Tick>(m_layer->m_gameState.m_currentProgress / 2) : 0;
}

double GdGame::progress() const { return m_layer ? m_layer->getCurrentPercent() : 0; }

void GdGame::press(std::uint8_t player, bool down) {
    m_injecting = true;
    m_layer->handleButton(down, 1, player == 1);
    m_injecting = false;
}

void GdGame::releaseAll() {
    m_injecting = true;
    for (bool p1 : {true, false}) m_layer->handleButton(false, 1, p1);
    m_injecting = false;
    m_layer->m_queuedButtons.clear();
    if (m_layer->m_player1) m_layer->m_player1->releaseAllButtons();
    if (m_layer->m_player2) m_layer->m_player2->releaseAllButtons();
    m_layer->m_queuedButtons.clear();
    m_applied = 0;
}

void GdGame::restart() {
    m_resetting = true;
    m_layer->m_isTestMode = true;
    m_layer->resetLevelFromStart();
    m_resetting = false;
    m_layer->m_extraDelta = 0;
    m_layer->m_resumeTimer = 0;
    releaseAll();
    m_desired = 0;
    m_dead = m_completed = false;
    if (m_layer->m_gameState.m_currentProgress != 0)
        m_clockError = "Level restart did not start at physics tick 0.";
}

void GdGame::injectInputs() {
    if (!m_inStep || m_desired == m_applied) return;
    for (std::uint8_t player : {1, 2}) {
        auto bit = playerBit(player);
        if ((m_desired & bit) != (m_applied & bit)) press(player, (m_desired & bit) != 0);
    }
    m_applied = m_desired;
}

StepResult GdGame::step() {
    if (!m_layer) return StepResult::Dead;
    m_dead = m_completed = false;
    auto before = m_layer->m_gameState.m_currentProgress;
    for (int attempt = 0; attempt < 4 && m_layer->m_gameState.m_currentProgress == before; ++attempt) {
        // Exactly one physics step: GD rounds (dt + extra) to multiples of
        // min(timeWarp, 1) / 240, so give it exactly one quantum.
        m_layer->m_extraDelta = 0;
        m_layer->m_resumeTimer = 0;
        auto warp = std::min(1.0f, m_layer->m_gameState.m_timeWarp);
        if (warp <= 0) warp = 1;
        m_inStep = true;
        stepOriginal(m_layer, static_cast<float>(warp / 240.0));
        m_inStep = false;
        if (m_dead) break;
    }
    auto advanced = m_layer->m_gameState.m_currentProgress - before;
    if (!m_dead && advanced != 2 && m_clockError.empty()) {
        m_clockError = fmt::format("Physics clock advanced by {} half-ticks instead of 2. "
            "Disable TPS/speedhack/Click Between Frames mods.", advanced);
        log::warn("[Real Pathfinder] {}", m_clockError);
    }
    if (m_dead) return StepResult::Dead;
    if (m_completed || m_layer->m_levelEndAnimationStarted || m_layer->m_hasCompletedLevel) return StepResult::Completed;
    if (advanced == 0) return StepResult::Dead; // game refused to advance: treat as a failure
    return StepResult::Alive;
}

std::uint64_t GdGame::stateKey() const {
    std::uint64_t h = m_applied;
    for (auto p : {m_layer->m_player1, m_layer->m_player2}) {
        if (!p) continue;
        h = hashMix(h, p->getPositionX());
        h = hashMix(h, p->getPositionY());
        h = hashMix(h, p->m_yVelocity);
        h = hashMix(h, p->getRotation());
        h = hashMix(h, p->m_vehicleSize);
        h = hashMix(h, p->m_playerSpeed);
        int flags = p->m_isShip | p->m_isBall << 1 | p->m_isBird << 2 | p->m_isDart << 3 | p->m_isRobot << 4 |
            p->m_isSpider << 5 | p->m_isSwing << 6 | p->m_isUpsideDown << 7 | p->m_isOnGround << 8 | p->m_isDashing << 9;
        h = hashMix(h, flags);
    }
    return h;
}

Snap GdGame::save() {
    if (!m_layer) return kNoSnap;
    SnapData d;
    d.checkpoint = m_layer->createCheckpoint();
    if (!d.checkpoint) return kNoSnap;
    d.p1.save(m_layer->m_player1);
    d.p2.save(m_layer->m_player2);
    d.pos1 = m_layer->m_player1->getPosition();
    d.pos2 = m_layer->m_player2->getPosition();
    d.rot1 = m_layer->m_player1->getRotation();
    d.rot2 = m_layer->m_player2->getRotation();
    auto rings = [](PlayerObject* p, std::vector<Ref<CCObject>>& out) {
        if (!p->m_touchingRings) return;
        for (auto obj : CCArrayExt<CCObject*>(p->m_touchingRings)) out.emplace_back(obj);
    };
    rings(m_layer->m_player1, d.rings1);
    rings(m_layer->m_player2, d.rings2);
    d.queued = m_layer->m_queuedButtons;
    d.extraDelta = m_layer->m_extraDelta;
    d.randomSeed = m_layer->m_randomSeed;
    d.fastSeed = GameToolbox::getfast_srand();
    d.raw = m_layer->m_gameState.m_currentProgress;
    d.levelTime = m_layer->m_gameState.m_levelTime;
    d.applied = m_applied;
    d.desired = m_desired;
    auto id = m_next++;
    if (m_next == kNoSnap) m_next = 1;
    m_snaps.emplace(id, std::move(d));
    return id;
}

bool GdGame::load(Snap id) {
    auto it = m_snaps.find(id);
    if (it == m_snaps.end() || !m_layer) return false;
    auto& d = it->second;
    m_resetting = true;
    if (m_mode == RestoreMode::PracticeStyle && m_layer->m_isPracticeMode && m_layer->m_checkpointArray) {
        // Same path as a practice-mode respawn: the whole level is reset, then the
        // checkpoint is applied, so triggers that fired later are undone too.
        m_layer->m_checkpointArray->removeAllObjects();
        m_layer->m_checkpointArray->addObject(d.checkpoint);
        m_layer->resetLevel();
        m_layer->m_checkpointArray->removeAllObjects();
    } else {
        m_layer->loadFromCheckpoint(d.checkpoint);
    }
    m_resetting = false;
    // GD checkpoints skip some player fields; restore all of them.
    d.p1.load(m_layer->m_player1);
    d.p2.load(m_layer->m_player2);
    m_layer->m_player1->setPosition(d.pos1);
    m_layer->m_player2->setPosition(d.pos2);
    m_layer->m_player1->setRotation(d.rot1);
    m_layer->m_player2->setRotation(d.rot2);
    auto rings = [](PlayerObject* p, std::vector<Ref<CCObject>> const& in) {
        if (!p->m_touchingRings) return;
        p->m_touchingRings->removeAllObjects();
        for (auto const& obj : in) p->m_touchingRings->addObject(obj);
    };
    rings(m_layer->m_player1, d.rings1);
    rings(m_layer->m_player2, d.rings2);
    m_layer->m_queuedButtons = d.queued;
    m_layer->m_extraDelta = d.extraDelta;
    m_layer->m_randomSeed = d.randomSeed;
    GameToolbox::fast_srand(d.fastSeed);
    m_layer->m_resumeTimer = 0;
    m_applied = d.applied;
    m_desired = d.desired;
    m_dead = m_completed = false;
    return m_layer->m_gameState.m_currentProgress == d.raw &&
        std::abs(m_layer->m_gameState.m_levelTime - d.levelTime) < 1e-7;
}

void GdGame::drop(Snap id) { m_snaps.erase(id); }

// ------------------------------------------------------------------ Runtime

static std::int64_t intSetting(char const* key) { return Mod::get()->getSettingValue<std::int64_t>(key); }

Runtime::~Runtime() { detach(); }

void Runtime::detach() {
    if (finder) finder.reset();   // drops checkpoints through the game first
    if (game) game.reset();
    if (layer && controlling) {
        layer->m_isTestMode = originalTestMode;
    }
    controlling = false;
}

std::string Runtime::start() {
    if (!layer) return "No level is open.";
    if (layer->m_isPlatformer) return "Platformer levels are not supported (they need left/right inputs).";
    if (layer->m_startPosObject) return "This run started from a Start Pos. Disable Start Pos and start from 0%.";
    if (running()) return "The pathfinder is already running.";
    if (finder && !exported && finder->phase() != Phase::Idle) exportMacro();

    auto restoreSetting = Mod::get()->getSettingValue<std::string>("restore-mode");
    auto mode = restoreSetting.find("Practice") != std::string::npos ? RestoreMode::PracticeStyle : RestoreMode::Direct;

    finder.reset();
    game.reset();
    originalTestMode = layer->m_isTestMode;
    originalPractice = layer->m_isPracticeMode;
    if (mode == RestoreMode::PracticeStyle && !layer->m_isPracticeMode) layer->togglePracticeMode(true);
    if (layer->m_checkpointArray) layer->m_checkpointArray->removeAllObjects();
    layer->m_isTestMode = true;

    SearchSettings s;
    s.snapshotInterval = static_cast<int>(intSetting("checkpoint-interval"));
    s.maxSnapshots = static_cast<std::size_t>(intSetting("max-checkpoints"));
    s.maxNodes = static_cast<std::uint64_t>(intSetting("max-branches"));
    s.repairBack = 240 * intSetting("repair-seconds");
    s.twoPlayer = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
    s.maxTicks = 240LL * 60 * intSetting("max-level-minutes");

    game = std::make_unique<GdGame>(layer, mode);
    finder = std::make_unique<Pathfinder>(*game, s);
    controlling = true;
    exported = false;
    lastExport.clear();
    startedAt = rateAt = std::chrono::steady_clock::now();
    rateSteps = 0;
    stepsPerSecond = 0;
    finder->start();
    log::info("[Real Pathfinder] Started. twoPlayer={} restore={}", s.twoPlayer, restoreSetting);
    return {};
}

void Runtime::stop() {
    if (finder) finder->stop();
    finishIfEnded();
}

void Runtime::frame() {
    if (!running()) return;
    auto budget = std::chrono::milliseconds(std::clamp<std::int64_t>(intSetting("work-ms"), 4, 200));
    auto deadline = std::chrono::steady_clock::now() + budget;
    finder->work(~0ull, [&] { return std::chrono::steady_clock::now() < deadline && game->clockError().empty(); });
    if (!game->clockError().empty() && running()) finder->stop();
    FMODAudioEngine::sharedEngine()->pauseAllMusic(true);

    auto now = std::chrono::steady_clock::now();
    auto seconds = std::chrono::duration<double>(now - rateAt).count();
    if (seconds >= 1.0) {
        stepsPerSecond = (finder->stats().steps - rateSteps) / seconds;
        rateSteps = finder->stats().steps;
        rateAt = now;
    }
    finishIfEnded();
}

void Runtime::finishIfEnded() {
    if (!finder || running() || exported) return;
    auto summaryText = exportMacro();
    log::info("[Real Pathfinder] Finished: {}", summaryText);
    Notification::create(finder->phase() == Phase::Done ? "Pathfinder: route found and saved!" : "Pathfinder stopped - partial route saved",
        finder->phase() == Phase::Done ? NotificationIcon::Success : NotificationIcon::Warning, 4.f)->show();
}

static std::string sanitize(std::string name) {
    for (auto& c : name) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    if (name.empty()) name = "level";
    return name.substr(0, 60);
}

std::string Runtime::exportMacro() {
    if (!finder || !layer) return "Nothing to export.";
    exported = true;
    bool complete = finder->phase() == Phase::Done;
    auto flips = complete ? finder->solution() : finder->verifiedPrefix();
    bool verified = true;
    if (!complete && flips.empty()) { flips = finder->bestPath(); verified = false; }

    MacroInfo info;
    info.levelName = std::string(layer->m_level->m_levelName);
    info.levelId = layer->m_level->m_levelID.value();
    info.complete = complete;
    info.lowDetail = layer->m_lowDetailMode;
    info.twoPlayer = finder->settings().twoPlayer;
    info.endTick = complete ? finder->solutionEndTick() : (verified ? finder->stats().verifiedTick : finder->stats().bestTick);

    auto dir = Mod::get()->getSaveDir() / "macros";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto stamp = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &stamp);
#else
    localtime_r(&stamp, &tm);
#endif
    std::ostringstream name;
    name << sanitize(info.levelName) << "_" << info.levelId << "_" << std::put_time(&tm, "%Y%m%d-%H%M%S")
         << (complete ? "" : "_partial");
    auto base = dir / name.str();
    try {
        auto analyzerPath = std::filesystem::path(base.string() + ".json");
        auto gdrPath = std::filesystem::path(base.string() + ".gdr.json");
        auto a = file::writeString(analyzerPath, toAnalyzerJson(flips, info));
        auto g = file::writeString(gdrPath, toGdrJson(flips, info));
        if (a.isErr() || g.isErr()) return lastExport = "Could not write the macro files.";
        std::ostringstream out;
        out << (complete ? "Full route, verified from 0%" :
                verified ? fmt::format("Partial route, verified up to {:.1f}%", finder->stats().verifiedPercent) :
                           fmt::format("Partial route (unverified), reaches {:.1f}%", finder->stats().bestPercent))
            << "\n" << flips.size() << " inputs\nSaved as:\n" << analyzerPath.filename().string()
            << "\n" << gdrPath.filename().string();
        return lastExport = out.str();
    } catch (std::exception const& e) {
        return lastExport = std::string("Could not export: ") + e.what();
    }
}

std::string Runtime::summary() const {
    if (!finder) return "Pause > Pathfinder to start";
    auto const& st = finder->stats();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt).count();
    std::ostringstream out;
    out << "Real Pathfinder: " << toString(finder->phase()) << "\n" << finder->message() << "\n"
        << fmt::format("Best {:.2f}%   Verified {:.2f}%   Branches {}   Repairs {}\n", st.bestPercent, st.verifiedPercent, st.nodes, st.repairs)
        << fmt::format("{:.0f} steps/sec ({:.1f}x)   Checkpoints {}   Time {:02}:{:02}", stepsPerSecond, stepsPerSecond / 240.0,
               st.liveSnapshots, elapsed / 60, elapsed % 60);
    if (game && !game->clockError().empty()) out << "\nERROR: " << game->clockError();
    if (!running() && !lastExport.empty()) out << "\n" << lastExport << "\nExit the level when you're done (Esc).";
    else out << "\nEsc: pause / stop";
    return out.str();
}

} // namespace rpf::game
