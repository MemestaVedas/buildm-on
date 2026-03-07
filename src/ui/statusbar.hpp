#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include "theme.hpp"

// ╭──────────────────────────────────────────────────╮
// │   Status Bar · Tab Bar · Bottom Bar              │
// ╰──────────────────────────────────────────────────╯

namespace UI {

using namespace ftxui;

// ─────────────────────────────────────────────────────
//  STATUS BAR  (top row)
//
//  ● Mobile: Active  CPU 34%  RAM 6.2GB  ↑120KB ↓840KB
//                                              Sat 14:22 ⚡3 errors
// ─────────────────────────────────────────────────────
struct SystemStats {
    bool        mobile_active  = false;
    int         cpu_percent    = 0;
    float       ram_gb         = 0.f;
    float       net_up_kb      = 0.f;
    float       net_down_kb    = 0.f;
    int         error_count    = 0;
    std::string time_str;        // e.g. "Sat Mar 07 · 14:22:09"
};

// Status bar badge — borderRounded pill with matching text+border color
inline Element StatusBadge(const std::string& label, Theme::Color c) {
    return hbox(Elements{
        text(" "),
        text(label) | color(c) | bold,
        text(" "),
    }) | borderRounded | color(c);
}

inline Element StatusBar(const SystemStats& stats) {
    // Left side badges
    Elements left;

    // Mobile badge
    if (stats.mobile_active) {
        left.push_back(StatusBadge("● Mobile: Active", Theme::Sky));
    } else {
        left.push_back(StatusBadge("○ Mobile: Off",    Theme::TextDim));
    }
    left.push_back(text("  "));
    left.push_back(StatusBadge("CPU " + std::to_string(stats.cpu_percent) + "%", Theme::Sage));
    left.push_back(text(" "));
    left.push_back(StatusBadge([&]{
        std::ostringstream ss; ss << std::fixed << std::setprecision(1) << stats.ram_gb << " GB";
        return ss.str();
    }(), Theme::Lavender));
    left.push_back(text(" "));
    left.push_back(StatusBadge([&]{
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(0)
           << "↑" << stats.net_up_kb << "KB  ↓" << stats.net_down_kb << "KB";
        return ss.str();
    }(), Theme::Gold));

    // Right side
    Elements right;
    right.push_back(text(stats.time_str) | color(Theme::TextDim));
    right.push_back(text("  "));
    if (stats.error_count > 0) {
        right.push_back(StatusBadge(
            "⚡ " + std::to_string(stats.error_count) + " errors",
            Theme::Rose
        ));
    }

    return hbox(Elements{
        hbox(left),
        filler(),
        hbox(right),
    }) | bgcolor(Theme::BG2) | color(Theme::Text) | size(HEIGHT, EQUAL, 3) | center;
}

// ─────────────────────────────────────────────────────
//  TAB BAR
//  Active tab: Sky color + borderRounded
//  Inactive:   TextSub color, no border
// ─────────────────────────────────────────────────────
struct TabDef {
    std::string icon;
    std::string label;
    std::string key;
};

inline const std::vector<TabDef> kTabs = {
    { "◈", "Dashboard", "1" },
    { "⌁", "Launcher",  "2" },
    { "◷", "History",   "3" },
    { "⟡", "Plugins",   "4" },
    { "?", "Help",      "5" },
};

inline Element TabBar(int active_tab) {
    Elements tabs;
    tabs.push_back(text(" "));  // left padding

    for (int i = 0; i < (int)kTabs.size(); ++i) {
        const auto& t = kTabs[i];
        bool is_active = (i == active_tab);

        auto label_el = hbox(Elements{
            text(" "),
            text(t.icon + " ")   | color(is_active ? Theme::Sky : Theme::TextSub),
            text(t.label)        | color(is_active ? Theme::Sky : Theme::TextSub)
                                 | bold,
            text(" "),
            text(t.key)          | color(Theme::TextDim),
            text(" "),
        });

        if (is_active) {
            tabs.push_back(label_el | borderRounded | color(Theme::Sky));
        } else {
            tabs.push_back(label_el | color(Theme::TextSub));
        }
        tabs.push_back(text(" "));
    }

    return hbox(tabs) | bgcolor(Theme::BG2) | size(HEIGHT, EQUAL, 3);
}

// ─────────────────────────────────────────────────────
//  BOTTOM HINT BAR
//  Shows keybinds contextually per tab
//  version string right-aligned in TextDim
// ─────────────────────────────────────────────────────
inline Element BottomBar(const std::vector<std::pair<std::string,std::string>>& hints,
                         const std::string& version = "buildm-on v2.0.0 · linux · aurora") {
    Elements elems;
    elems.push_back(text(" "));
    for (const auto& [key, desc] : hints) {
        elems.push_back(Theme::keyhint(key, desc));
        elems.push_back(text("  "));
    }

    return hbox(Elements{
        hbox(elems),
        filler(),
        text(version + " ") | color(Theme::TextDim),
    }) | bgcolor(Theme::BG2) | color(Theme::TextSub) | size(HEIGHT, EQUAL, 1);
}

} // namespace UI
