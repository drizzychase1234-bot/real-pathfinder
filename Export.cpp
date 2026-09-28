#include "Export.hpp"
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace rpf {

static std::string escape(std::string const& in) {
    std::string out;
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += static_cast<char>(c);
        }
    }
    return out;
}

void validateFlips(std::vector<Flip> const& flips) {
    bool held[3] = {false, false, false};
    Tick last = -1;
    for (auto const& f : flips) {
        if (f.tick < 0 || f.tick < last) throw std::runtime_error("Inputs are not in tick order.");
        if (f.player != 1 && f.player != 2) throw std::runtime_error("Invalid player number.");
        if (held[f.player] == f.down) throw std::runtime_error("Inputs do not alternate press/release.");
        held[f.player] = f.down;
        last = f.tick;
    }
}

static Tick safeEnd(std::vector<Flip> const& flips, MacroInfo const& info) {
    Tick end = info.endTick;
    if (!flips.empty() && end <= flips.back().tick) end = flips.back().tick + 1;
    // Leave room for the end-of-level animation before the level reports completion.
    return end + 240 * 20;
}

std::string toAnalyzerJson(std::vector<Flip> const& flips, MacroInfo const& info) {
    validateFlips(flips);
    std::ostringstream out;
    out << "{\n";
    out << "  \"format\": \"frame-window-replay\",\n";
    out << "  \"version\": 1,\n";
    out << "  \"tps\": 240,\n";
    out << "  \"endTick\": " << safeEnd(flips, info) << ",\n";
    out << "  \"author\": \"" << escape(info.author) << (info.complete ? "" : " (partial route)") << "\",\n";
    out << "  \"level\": {\"id\": " << info.levelId << ", \"name\": \"" << escape(info.levelName) << "\"},\n";
    out << "  \"inputs\": [";
    for (std::size_t i = 0; i < flips.size(); ++i) {
        auto const& f = flips[i];
        out << (i ? ",\n    " : "\n    ");
        out << "{\"id\": " << (i + 1) << ", \"tick\": " << f.tick << ", \"press\": " << (f.down ? "true" : "false")
            << ", \"player\": " << int(f.player) << ", \"button\": 1}";
    }
    out << (flips.empty() ? "]\n" : "\n  ]\n");
    out << "}\n";
    return out.str();
}

std::string toGdrJson(std::vector<Flip> const& flips, MacroInfo const& info) {
    validateFlips(flips);
    std::ostringstream out;
    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"author\": \"" << escape(info.author) << "\",\n";
    out << "  \"description\": \"" << (info.complete ? "Verified full route" : "Partial route") << "\",\n";
    out << "  \"duration\": " << (static_cast<double>(safeEnd(flips, info)) / 240.0) << ",\n";
    out << "  \"gameVersion\": 2.2081,\n";
    out << "  \"framerate\": 240,\n";
    out << "  \"seed\": 0,\n";
    out << "  \"coins\": 0,\n";
    out << "  \"ldm\": " << (info.lowDetail ? "true" : "false") << ",\n";
    out << "  \"platformer\": false,\n";
    out << "  \"bot\": {\"name\": \"Real Pathfinder\", \"version\": \"1.0.0\"},\n";
    out << "  \"level\": {\"id\": " << info.levelId << ", \"name\": \"" << escape(info.levelName) << "\"},\n";
    out << "  \"inputs\": [";
    for (std::size_t i = 0; i < flips.size(); ++i) {
        auto const& f = flips[i];
        out << (i ? ",\n    " : "\n    ");
        out << "{\"frame\": " << f.tick << ", \"btn\": 1, \"2p\": " << (f.player == 2 ? "true" : "false")
            << ", \"down\": " << (f.down ? "true" : "false") << "}";
    }
    out << (flips.empty() ? "]\n" : "\n  ]\n");
    out << "}\n";
    return out.str();
}

} // namespace rpf
