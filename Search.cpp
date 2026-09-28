#include "Search.hpp"
#include <algorithm>
#include <cmath>

namespace rpf {

char const* toString(Phase phase) {
    switch (phase) {
        case Phase::Idle: return "Idle";
        case Phase::Searching: return "Searching";
        case Phase::Verifying: return "Verifying";
        case Phase::Done: return "Done";
        case Phase::Failed: return "Stopped (no route)";
        case Phase::Stopped: return "Stopped";
    }
    return "?";
}

static std::uint64_t mix(std::uint64_t a, std::uint64_t b) {
    std::uint64_t x = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2));
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

Pathfinder::Pathfinder(IGame& game, SearchSettings settings) : m_game(game), m_settings(settings) {
    m_settings.snapshotInterval = std::max(1, m_settings.snapshotInterval);
    m_settings.batchSize = std::max(1, m_settings.batchSize);
    m_settings.maxSnapshots = std::max<std::size_t>(8, m_settings.maxSnapshots);
}

Pathfinder::~Pathfinder() {
    for (auto const& [snap, info] : m_snapInfo) m_game.drop(snap);
    m_snapInfo.clear();
    m_lru.clear();
}

void Pathfinder::start() {
    resetTree();
    m_prefix.clear();
    m_lo = 0;
    m_zones.clear();
    m_failures.clear();
    m_solution.clear();
    m_candidateSolution.clear();
    m_stats = {};
    m_phase = Phase::Searching;
    m_message = "Searching";
}

void Pathfinder::stop() {
    if (m_phase == Phase::Searching || m_phase == Phase::Verifying) finish(Phase::Stopped, "Stopped by you");
}

void Pathfinder::finish(Phase phase, std::string message) {
    if (m_job) { dropSnaps(m_job->snaps); m_job.reset(); }
    m_batch.reset();
    m_phase = phase;
    m_message = std::move(message);
}

void Pathfinder::resetTree() {
    if (m_job) { dropSnaps(m_job->snaps); m_job.reset(); }
    for (auto& node : m_nodes) dropSnaps(node.snaps);
    m_nodes.clear();
    m_frontier = {};
    m_deathGroups.clear();
    m_siblingDeaths.clear();
    m_batch.reset();
    m_best = -1;
    m_groupBatches.clear();
}

std::int32_t Pathfinder::groupFor(Tick death) {
    auto bucket = death / std::max<Tick>(1, m_settings.obstacleBucket);
    auto [it, inserted] = m_deathGroups.try_emplace(bucket, -1);
    if (inserted) it->second = newGroup();
    return it->second;
}

std::int32_t Pathfinder::newGroup() {
    m_groupBatches.push_back(0);
    return static_cast<std::int32_t>(m_groupBatches.size() - 1);
}

void Pathfinder::work(std::uint64_t maxUnits, std::function<bool()> const& keepGoing) {
    std::uint64_t used = 0;
    std::uint64_t iterations = 0;
    while (used < maxUnits && (m_phase == Phase::Searching || m_phase == Phase::Verifying)) {
        if (keepGoing && (++iterations & 15) == 0 && !keepGoing()) break;
        if (!m_job) { planNext(); continue; }
        used += advance(*m_job);
        if (m_job && m_job->done) {
            Job job = std::move(*m_job);
            m_job.reset();
            consume(job);
        }
    }
}

// ---------------------------------------------------------------- planning

void Pathfinder::planNext() {
    if (m_phase != Phase::Searching) return;
    if (m_nodes.empty()) {
        Job job;
        job.kind = Job::Kind::Root;
        job.flips = m_prefix;
        job.reachTick = m_lo;
        job.keepSnaps = true;
        m_job = std::move(job);
        return;
    }
    if (m_batch) { nextCandidate(); return; }
    while (!m_frontier.empty()) {
        auto entry = m_frontier.top();
        m_frontier.pop();
        auto const& node = m_nodes[entry.node];
        if (node.version != entry.version || node.exhausted) continue;
        // Scores of siblings drop when another member of their group is expanded.
        auto current = score(node);
        if (current < entry.score - 1e-9) { m_frontier.push({current, entry.node, entry.version}); continue; }
        beginBatch(entry.node);
        return;
    }
    finish(Phase::Failed, "Tried every timing it could find and none got further. "
        "This spot may need something the search cannot reach from its current route.");
}

