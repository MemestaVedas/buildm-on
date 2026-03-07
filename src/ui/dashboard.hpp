#pragma once
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include "theme.hpp"
#include "statusbar.hpp"
#include "buildcard.hpp"
#include "errorpanel.hpp"
#include "widgets.hpp"

// ╭──────────────────────────────────────────────────╮
// │   Dashboard Tab — main layout assembly           │
// ╰──────────────────────────────────────────────────╯

namespace UI {

using namespace ftxui;

// ─────────────────────────────────────────────────────
//  State passed in from the app per render frame
// ─────────────────────────────────────────────────────
struct DashboardState {
    SystemStats              stats;
    std::vector<BuildEntry>  builds;
    std::vector<ErrorEntry>  errors;
    std::vector<FlameEntry>  flame;
    std::vector<LogLine>     logs;
    std::vector<StatTile>    stat_tiles;
    int                      selected_build = -1;
    float                    flame_total_s  = 0.f;
    std::string              flame_tool     = "cargo";
};

// ─────────────────────────────────────────────────────
//  Render the Dashboard tab content (no chrome)
//
//  Layout (two-column hbox):
//
//  ┌──────────────────────┬────────────────────────────┐
//  │  Active Builds  (45) │  Flamechart           (flex)│
//  │                      ├────────────────────────────┤
//  │                      │  Log Output           (flex)│
//  ├──────────────────────┼────────────────────────────┤
//  │  Error Analysis (45) │  Stat  Stat  Stat          │
//  └──────────────────────┴────────────────────────────╯
// ─────────────────────────────────────────────────────
inline Element DashboardView(const DashboardState& s) {
    // Left column: fixed 45 chars
    Elements left_el;
    left_el.push_back(ActiveBuildsPanel(s.builds, s.selected_build) | flex);
    left_el.push_back(emptyElement() | size(HEIGHT, EQUAL, 1));
    left_el.push_back(ErrorPanel(s.errors) | flex);
    auto left_col = vbox(std::move(left_el)) | size(WIDTH, EQUAL, 45);

    // Right column: flex
    Elements right_el;
    right_el.push_back(FlamechartPanel(s.flame, s.flame_tool, s.flame_total_s) | flex);
    right_el.push_back(emptyElement() | size(HEIGHT, EQUAL, 1));
    right_el.push_back(LogPanel(s.logs) | flex);
    right_el.push_back(emptyElement() | size(HEIGHT, EQUAL, 1));
    right_el.push_back(StatRow(s.stat_tiles));
    auto right_col = vbox(std::move(right_el)) | flex;

    return hbox(Elements{
        left_col,
        text(" "),
        right_col,
    }) | flex;
}

// ─────────────────────────────────────────────────────
//  Full Dashboard screen (chrome + content)
//  StatusBar / TabBar / Rule / DashboardView / BottomBar
// ─────────────────────────────────────────────────────
inline Element DashboardScreen(const DashboardState& s, int active_tab) {
    static const std::vector<std::pair<std::string,std::string>> kHints = {
        {"1-5", "tabs"}, {"e", "errors"}, {"c", "copy errors"},
        {"f", "flamechart"}, {"k", "kill build"}, {"q", "quit"},
    };

    return vbox(Elements{
        StatusBar(s.stats),
        TabBar(active_tab),
        Theme::Rule(),
        DashboardView(s) | flex | bgcolor(Theme::BG),
        BottomBar(kHints),
    }) | bgcolor(Theme::BG);
}

} // namespace UI
