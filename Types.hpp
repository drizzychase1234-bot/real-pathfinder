#pragma once
// Geode-free core types. Everything in src/core compiles without Geometry Dash
// so the search logic can be unit tested on any machine.
#include <cstdint>
#include <string>
#include <vector>

namespace rpf {

using Tick = std::int64_t;
using Snap = std::uint32_t;
constexpr Snap kNoSnap = 0;

enum class StepResult { Alive, Dead, Completed };

// One button edge. `tick` uses the Frame Window Analyzer convention:
// the input is injected immediately before the native command processing of
// physics step `tick`, where tick 0 is the first step after a restart.
struct Flip {
    Tick tick = -1;
    std::uint8_t player = 1; // 1 or 2
    bool down = false;
};

inline std::uint8_t playerBit(std::uint8_t player) { return player == 2 ? 2 : 1; }

// The game the search drives. The real implementation steps Geometry Dash's own
// physics; tests use a toy game. All calls are synchronous.
class IGame {
public:
    virtual ~IGame() = default;
    // Genuine restart from 0% with all buttons released. Never uses checkpoints.
    virtual void restart() = 0;
    // Tick of the NEXT physics step (inputs set now are injected at this tick).
    virtual Tick tick() const = 0;
    // Desired held mask (bit0 = P1, bit1 = P2), applied at the next step.
    virtual void setHeld(std::uint8_t mask) = 0;
    virtual std::uint8_t held() const = 0;
    // Advance exactly one physics step.
    virtual StepResult step() = 0;
    // Level progress in percent, for display only.
    virtual double progress() const = 0;
    // Hash of the physically relevant state, used to skip duplicate branches.
    virtual std::uint64_t stateKey() const = 0;
    // Fast (possibly imperfect) state save/restore.
    virtual Snap save() = 0;
    virtual bool load(Snap snap) = 0; // false if the restore failed validation
    virtual void drop(Snap snap) = 0;
};

struct SearchSettings {
    int snapshotInterval = 30;       // ticks between checkpoints on a branch
    std::size_t maxSnapshots = 1200; // memory cap for live checkpoints
    int batchSize = 48;              // candidate timings tried per expansion
    double expansionPenalty = 8.0;   // score (ticks) lost per expansion - drives backtracking
    double marginBonus = 2.0;        // score per tick of distance from a window edge
    int maxMargin = 8;
    Tick obstacleBucket = 24;        // deaths this close together count as the same obstacle
    Tick maxTicks = 240LL * 60 * 30; // 30 minutes of level time
    std::uint64_t maxNodes = 3'000'000;
    bool twoPlayer = false;
    Tick repairBack = 480;           // exact-mode zone before a verification death
    Tick repairAhead = 240;          // and after it
    int maxRepairs = 16;
};

} // namespace rpf