void Pathfinder::beginBatch(std::int32_t node) {
    Batch batch;
    batch.node = node;
    m_batch = std::move(batch);
}

void Pathfinder::nextCandidate() {
    auto& batch = *m_batch;
    auto& node = m_nodes[batch.node];
    if (batch.runEnded || batch.evaluated >= m_settings.batchSize) { endBatch(); return; }
    if (node.cursor < node.lo) { node.exhausted = true; endBatch(); return; }

    Flip flip;
    flip.tick = node.cursor;
    flip.player = node.cursorPlayer;
    flip.down = !(node.held & playerBit(flip.player));
    if (m_settings.twoPlayer && node.cursorPlayer == 1) node.cursorPlayer = 2;
    else { node.cursorPlayer = 1; --node.cursor; }
    ++batch.evaluated;

    Job job;
    job.kind = Job::Kind::Candidate;
    job.node = batch.node;
    job.flips = pathTo(batch.node);
    job.reachTick = flip.tick;
    job.extra = flip;
    job.keepSnaps = true;
    chooseStart(job, batch.node, flip.tick);
    m_job = std::move(job);
}

void Pathfinder::endBatch() {
    if (!m_batch) return;
    for (int i = 0; i < 2; ++i) if (!m_batch->run[i].empty()) finalizeRun(i, false);
    auto index = m_batch->node;
    m_batch.reset();
    auto& node = m_nodes[index];
    ++node.batches;
    ++m_groupBatches[node.group];
    if (node.cursor < node.lo) node.exhausted = true;
    if (!node.exhausted) push(index);
}

void Pathfinder::finalizeRun(int playerIndex, bool) {
    auto& run = m_batch->run[playerIndex];
    auto const length = static_cast<int>(run.size());
    for (int i = 0; i < length; ++i) {
        auto child = run[i].child;
        if (child < 0) continue;
        m_nodes[child].margin = std::min(i, length - 1 - i);
        push(child);
    }
    run.clear();
}

double Pathfinder::score(Node const& node) const {
    return static_cast<double>(node.death)
        + m_settings.marginBonus * std::min(node.margin, m_settings.maxMargin)
        - m_settings.expansionPenalty * (node.batches + m_groupBatches[node.group]);
}

void Pathfinder::push(std::int32_t index) {
    auto& node = m_nodes[index];
    if (node.exhausted || node.completed) return;
    ++node.version;
    m_frontier.push({score(node), index, node.version});
}

std::vector<Flip> Pathfinder::pathTo(std::int32_t index) const {
    std::vector<Flip> chain;
    for (auto cur = index; cur >= 0; cur = m_nodes[cur].parent)
        if (m_nodes[cur].flip.tick >= 0) chain.push_back(m_nodes[cur].flip);
    std::vector<Flip> out = m_prefix;
    out.insert(out.end(), chain.rbegin(), chain.rend());
    return out;
}

bool Pathfinder::zoneOk(Tick snapTick, Tick target) const {
    for (auto const& zone : m_zones)
        if (snapTick <= zone.b && zone.a <= target) return false;
    return true;
}

// Pick the latest checkpoint on this branch's history at or before `target`.
// Checkpoints are never used across an exact-mode zone.
void Pathfinder::chooseStart(Job& job, std::int32_t index, Tick target) {
    Snap best = kNoSnap;
    Tick bestTick = -1;
    Tick limit = target;
    for (auto cur = index; cur >= 0; ) {
        auto const& node = m_nodes[cur];
        for (auto it = node.snaps.rbegin(); it != node.snaps.rend(); ++it) {
            if (it->first > limit) continue;
            if (it->first > bestTick && zoneOk(it->first, target)) { best = it->second; bestTick = it->first; }
            break;
        }
        if (node.flip.tick >= 0) limit = std::min(limit, node.flip.tick);
        if (limit <= bestTick) break;
        cur = node.parent;
    }
    job.from = best;
    job.fromTick = bestTick;
}

