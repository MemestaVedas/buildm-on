#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ftxui/dom/elements.hpp>
#include "theme.hpp"

// ╭──────────────────────────────────────────────────╮
// │   Flamechart · Stat Tiles · Log View             │
// ╰──────────────────────────────────────────────────╯

namespace UI {

using namespace ftxui;

// ═════════════════════════════════════════════════════
//  FLAMECHART
// ═════════════════════════════════════════════════════

struct FlameEntry {
    std::string name;        // crate/module name
    float       duration_s;  // seconds
    bool        is_slow;     // highlight in gold
};

// Pick a pastel color cycling through the palette
inline Theme::Color FlameColor(int idx, bool slow) {
    if (slow) return Theme::Gold;
    static const Theme::Color kColors[] = {
        Theme::Sky, Theme::Lavender, Theme::Mint,
        Theme::Sage, Theme::Peach,   Theme::Sky,
    };
    return kColors[idx % 6];
}

// ─────────────────────────────────────────────────────
//  Flamechart Panel
//
//  hyper    ██████████████████████░  12.4s
//  tokio    ██████████████░░░░░░░░░   9.8s
//  sqlx  ⚡ ████████████░░░░░░░░░░░ SLOW 7.4s
// ─────────────────────────────────────────────────────
inline Element FlamechartPanel(const std::vector<FlameEntry>& entries,
                                const std::string& tool_label = "cargo",
                                float total_s = 0.f) {
    if (entries.empty()) return text("  No timing data") | color(Theme::TextDim);

    float max_dur = 0.f;
    for (const auto& e : entries) max_dur = std::max(max_dur, e.duration_s);
    if (max_dur <= 0.f) max_dur = 1.f;

    float computed_total = total_s > 0.f ? total_s : max_dur;

    Elements rows;
    for (int i = 0; i < (int)entries.size(); ++i) {
        const auto& e   = entries[i];
        auto c          = FlameColor(i, e.is_slow);
        float fill      = e.duration_s / max_dur;

        std::ostringstream time_ss;
        time_ss << std::fixed << std::setprecision(1) << e.duration_s << "s";
        std::string time_str = time_ss.str();

        // Label column (right-aligned, fixed width)
        auto label_el = hbox(Theme::make_elements(
            text(e.is_slow ? "⚡ " : "   ") | color(Theme::Gold),
            text(e.name) | color(e.is_slow ? Theme::Gold : Theme::TextSub)
        )) | size(WIDTH, EQUAL, 14);

        // Bar — use gauge() inside a fixed-height box
        auto bar = hbox(Theme::make_elements(
            gauge(fill) | color(c) | flex
        )) | size(HEIGHT, EQUAL, 1);

        // Time column
        auto time_el = hbox(Theme::make_elements(
            text("  "),
            text(time_str) | color(e.is_slow ? Theme::Gold : Theme::TextDim),
            text(e.is_slow ? " !" : "  ") | color(Theme::Gold)
        )) | size(WIDTH, EQUAL, 8);

        rows.push_back(hbox(Theme::make_elements( label_el, text(" "), bar | flex, time_el )));
    }

    std::ostringstream total_ss;
    total_ss << std::fixed << std::setprecision(1) << computed_total;

    auto header = hbox(Theme::make_elements(
        text(" ≋ ") | color(Theme::Mint),
        text("Compile Flamechart") | color(Theme::Mint) | bold,
        filler(),
        text(tool_label + " · " + total_ss.str() + "s total ") | color(Theme::TextDim)
    ));

    return window(header, vbox(rows) | flex)
        
        | color(Theme::BorderHi);
}

// ═════════════════════════════════════════════════════
//  STAT TILES
//  A row of small metric cards
// ═════════════════════════════════════════════════════
struct StatTile {
    std::string label;    // "Builds Today"
    std::string value;    // "14"
    std::string sub;      // "↑ 3 from yesterday"
    Theme::Color color = Theme::Sky;
};

inline Element StatCard(const StatTile& s) {
    return vbox(Theme::make_elements(
        text(" " + s.label) | color(Theme::TextDim),
        text(" " + s.value) | color(s.color) | bold | size(HEIGHT, EQUAL, 1),
        text(" " + s.sub)   | color(Theme::TextDim)
    ))  | color(Theme::BorderHi) | bgcolor(Theme::Surface) | flex;
}

inline Element StatRow(const std::vector<StatTile>& tiles) {
    Elements cards;
    for (int i = 0; i < (int)tiles.size(); ++i) {
        cards.push_back(StatCard(tiles[i]));
        if (i + 1 < (int)tiles.size()) cards.push_back(text(" "));
    }
    return hbox(cards);
}

// ═════════════════════════════════════════════════════
//  LOG VIEW
//  A scrollable-friendly vbox of log lines
// ═════════════════════════════════════════════════════
enum class LogLevel { Info, Ok, Warning, Error, Dim };

struct LogLine {
    std::string timestamp;  // "14:22:04"
    std::string prefix;     // "   Compiling" / "    Finished" / "error[E0382]"
    std::string body;       // rest of line
    LogLevel    level = LogLevel::Info;
};

inline Theme::Color LogColor(LogLevel l) {
    switch (l) {
        case LogLevel::Info:    return Theme::Sky;
        case LogLevel::Ok:      return Theme::Sage;
        case LogLevel::Warning: return Theme::Gold;
        case LogLevel::Error:   return Theme::Rose;
        case LogLevel::Dim:     return Theme::TextDim;
    }
    return Theme::TextSub;
}

inline Element LogLineEl(const LogLine& l) {
    return hbox(Theme::make_elements(
        text(l.timestamp + "  ")  | color(Theme::TextDim),
        text(l.prefix)            | color(LogColor(l.level)) | bold,
        text("  " + l.body)       | color(Theme::TextSub)
    ));
}

inline Element LogPanel(const std::vector<LogLine>& lines,
                         int                         max_lines = 8) {
    Elements rows;
    // Show only the last max_lines entries (auto-scroll behaviour)
    int start = std::max(0, (int)lines.size() - max_lines);
    for (int i = start; i < (int)lines.size(); ++i) {
        rows.push_back(LogLineEl(lines[i]));
    }
    if (rows.empty()) {
        rows.push_back(text("  Waiting for output…") | color(Theme::TextDim));
    }

    auto header = hbox(Theme::make_elements(
        text(" ⫶ ") | color(Theme::TextSub),
        text("Live Output") | color(Theme::TextSub) | bold,
        filler(),
        Theme::keyhint("S", "scroll lock"),
        text(" ")
    ));

    return window(header, vbox(rows) | flex)
        
        | color(Theme::BorderHi);
}

} // namespace UI
