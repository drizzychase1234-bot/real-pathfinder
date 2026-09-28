// Standalone tests for the search core using a small GD-like toy game.
// Build: cmake -S . -B build-tests -DRPF_BUILD_MOD=OFF && cmake --build build-tests && ./build-tests/rpf_tests
#include "Export.hpp"
#include "Search.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

using namespace rpf;

enum Mode { Cube, Ship, Wave };

struct Rect { double x0, x1, y0, y1; };
struct Portal { double x; int mode = -1; double speed = 0; bool flipGravity = false; };
struct Orb { double x, y; };

struct Level {
    std::string name;
    double length = 200;
    double ceiling = 1e9;          // ship/wave only
    std::vector<Rect> kill;        // spikes, walls
    std::vector<Rect> solid;       // platforms (cube lands on top, dies on side/bottom)
    std::vector<Portal> portals;
    std::vector<Orb> orbs;
};

struct State {
    Tick tick = 0;
    double x = 0, y = 0, vy = 0, speed = 1;
    int mode = Cube;
    bool flipped = false, onGround = true;
    std::uint8_t held = 0, prevHeld = 0;
    std::uint32_t usedOrbs = 0;
};

class ToyGame final : public IGame {
public:
    Level level;
    State s;
    std::uint8_t desired = 0;
    std::map<Snap, State> snaps;
    Snap next = 1;
    std::function<void(State&)> loadNoise; // simulates imperfect checkpoint restores
    std::uint64_t steps = 0;

    explicit ToyGame(Level l) : level(std::move(l)) {}

    void restart() override { s = {}; desired = 0; }
    Tick tick() const override { return s.tick; }
    void setHeld(std::uint8_t mask) override { desired = mask; }
    std::uint8_t held() const override { return desired; }
    double progress() const override { return std::min(100.0, 100.0 * s.x / level.length); }
    std::uint64_t stateKey() const override {
        auto q = [](double v) { return static_cast<std::uint64_t>(std::llround(v * 1000.0)); };
        std::uint64_t h = q(s.x) * 1000003ull ^ q(s.y) * 998244353ull ^ q(s.vy) * 7919ull;
        return h ^ (static_cast<std::uint64_t>(s.mode) << 56) ^ (static_cast<std::uint64_t>(desired) << 60)
            ^ (static_cast<std::uint64_t>(s.usedOrbs) << 40) ^ (s.flipped ? 1ull << 63 : 0);
    }
    Snap save() override { auto id = next++; snaps[id] = s; snaps[id].held = desired; return id; }
    bool load(Snap id) override {
        auto it = snaps.find(id);
        if (it == snaps.end()) return false;
        s = it->second; desired = s.held;
        if (loadNoise) loadNoise(s);
        return true;
    }
    void drop(Snap id) override { snaps.erase(id); }

    StepResult step() override {
        ++steps;
        s.held = desired;
        bool pressing = s.held & 1;
        bool pressEdge = pressing && !(s.prevHeld & 1);
        double oldX = s.x, oldY = s.y;
        double nx = s.x + s.speed;
        for (auto const& p : level.portals) if (p.x > oldX && p.x <= nx) {
            if (p.mode >= 0 && p.mode != s.mode) { s.mode = p.mode; s.vy = 0; s.onGround = false; }
            if (p.speed > 0) { s.speed = p.speed; nx = s.x + s.speed; }
            if (p.flipGravity) s.flipped = !s.flipped;
        }
        double g = s.flipped ? 0.08 : -0.08;
        switch (s.mode) {
            case Cube: {
                if (s.onGround && pressing) { s.vy = s.flipped ? -1.2 : 1.2; s.onGround = false; }
                for (std::size_t i = 0; i < level.orbs.size(); ++i) {
                    auto const& o = level.orbs[i];
                    if (pressEdge && !(s.usedOrbs & (1u << i)) && std::abs(s.x - o.x) < 1.5 && std::abs(s.y - o.y) < 1.5) {
                        s.vy = s.flipped ? -1.3 : 1.3; s.usedOrbs |= 1u << i; s.onGround = false;
                    }
                }
                s.vy += g;
                s.y += s.vy;
                s.x = nx;
                bool supported = false;
                if (s.y <= 0) { s.y = 0; s.vy = 0; supported = true; }
                for (auto const& b : level.solid) {
                    if (s.x < b.x0 || s.x > b.x1) continue;
                    if (!s.flipped) {
                        if (oldY >= b.y1 - 1e-9 && s.y < b.y1) { s.y = b.y1; s.vy = 0; supported = true; }  // landed (no tunneling)
                        else if (s.y < b.y1 && s.y + 1.0 > b.y0) { s.tick++; s.prevHeld = s.held; return StepResult::Dead; }
                    } else {
                        if (oldY <= b.y0 + 1e-9 && s.y > b.y0) { s.y = b.y0; s.vy = 0; supported = true; }  // landed upside down
                        else if (s.y > b.y0 && s.y - 1.0 < b.y1) { s.tick++; s.prevHeld = s.held; return StepResult::Dead; }
                    }
                }
                s.onGround = supported;
                break;
            }
            case Ship: {
                s.vy += pressing ? 0.05 : -0.05;
                s.vy = std::clamp(s.vy, -0.7, 0.7);
                s.y += s.vy;
                s.x = nx;
                if (s.y < 0.5 || s.y > level.ceiling - 0.5) { s.tick++; s.prevHeld = s.held; return StepResult::Dead; }
                break;
            }
            case Wave: {
                s.vy = pressing ? s.speed : -s.speed;
                s.y += s.vy;
                s.x = nx;
                if (s.y < 0.5 || s.y > level.ceiling - 0.5) { s.tick++; s.prevHeld = s.held; return StepResult::Dead; }
                break;
            }
        }
        for (auto const& k : level.kill)
            if (s.x >= k.x0 && s.x <= k.x1 && s.y >= k.y0 && s.y <= k.y1) { s.tick++; s.prevHeld = s.held; return StepResult::Dead; }
        (void)oldX;
        s.prevHeld = s.held;
        s.tick++;
        if (s.x >= level.length) return StepResult::Completed;
        return StepResult::Alive;
    }
};

