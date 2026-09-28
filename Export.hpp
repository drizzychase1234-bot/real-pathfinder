#pragma once
#include "Types.hpp"
#include <string>
#include <vector>

namespace rpf {

struct MacroInfo {
    std::string levelName;
    long long levelId = 0;
    std::string author = "Real Pathfinder";
    Tick endTick = 0;       // tick after which the level is over (or where the route stops)
    bool complete = false;  // true if the route was verified through the end
    bool lowDetail = false;
    bool twoPlayer = false;
};

// Frame Window Analyzer native replay (format "frame-window-replay", version 1).
std::string toAnalyzerJson(std::vector<Flip> const& flips, MacroInfo const& info);
// GDR 1 JSON (".gdr.json"), readable by GDR-compatible bots and the analyzer.
std::string toGdrJson(std::vector<Flip> const& flips, MacroInfo const& info);
// Throws std::runtime_error if flips are not a valid alternating press/release sequence.
void validateFlips(std::vector<Flip> const& flips);

} // namespace rpf
