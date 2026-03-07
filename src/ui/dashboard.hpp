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
};

// ─────────────────────────────────────────────────────
//  Render the Dashboard tab content (no chrome)
//
//  Layout:
//
//  ┌──────────────────┬──────────────────────────────┐
//  │  Active Builds   │  Flamechart                  │
//  │                  ├──────────────────────────────┤
//  │                  │  Log Output                  │
//  ├──────────────────┼──────────────────────────────┤
//  │  Error Analysis  │  [Stat] [Stat] [Stat]        │
//  └──────────────────┴──────────────────────────────╯
// ─────────────────────────────────────────────────────
inline Element DashboardView(const DashboardState& s) {
    // Left column
    Elements left_el;
    left_el.push_back(ActiveBuildsPanel(s.builds, s.selected_build) | flex);
    left_el.push_back(emptyElement() | size(HEIGHT, EQUAL, 1));
    left_el.push_back(ErrorPanel(s.errors) | flex);
    auto left_col = vbox(std::move(left_el)) | flex;

    // Right column
    Elements right_el;
    right_el.push_back(FlamechartPanel(s.flame, "cargo", s.flame_total_s) | flex);
    right_el.push_back(emptyElement() | size(HEIGHT, EQUAL, 1));
    right_el.push_back(LogPanel(s.logs) | flex);
    right_el.push_back(emptyElement() | size(HEIGHT, EQUAL, 1));
    right_el.push_back(StatRow(s.stat_tiles));
    auto right_col = vbox(std::move(right_el)) | flex;

    Elements main_el;
    main_el.push_back(left_col | size(WIDTH, EQUAL, 45));
    main_el.push_back(text(" "));
    main_el.push_back(right_col | flex);
    return hbox(std::move(main_el)) | flex;
}

// ─────────────────────────────────────────────────────
//  Full Dashboard screen (chrome + content)
// ─────────────────────────────────────────────────────
inline Element DashboardScreen(const DashboardState& s, int active_tab) {
    static const std::vector<std::pair<std::string,std::string>> kHints = {
        {"1-5", "tabs"}, {"e", "errors"}, {"c", "copy errors"},
        {"f", "flamechart"}, {"k", "kill build"}, {"q", "quit"},
    };

    Elements screen_el;
    screen_el.push_back(StatusBar(s.stats));
    screen_el.push_back(TabBar(active_tab));
    screen_el.push_back(Theme::Rule());
    screen_el.push_back(DashboardView(s) | flex | bgcolor(Theme::BG));
    screen_el.push_back(BottomBar(kHints));
    return vbox(std::move(screen_el)) | bgcolor(Theme::BG);
}

} // namespace UI