static Rect spike(double x, double h, double base = 0) { return {x - 0.6, x + 0.6, base - 0.5, base + h}; }

static Level cubeLevel() {
    Level l; l.name = "cube spikes"; l.length = 320;
    l.kill = {spike(40, 5), spike(90, 5), spike(130, 5), spike(131.5, 5), spike(200, 6), spike(240, 3), spike(290, 5)};
    return l;
}
static Level shipLevel() {
    Level l; l.name = "ship corridor + speed portal"; l.length = 240; l.ceiling = 20;
    l.portals = {{15, Ship}, {120, -1, 1.4}};
    l.kill = {{60, 62, 0, 8}, {100, 102, 12, 20}, {140, 142, 0, 10}, {180, 182, 9, 20}, {215, 217, 0, 11}};
    return l;
}
static Level waveLevel() {
    Level l; l.name = "wave zigzag"; l.length = 170; l.ceiling = 20;
    l.portals = {{10, Wave}};
    l.kill = {{40, 42, 0, 10}, {70, 72, 8, 20}, {100, 102, 0, 12}, {130, 132, 6, 20}, {150, 152, 0, 9}};
    return l;
}
static Level orbLevel() {
    Level l; l.name = "orb over tall wall"; l.length = 160;
    // Wall too tall for a normal jump (peak ~9): needs jump + fresh click in the orb.
    l.kill = {{75, 76, 0, 13}, spike(30, 4), spike(130, 5)};
    l.orbs = {{62, 6}};
    return l;
}
static Level trapLevel() {
    Level l; l.name = "dead-end trap (needs backtracking)"; l.length = 230;
    // Jumping the first spike late lands on a platform that ends in a wall (dead end,
    // but it survives longer than the right route). Jumping early goes underneath.
    l.solid = {{48, 150, 3, 4}};
    l.kill = {spike(40, 4), {148, 149, 3.5, 200}, spike(175, 4), spike(205, 4)};
    return l;
}
static Level gravityLevel() {
    Level l; l.name = "gravity portal + ceiling"; l.length = 200;
    l.solid = {{80, 200, 14, 15}};                   // ceiling to walk on upside down
    l.portals = {{75, -1, 0, true}};
    l.kill = {spike(40, 5), {120, 121, 11, 14}, spike(170, 3)};
    return l;
}

static bool replayGenuine(Level const& level, std::vector<Flip> const& flips, Tick* deathTick = nullptr) {
    ToyGame game(level);
    game.restart();
    std::size_t cursor = 0;
    std::uint8_t mask = 0;
    for (Tick t = 0; t < 240 * 60 * 5; ++t) {
        while (cursor < flips.size() && flips[cursor].tick == t) {
            auto const& f = flips[cursor++];
            mask = f.down ? (mask | playerBit(f.player)) : (mask & ~playerBit(f.player));
        }
        game.setHeld(mask);
        auto r = game.step();
        if (r == StepResult::Completed) return true;
        if (r == StepResult::Dead) { if (deathTick) *deathTick = t; return false; }
    }
    return false;
}

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++failures; } } while (0)