// ---------------------------------------------------------------- jobs

std::uint64_t Pathfinder::advance(Job& job) {
    if (!job.begun) {
        job.begun = true;
        bool loaded = false;
        if (job.from != kNoSnap) {
            touchSnap(job.from);
            if (m_game.load(job.from) && m_game.tick() == job.fromTick) { loaded = true; ++m_stats.loads; }
            else ++m_stats.loadFailures;
        }
        job.cursor = 0;
        job.mask = 0;
        if (loaded) {
            while (job.cursor < job.flips.size() && job.flips[job.cursor].tick <= job.fromTick) {
                auto const& f = job.flips[job.cursor++];
                job.mask = f.down ? (job.mask | playerBit(f.player)) : (job.mask & ~playerBit(f.player));
            }
            m_game.setHeld(job.mask);
            return 4;
        }
        m_game.restart();
        m_game.setHeld(0);
        ++m_stats.restarts;
        return 40;
    }

    Tick t = m_game.tick();
    while (job.cursor < job.flips.size() && job.flips[job.cursor].tick <= t) {
        auto const& f = job.flips[job.cursor++];
        if (f.tick < t) { // an input tick was skipped: the clock is not what we expect
            job.done = true; job.desync = true; job.result = StepResult::Dead; job.endTick = t;
            return 1;
        }
        job.mask = f.down ? (job.mask | playerBit(f.player)) : (job.mask & ~playerBit(f.player));
    }
    if (!job.reached && t >= job.reachTick) {
        job.reached = true;
        if (job.extra) {
            auto bit = playerBit(job.extra->player);
            job.mask = job.extra->down ? (job.mask | bit) : (job.mask & ~bit);
        }
        m_game.setHeld(job.mask);
        if (job.keepSnaps) if (auto s = takeSnap(-2)) job.snaps.push_back({t, s});
    } else {
        m_game.setHeld(job.mask);
        if (job.reached && job.keepSnaps && t % m_settings.snapshotInterval == 0)
            if (auto s = takeSnap(-2)) job.snaps.push_back({t, s});
    }
    if (job.reached && t >= m_settings.maxTicks) {
        job.done = true; job.result = StepResult::Dead; job.endTick = t;
        job.key = m_game.stateKey(); job.percent = static_cast<float>(m_game.progress());
        return 0;
    }
    auto result = m_game.step();
    ++m_stats.steps;
    if (result != StepResult::Alive) {
        job.done = true;
        job.result = result;
        job.endTick = t;
        job.desync = !job.reached;
        job.key = m_game.stateKey();
        job.percent = static_cast<float>(m_game.progress());
    }
    return 1;
}

void Pathfinder::consume(Job& job) {
    switch (job.kind) {
        case Job::Kind::Root: consumeRoot(job); break;
        case Job::Kind::Candidate: consumeCandidate(job); break;
        case Job::Kind::Verify: consumeVerify(job); break;
    }
}

void Pathfinder::consumeRoot(Job& job) {
    if (job.desync) {
        dropSnaps(job.snaps);
        finish(Phase::Failed, "The verified part of the route stopped reproducing. "
            "Check that no other gameplay mods (noclip, speedhack, TPS, CBF) are on.");
        return;
    }
    Node root;
    root.held = job.mask;
    root.lo = m_lo;
    root.death = job.endTick;
    root.completed = job.result == StepResult::Completed;
    root.percent = job.percent;
    root.cursor = root.death;
    root.snaps = std::move(job.snaps);
    root.group = groupFor(root.death);
    m_nodes.push_back(std::move(root));
    reassign(m_nodes.back().snaps, 0);
    ++m_stats.nodes;
    m_best = 0;
    if (job.endTick > m_stats.bestTick) { m_stats.bestTick = job.endTick; m_stats.bestPercent = job.percent; }
    if (m_nodes[0].completed) { startVerify(m_prefix); return; }
    push(0);
}

