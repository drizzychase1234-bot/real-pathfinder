#pragma once
#include "Types.hpp"
#include <functional>
#include <list>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace rpf {

enum class Phase { Idle, Searching, Verifying, Done, Failed, Stopped };
char const* toString(Phase phase);

struct Stats {
    std::uint64_t steps = 0, restarts = 0, loads = 0, loadFailures = 0;
    std::uint64_t nodes = 0, candidates = 0, desyncs = 0, duplicates = 0;
    std::uint64_t repairs = 0, verifications = 0;
    std::size_t liveSnapshots = 0;
    Tick bestTick = 0;
    double bestPercent = 0;
    Tick verifiedTick = 0;     // a clean run from 0% survived at least this far
    double verifiedPercent = 0;
};

// Death-driven best-first search over button timings, running on a real
// (or toy) game through IGame.
//
// How it works:
//  1. Simulate with the current buttons until the player dies at tick D.
//  2. The only useful changes are button flips at ticks <= D. Try them from the
//     latest tick backwards. A flip that survives past D is progress.
//  3. Always continue from the branch that got furthest, but every time a branch
//     is expanded its score drops, so dead ends eventually hand control back to
//     earlier alternatives (backtracking).
//  4. Among neighbouring working timings, prefer the middle of the window.
//  5. A found route is replayed from 0% with no checkpoint loads. If that clean
//     run dies, the area around the death is searched again in exact mode
//     (every branch replayed from 0%) until the clean run completes.
class Pathfinder {
public:
    Pathfinder(IGame& game, SearchSettings settings);
    ~Pathfinder();
    Pathfinder(Pathfinder const&) = delete;
    Pathfinder& operator=(Pathfinder const&) = delete;

    void start();
    void stop();
    // Performs work until `maxUnits` steps are used or keepGoing() is false.
    void work(std::uint64_t maxUnits, std::function<bool()> const& keepGoing = {});

    Phase phase() const { return m_phase; }
    std::string const& message() const { return m_message; }
    Stats const& stats() const { return m_stats; }
    SearchSettings const& settings() const { return m_settings; }

    // Verified complete route (only valid when phase() == Done).
    std::vector<Flip> const& solution() const { return m_solution; }
    Tick solutionEndTick() const { return m_solutionEnd; }
    // Best route found so far and how far it got (may be unverified).
    std::vector<Flip> bestPath() const;
    // Inputs of the verified prefix (clean run survives to stats().verifiedTick).
    std::vector<Flip> verifiedPrefix() const;

private:
    struct Node {
        std::int32_t parent = -1;
        Flip flip;               // tick -1 for the root
        std::uint8_t held = 0;   // buttons held during this node's segment
        Tick lo = 0;             // earliest tick a child flip may use
        Tick death = 0;          // tick of the fatal (or completing) step
        bool completed = false;
        float percent = 0;
        Tick cursor = 0;         // next candidate tick (descending)
        std::uint8_t cursorPlayer = 1;
        bool exhausted = false;
        int batches = 0;
        int margin = 0;
        std::int32_t group = 0;  // siblings from one timing window share a penalty
        std::uint32_t version = 0;
        std::vector<std::pair<Tick, Snap>> snaps;
    };
    struct Job {
        enum class Kind { Root, Candidate, Verify } kind = Kind::Root;
        std::int32_t node = -1;
        std::vector<Flip> flips;
        Tick reachTick = 0;
        std::optional<Flip> extra;
        bool keepSnaps = false;
        Snap from = kNoSnap;
        Tick fromTick = 0;
        bool begun = false, reached = false;
        std::size_t cursor = 0;
        std::uint8_t mask = 0;
        std::vector<std::pair<Tick, Snap>> snaps;
        bool done = false, desync = false;
        StepResult result = StepResult::Alive;
        Tick endTick = 0;
        std::uint64_t key = 0;
        float percent = 0;
    };
    struct Entry {
        double score;
        std::int32_t node;
        std::uint32_t version;
        bool operator<(Entry const& o) const { return score < o.score; }
    };
    struct Zone { Tick a, b; };
    struct RunItem { std::int32_t child; Tick tick; };
    struct Batch {
        std::int32_t node = -1;
        int evaluated = 0;
        bool runEnded = false;
        std::vector<RunItem> run[2];
    };
    struct SnapInfo { std::int32_t owner; std::list<Snap>::iterator it; };

    IGame& m_game;
    SearchSettings m_settings;
    Phase m_phase = Phase::Idle;
    std::string m_message;
    Stats m_stats;

    std::vector<Node> m_nodes;
    std::priority_queue<Entry> m_frontier;
    // Branches that die at the same place (same obstacle) share one penalty group.
    std::unordered_map<Tick, std::int32_t> m_deathGroups;
    std::int32_t groupFor(Tick death);
    // Same parent + identical death: only the earliest flip is worth expanding.
    std::unordered_map<std::uint64_t, std::int32_t> m_siblingDeaths;
    std::optional<Job> m_job;
    std::optional<Batch> m_batch;
    std::int32_t m_best = -1;
    std::vector<int> m_groupBatches;
    std::int32_t newGroup();

    std::vector<Flip> m_prefix; // verified inputs before m_lo
    Tick m_lo = 0;
    std::vector<Zone> m_zones;
    std::vector<Tick> m_failures;

    std::vector<Flip> m_candidateSolution;
    std::vector<Flip> m_solution;
    Tick m_solutionEnd = 0;

    std::list<Snap> m_lru;
    std::unordered_map<Snap, SnapInfo> m_snapInfo;

    // search
    void resetTree();
    void planNext();
    void beginBatch(std::int32_t node);
    void nextCandidate();
    void endBatch();
    void finalizeRun(int playerIndex, bool lowEdgeIsFailure);
    void push(std::int32_t node);
    double score(Node const& node) const;
    std::vector<Flip> pathTo(std::int32_t node) const;
    void chooseStart(Job& job, std::int32_t node, Tick target);
    bool zoneOk(Tick snapTick, Tick target) const;
    void consume(Job& job);
    void consumeRoot(Job& job);
    void consumeCandidate(Job& job);
    void consumeVerify(Job& job);
    void startVerify(std::vector<Flip> flips);
    void repair(Tick deathTick);
    void finish(Phase phase, std::string message);

    // jobs
    std::uint64_t advance(Job& job);

    // checkpoint bookkeeping
    Snap takeSnap(std::int32_t owner);
    void touchSnap(Snap snap);
    void dropSnap(Snap snap);
    void dropSnaps(std::vector<std::pair<Tick, Snap>>& snaps);
    void reassign(std::vector<std::pair<Tick, Snap>> const& snaps, std::int32_t owner);
    void evictIfNeeded();
};

} // namespace rpf
