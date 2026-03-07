#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ftxui/dom/elements.hpp>
#include "theme.hpp"

// ╭──────────────────────────────────────────────────╮
// │   Build Cards — active build list panel          │
// ╰──────────────────────────────────────────────────╯

namespace UI {

using namespace ftxui;

// ─────────────────────────────────────────────────────
//  Data model for a single build
// ─────────────────────────────────────────────────────
enum class BuildStatus { Running, Success, Failed, Pending, Cancelled };

struct BuildEntry {
    int         pid         = 0;
    std::string tool;          // "cargo", "npm", "bazel", etc.
    std::string icon;          // emoji: 🦀 📦 🔧
    std::string command;       // full command string
    std::string directory;     // working dir
    BuildStatus status        = BuildStatus::Pending;
    float       progress      = 0.f;   // 0.0 → 1.0
    int         elapsed_secs  = 0;
    int         error_count   = 0;
    int         warn_count    = 0;
    std::string detail;        // e.g. "47 crates" or "bundling"
};

// ─────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────
inline std::string FormatDuration(int secs) {
    int m = secs / 60, s = secs % 60;
    std::ostringstream ss;
    ss << m << ":" << std::setw(2) << std::setfill('0') << s;
    return ss.str();
}

inline std::pair<std::string, Theme::Color> StatusLabel(BuildStatus st) {
    switch (st) {
        case BuildStatus::Running:   return { "● RUNNING",   Theme::Sky      };
        case BuildStatus::Success:   return { "✓ SUCCESS",   Theme::Sage     };
        case BuildStatus::Failed:    return { "✕ FAILED",    Theme::Rose     };
        case BuildStatus::Pending:   return { "◌ QUEUED",    Theme::Gold     };
        case BuildStatus::Cancelled: return { "— CANCELLED", Theme::TextDim  };
    }
    return { "?", Theme::TextDim };
}

inline Theme::Color AccentColor(BuildStatus st) {
    switch (st) {
        case BuildStatus::Running:   return Theme::Sky;
        case BuildStatus::Success:   return Theme::Sage;
        case BuildStatus::Failed:    return Theme::Rose;
        case BuildStatus::Pending:   return Theme::Gold;
        case BuildStatus::Cancelled: return Theme::TextDim;
    }
    return Theme::TextDim;
}

// ─────────────────────────────────────────────────────
//  Single build card
//
//  ┃  🦀 cargo build           ● RUNNING
//     ~/projects/myapp · release mode
//     ████████████░░░░░░░  63%
//     47 crates                    0:42
// ─────────────────────────────────────────────────────
inline Element BuildCard(const BuildEntry& b, bool selected = false) {
    auto [status_label, accent] = StatusLabel(b.status);
    auto strip_color = AccentColor(b.status);

    // ── Top row: tool name + status badge
    auto top_row = hbox(Theme::make_elements(
        text(b.icon + " ")  | color(accent) | bold,
        text(b.tool + " ")  | color(Theme::Text) | bold,
        filler(),
        hbox(Theme::make_elements(
            text(" " + status_label + " ") | color(accent) | bold
        )) | borderRounded | color(accent),
        text(" ")
    ));

    // ── Command + dir row
    auto cmd_row = hbox(Theme::make_elements(
        text(b.directory + "  ") | color(Theme::TextDim),
        text(b.command)          | color(Theme::TextSub)
    )) | size(HEIGHT, EQUAL, 1);

    // ── Progress bar (only for running/pending)
    Element progress_el = text("");
    if (b.status == BuildStatus::Running || b.status == BuildStatus::Pending) {
        progress_el = vbox(Theme::make_elements(
            Theme::ProgressBar(b.progress, strip_color)
        ));
    }

    // ── Meta row: detail + duration
    auto meta_row = hbox(Theme::make_elements(
        text(b.detail)                            | color(Theme::TextDim),
        text(b.error_count > 0
            ? "  ⚡ " + std::to_string(b.error_count) + " err"
            : "")                                 | color(Theme::Rose),
        filler(),
        text(FormatDuration(b.elapsed_secs) + " ")| color(Theme::TextDim)
    ));

    // ── Assemble card body
    auto body = vbox(Theme::make_elements(
        top_row,
        cmd_row,
        progress_el,
        meta_row
    ));

    // ── Left accent strip + body side by side
    auto card_inner = hbox(Theme::make_elements(
        Theme::AccentStrip(strip_color),
        text(" "),
        body | flex
    ));

    // ── Card container with border
    auto card = card_inner
        | borderRounded
        | color(selected ? strip_color : Theme::BorderHi)
        | bgcolor(Theme::Surface);

    return card;
}

// ─────────────────────────────────────────────────────
//  Active Builds Panel
//  Shows N build cards stacked, wrapped in RoundedPanel
// ─────────────────────────────────────────────────────
inline Element ActiveBuildsPanel(const std::vector<BuildEntry>& builds,
                                  int selected_idx = -1) {
    int running_count = 0;
    for (const auto& b : builds)
        if (b.status == BuildStatus::Running) ++running_count;

    Elements cards;
    for (int i = 0; i < (int)builds.size(); ++i) {
        cards.push_back(BuildCard(builds[i], i == selected_idx));
        if (i + 1 < (int)builds.size())
            cards.push_back(text(""));  // small gap
    }

    if (builds.empty()) {
        cards.push_back(
            text("  No active builds. Press [2] to launch one.") | color(Theme::TextDim)
        );
    }

    auto header_right = running_count > 0
        ? text(std::to_string(running_count) + " running ") | color(Theme::SkyDim)
        : text("idle ") | color(Theme::TextDim);

    // Manual header since we need right-aligned count
    auto header = hbox(Theme::make_elements(
        text(" ⬡ ") | color(Theme::Sky),
        text("Active Builds") | color(Theme::Sky) | bold,
        filler(),
        header_right
    ));

    return window(header, vbox(cards) | flex)
        | borderRounded
        | color(Theme::BorderHi);
}

} // namespace UI