void Pathfinder::consumeCandidate(Job& job) {
    ++m_stats.candidates;
    int pi = job.extra->player == 2 ? 1 : 0;
    auto parentIndex = job.node;
    auto parentDeath = m_nodes[parentIndex].death;

    auto failure = [&] {
        if (m_batch && !m_batch->run[pi].empty()) { finalizeRun(pi, true); m_batch->runEnded = true; }
    };
    if (job.desync) { ++m_stats.desyncs; dropSnaps(job.snaps); failure(); return; }

    bool completed = job.result == StepResult::Completed;
    bool success = completed || job.endTick > parentDeath;
    std::int32_t child = -1;
    {
        if (m_nodes.size() >= m_settings.maxNodes) {
            dropSnaps(job.snaps);
            finish(Phase::Failed, "Hit the branch limit. Raise 'Max branches' in the mod settings to search longer.");
            return;
        }
        Node node;
        node.parent = parentIndex;
        node.flip = *job.extra;
        auto bit = playerBit(job.extra->player);
        node.held = job.extra->down ? (m_nodes[parentIndex].held | bit) : (m_nodes[parentIndex].held & ~bit);
        node.lo = job.extra->tick + 1;
        node.death = job.endTick;
        node.completed = completed;
        node.percent = job.percent;
        node.cursor = node.death;
        node.snaps = std::move(job.snaps);
        node.group = groupFor(job.endTick);
        child = static_cast<std::int32_t>(m_nodes.size());
        m_nodes.push_back(std::move(node));
        reassign(m_nodes.back().snaps, child);
        ++m_stats.nodes;
        if (job.endTick > m_stats.bestTick || completed) {
            m_stats.bestTick = job.endTick; m_stats.bestPercent = completed ? 100.0 : job.percent; m_best = child;
        }
        // A flip that did not get further can still be the setup for the next
        // one (e.g. release mid-air so the next press is a fresh click on an orb).
        if (!success) {
            m_nodes[child].batches = 1;
            auto key = mix(static_cast<std::uint64_t>(job.endTick), job.key);
            // Candidates are tried latest-first, so an older sibling with the same
            // outcome flipped later and can only try a subset of this node's timings.
            auto siblingKey = mix(key, static_cast<std::uint64_t>(parentIndex) + 1);
            auto [sib, fresh] = m_siblingDeaths.try_emplace(siblingKey, child);
            if (!fresh) {
                ++m_stats.duplicates;
                auto& older = m_nodes[sib->second];
                if (older.cursor == older.death && older.batches <= 1 && !older.exhausted) {
                    older.exhausted = true;
                    dropSnaps(older.snaps);
                }
                sib->second = child;
            }
        }
    }

    if (completed && child >= 0) { startVerify(pathTo(child)); return; }
    if (success) { if (m_batch) m_batch->run[pi].push_back({child, job.extra->tick}); }
    else {
        failure();
        if (child >= 0) push(child);
    }
}

void Pathfinder::startVerify(std::vector<Flip> flips) {
    if (m_job) { dropSnaps(m_job->snaps); m_job.reset(); }
    m_batch.reset();
    m_candidateSolution = std::move(flips);
    m_phase = Phase::Verifying;
    m_message = "Found a route - replaying it from 0% to verify";
    ++m_stats.verifications;
    Job job;
    job.kind = Job::Kind::Verify;
    job.flips = m_candidateSolution;
    job.reachTick = 0;
    job.keepSnaps = false;
    m_job = std::move(job);
}

void Pathfinder::consumeVerify(Job& job) {
    if (job.result == StepResult::Completed && !job.desync) {
        m_solution = m_candidateSolution;
        m_solutionEnd = job.endTick;
        m_stats.verifiedTick = job.endTick;
        m_stats.verifiedPercent = 100;
        finish(Phase::Done, "Route found and verified from 0%");
        return;
    }
    auto v = job.endTick;
    if (v >= m_stats.verifiedTick) { m_stats.verifiedTick = v; m_stats.verifiedPercent = job.percent; }
    repair(v);
}