static void runLevel(Level const& level, std::function<void(State&)> noise = {}, bool expectRepair = false) {
    ToyGame game(level);
    game.loadNoise = noise;
    SearchSettings settings;
    settings.maxSnapshots = 400;
    Pathfinder pf(game, settings);
    auto t0 = std::chrono::steady_clock::now();
    pf.start();
    pf.work(50'000'000);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto const& st = pf.stats();
    std::printf("[%s] %s | %s | steps=%llu restarts=%llu loads=%llu nodes=%llu cand=%llu repairs=%llu snaps=%zu | %.0f ms\n",
        level.name.c_str(), toString(pf.phase()), pf.message().c_str(),
        (unsigned long long)st.steps, (unsigned long long)st.restarts, (unsigned long long)st.loads,
        (unsigned long long)st.nodes, (unsigned long long)st.candidates, (unsigned long long)st.repairs,
        st.liveSnapshots, ms);
    CHECK(pf.phase() == Phase::Done, "search should complete");
    if (pf.phase() != Phase::Done) return;
    auto const& sol = pf.solution();
    bool ok = replayGenuine(level, sol);
    CHECK(ok, "solution must complete in a genuine replay");
    try { validateFlips(sol); } catch (std::exception const& e) { CHECK(false, e.what()); }
    CHECK(st.liveSnapshots <= settings.maxSnapshots, "snapshot cap respected");
    if (expectRepair) CHECK(st.repairs > 0, "noisy checkpoints should have triggered a repair");
    std::printf("   inputs=%zu first ticks:", sol.size());
    for (std::size_t i = 0; i < std::min<std::size_t>(sol.size(), 10); ++i)
        std::printf(" %s%lld", sol[i].down ? "+" : "-", (long long)sol[i].tick);
    std::printf("\n");
}

int main() {
    runLevel(cubeLevel());
    runLevel(shipLevel());
    runLevel(waveLevel());
    runLevel(orbLevel());
    runLevel(gravityLevel());
    {
        // The trap: the orb route survives longer but is a dead end.
        runLevel(trapLevel());
        ToyGame game(trapLevel());
        Pathfinder pf(game, {});
        pf.start(); pf.work(50'000'000);
        bool usedOrbRoute = false;
        // Replay and check the player never stands on the high platform.
        ToyGame check(trapLevel()); check.restart();
        std::size_t c = 0; std::uint8_t m = 0;
        auto const& sol = pf.solution();
        for (Tick t = 0; t < 100000; ++t) {
            while (c < sol.size() && sol[c].tick == t) { m = sol[c].down ? 1 : 0; ++c; }
            check.setHeld(m);
            auto r = check.step();
            if (check.s.y >= 3.9 && check.s.x > 50 && check.s.x < 140) usedOrbRoute = true;
            if (r != StepResult::Alive) break;
        }
        CHECK(!usedOrbRoute, "final route must avoid the dead-end platform");
    }
    // Imperfect checkpoints: loads inside x in [80,150] perturb vertical speed, like
    // GD checkpoints that do not restore every field. The clean replay must catch
    // this and the exact-mode repair must still deliver a genuinely working route.
    runLevel(cubeLevel(), [](State& s) { if (s.x > 30 && !s.onGround) s.vy += 0.05; }, true);
    runLevel(shipLevel(), [](State& s) { if (s.x > 90 && s.x < 160) s.vy -= 0.12; }, true);

    // Export format sanity.
    std::vector<Flip> flips = {{10, 1, true}, {14, 1, false}, {30, 1, true}};
    MacroInfo info; info.levelName = "Test \"Level\""; info.levelId = 123; info.endTick = 500;
    auto json = toAnalyzerJson(flips, info);
    CHECK(json.find("\"format\": \"frame-window-replay\"") != std::string::npos, "analyzer format tag");
    CHECK(json.find("Test \\\"Level\\\"") != std::string::npos, "escaped name");
    auto gdr = toGdrJson(flips, info);
    CHECK(gdr.find("\"2p\": false") != std::string::npos, "gdr input fields");
    bool threw = false;
    try { validateFlips({{5, 1, true}, {6, 1, true}}); } catch (...) { threw = true; }
    CHECK(threw, "double press rejected");

    std::printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
