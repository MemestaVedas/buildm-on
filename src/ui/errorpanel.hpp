#pragma once
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include "theme.hpp"

// ╭──────────────────────────────────────────────────╮
// │   Error Analysis Panel                           │
// │   Smart error parser display + diff badges       │
// ╰──────────────────────────────────────────────────╯

namespace UI {

using namespace ftxui;

// ─────────────────────────────────────────────────────
//  Data model
// ─────────────────────────────────────────────────────
enum class ErrorSeverity { Error, Warning, Hint, Note };
enum class ErrorDiffState { New, Persisting, Resolved };

struct ErrorEntry {
    ErrorSeverity  severity   = ErrorSeverity::Error;
    ErrorDiffState diff_state = ErrorDiffState::Persisting;
    std::string    file;          // "src/main.rs"
    int            line    = 0;
    int            col     = 0;
    std::string    code;          // "E0382"
    std::string    message;       // "borrow of moved value: `config`"
    std::string    snippet_line;  // the offending source line
    std::string    pointer;       // "       ^^^^^^ value used after move"
    std::string    tool;          // "rustc", "tsc", etc.
};

// ─────────────────────────────────────────────────────
//  Severity color + icon
// ─────────────────────────────────────────────────────
inline std::pair<std::string,Theme::Color> SeverityStyle(ErrorSeverity s) {
    switch (s) {
        case ErrorSeverity::Error:   return { "✕ error",   Theme::Rose     };
        case ErrorSeverity::Warning: return { "⚠ warning", Theme::Gold     };
        case ErrorSeverity::Hint:    return { "→ hint",    Theme::Sky      };
        case ErrorSeverity::Note:    return { "• note",    Theme::TextSub  };
    }
    return { "?", Theme::TextDim };
}

// ─────────────────────────────────────────────────────
//  Diff state badge: NEW / RESOLVED / (nothing)
// ─────────────────────────────────────────────────────
inline Element DiffBadge(ErrorDiffState state) {
    switch (state) {
        case ErrorDiffState::New:
            return hbox(Elements{
                text(" NEW ") | color(Theme::Gold) | bold,
            }) | borderRounded | color(Theme::Gold);

        case ErrorDiffState::Resolved:
            return hbox(Theme::make_elements(
                text(" FIXED ") | color(Theme::Sage) | bold
            )) | borderRounded | color(Theme::Sage);

        case ErrorDiffState::Persisting:
        default:
            return text("");
    }
}

// ─────────────────────────────────────────────────────
//  Single error entry element
//
//  ┃  ✕ error[E0382]  src/main.rs:48:12       NEW ✦
//     borrow of moved value: `config`
//     48 │  let x = config.build();
//              ^^^^^^ value used after move
// ─────────────────────────────────────────────────────
inline Element ErrorCard(const ErrorEntry& e, bool compact = false) {
    auto [sev_label, sev_color] = SeverityStyle(e.severity);

    // Location string: "src/main.rs:48:12"
    std::string location = e.file + ":" + std::to_string(e.line) + ":" + std::to_string(e.col);

    // Top row
    auto top = hbox(Theme::make_elements(
        text(" ▸ ")         | color(sev_color),
        text(sev_label)     | color(sev_color) | bold,
        text(e.code.empty() ? "" : "[" + e.code + "]") | color(sev_color),
        text("  "),
        text(location)      | color(Theme::TextSub),
        filler(),
        DiffBadge(e.diff_state),
        text(" ")
    ));

    // Message row
    auto msg = hbox(Theme::make_elements(
        text("   "),
        text(e.message) | color(Theme::Text)
    ));

    Element card;
    if (!compact && !e.snippet_line.empty()) {
        // Code snippet box
        auto snippet = vbox(Theme::make_elements(
            hbox(Theme::make_elements(
                text("   "),
                text(std::to_string(e.line) + " │ ") | color(Theme::TextDim),
                text(e.snippet_line)                  | color(Theme::TextSub)
            )),
            hbox(Theme::make_elements(
                text("     "),
                text(e.pointer) | color(sev_color)
            ))
        )) | bgcolor(Theme::BG) | borderEmpty;

        card = vbox(Theme::make_elements( top, msg, snippet ));
    } else {
        card = vbox(Theme::make_elements( top, msg ));
    }

    // Left border strip in severity color
    return hbox(Theme::make_elements(
        Theme::AccentStrip(sev_color),
        card | flex
    )) | bgcolor(Theme::Surface) | borderRounded | color(Theme::BorderHi);
}

// ─────────────────────────────────────────────────────
//  Error Analysis Panel
//  Header shows: "3 active · 1 new · 2 resolved"
// ─────────────────────────────────────────────────────
inline Element ErrorPanel(const std::vector<ErrorEntry>& errors,
                           int                           max_visible = 4) {
    int total    = errors.size();
    int new_cnt  = 0, fixed_cnt = 0, warn_cnt = 0;
    for (const auto& e : errors) {
        if (e.diff_state == ErrorDiffState::New)      ++new_cnt;
        if (e.diff_state == ErrorDiffState::Resolved)  ++fixed_cnt;
        if (e.severity   == ErrorSeverity::Warning)    ++warn_cnt;
    }

    // Header summary
    auto header = hbox(Theme::make_elements(
        text(" ✕ ")            | color(Theme::Rose),
        text("Error Analysis") | color(Theme::Rose) | bold,
        filler(),
        text(std::to_string(total) + " active")   | color(Theme::TextDim),
        text("  ·  "),
        text("+" + std::to_string(new_cnt) + " new")  | color(Theme::Gold) | bold,
        text("  ·  "),
        text("-" + std::to_string(fixed_cnt) + " fixed") | color(Theme::Sage),
        text(" ")
    ));

    Elements cards;
    int shown = 0;
    for (const auto& e : errors) {
        if (e.diff_state == ErrorDiffState::Resolved) continue; // resolved shown compact at bottom
        if (shown >= max_visible) break;
        cards.push_back(ErrorCard(e, shown >= 2));  // compact after 2nd
        cards.push_back(text(""));
        ++shown;
    }

    // Show resolved compactly if any
    for (const auto& e : errors) {
        if (e.diff_state == ErrorDiffState::Resolved) {
            cards.push_back(ErrorCard(e, true));
            cards.push_back(text(""));
        }
    }

    if (errors.empty()) {
        cards.push_back(text("  ✓ No errors") | color(Theme::Sage));
    }

    return window(header, vbox(cards) | flex)
        | borderRounded
        | color(Theme::BorderHi);
}

} // namespace UI