void Pathfinder::repair(Tick v) {
    ++m_stats.repairs;
    if (m_stats.repairs > static_cast<std::uint64_t>(m_settings.maxRepairs)) {
        finish(Phase::Failed, "The route kept failing its clean replay. The verified part can still be exported.");
        return;
    }
    int nearby = 0;
    for (auto f : m_failures) if (f >= v - m_settings.repairBack && f <= v + m_settings.repairAhead) ++nearby;
    m_failures.push_back(v);
    Tick back = m_settings.repairBack << std::min(nearby, 5);
    Zone zone{std::max<Tick>(0, v - back), v + m_settings.repairAhead};
    m_zones.push_back(zone);

    std::vector<Flip> prefix;
    for (auto const& f : m_candidateSolution) if (f.tick < zone.a) prefix.push_back(f);
    resetTree();
    m_prefix = std::move(prefix);
    m_lo = zone.a;
    m_phase = Phase::Searching;
    m_message = "Clean replay died - re-checking that spot in exact mode (slower)";
}

std::vector<Flip> Pathfinder::bestPath() const {
    if (m_phase == Phase::Done) return m_solution;
    if (m_best >= 0 && m_best < static_cast<std::int32_t>(m_nodes.size())) return pathTo(m_best);
    return m_prefix;
}

std::vector<Flip> Pathfinder::verifiedPrefix() const {
    if (m_phase == Phase::Done) return m_solution;
    std::vector<Flip> out;
    for (auto const& f : m_candidateSolution) if (f.tick < m_stats.verifiedTick) out.push_back(f);
    if (out.empty()) for (auto const& f : m_prefix) out.push_back(f);
    return out;
}

// ---------------------------------------------------------------- checkpoints

Snap Pathfinder::takeSnap(std::int32_t owner) {
    auto snap = m_game.save();
    if (snap == kNoSnap) return kNoSnap;
    m_lru.push_back(snap);
    m_snapInfo[snap] = {owner, std::prev(m_lru.end())};
    evictIfNeeded();
    m_stats.liveSnapshots = m_snapInfo.size();
    return snap;
}

void Pathfinder::touchSnap(Snap snap) {
    auto it = m_snapInfo.find(snap);
    if (it == m_snapInfo.end()) return;
    m_lru.splice(m_lru.end(), m_lru, it->second.it);
}

void Pathfinder::dropSnap(Snap snap) {
    auto it = m_snapInfo.find(snap);
    if (it == m_snapInfo.end()) return;
    m_lru.erase(it->second.it);
    m_snapInfo.erase(it);
    m_game.drop(snap);
    m_stats.liveSnapshots = m_snapInfo.size();
}

void Pathfinder::dropSnaps(std::vector<std::pair<Tick, Snap>>& snaps) {
    for (auto const& [tick, snap] : snaps) dropSnap(snap);
    snaps.clear();
}

void Pathfinder::reassign(std::vector<std::pair<Tick, Snap>> const& snaps, std::int32_t owner) {
    for (auto const& [tick, snap] : snaps) {
        auto it = m_snapInfo.find(snap);
        if (it != m_snapInfo.end()) it->second.owner = owner;
    }
}

void Pathfinder::evictIfNeeded() {
    while (m_snapInfo.size() > m_settings.maxSnapshots && m_lru.size() > 1) {
        auto snap = m_lru.front();
        auto owner = m_snapInfo[snap].owner;
        auto erase = [snap](std::vector<std::pair<Tick, Snap>>& list) {
            list.erase(std::remove_if(list.begin(), list.end(),
                [snap](auto const& p) { return p.second == snap; }), list.end());
        };
        if (owner >= 0 && owner < static_cast<std::int32_t>(m_nodes.size())) erase(m_nodes[owner].snaps);
        else if (owner == -2 && m_job) erase(m_job->snaps);
        dropSnap(snap);
    }
}

} // namespace rpf
