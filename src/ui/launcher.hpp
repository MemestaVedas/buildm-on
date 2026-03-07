#pragma once
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include "theme.hpp"
#include "statusbar.hpp"
#include "widgets.hpp"

// ╭──────────────────────────────────────────────────╮
// │   Launcher Tab — interactive build runner        │
// ╰──────────────────────────────────────────────────╯

namespace UI {

using namespace ftxui;

// ─────────────────────────────────────────────────────
//  Quick-launch preset commands shown as pill buttons
// ─────────────────────────────────────────────────────
static const std::vector<std::string> kQuickLaunch = {
    "cargo build",
    "cargo build --release",
    "npm run build",
    "yarn build",
    "make all",
    "bazel build //...",
    "cmake --build .",
};

// Quick-launch chip: borderRounded, TextDim by default, Lavender on hover/focus
inline Element QuickLaunchChip(const std::string& cmd, bool focused) {
    auto c = focused ? Theme::Lavender : Theme::TextDim;
    return hbox(Elements{
        text(" " + cmd + " ") | color(c),
    }) | borderRounded | color(c);
}

// ─────────────────────────────────────────────────────
//  Last run status tile
// ─────────────────────────────────────────────────────
struct LastRunInfo {
    bool        has_result  = false;
    bool        success     = false;
    std::string duration;    // "38.2s"
    int         error_count = 0;
};

inline Element LastRunTile(const LastRunInfo& r) {
    if (!r.has_result) {
        return hbox(Elements{
            text("  No recent run") | color(Theme::TextDim),
        }) | borderRounded | color(Theme::BorderHi) | bgcolor(Theme::Surface);
    }

    auto status_col = vbox(Elements{
        text(" Status")  | color(Theme::TextDim),
        text(r.success ? " ✓ success" : " ✕ failed")
            | color(r.success ? Theme::Sage : Theme::Rose) | bold,
    });

    auto duration_col = vbox(Elements{
        text(" Duration") | color(Theme::TextDim),
        text(" " + r.duration) | color(Theme::Sky) | bold,
    });

    auto errors_col = vbox(Elements{
        text(" Errors") | color(Theme::TextDim),
        text(" " + std::to_string(r.error_count))
            | color(r.error_count > 0 ? Theme::Rose : Theme::Sage) | bold,
    });

    return hbox(Elements{
        text(" "),
        status_col,
        filler(),
        duration_col,
        filler(),
        errors_col,
        text(" "),
    }) | borderRounded | color(Theme::BorderHi) | bgcolor(Theme::Surface);
}

// ─────────────────────────────────────────────────────
//  Input field rendering (display state only)
//  Focused:   borderRounded Lavender/Peach + FocusedBG
//  Unfocused: borderRounded BorderHi + BG
//
//  The actual FTXUI Input component is overlaid on top
//  in the component tree; this just renders the chrome.
// ─────────────────────────────────────────────────────
struct LauncherState {
    std::string  directory;
    std::string  command;
    bool         dir_focused   = false;
    bool         cmd_focused   = false;
    bool         running       = false;
    int          quick_focused = -1;
    LastRunInfo  last_run;
    std::vector<LogLine> output_log;
};

// Styled input box that wraps the FTXUI-provided input element
inline Element StyledInput(const std::string& prompt_icon,
                            Element            input_el,
                            bool               focused,
                            Theme::Color       accent = Theme::Lavender) {
    return hbox(Elements{
        text(" " + prompt_icon + " ") | color(accent),
        input_el | flex,
        text(" "),
    }) | borderRounded
       | color(focused ? accent : Theme::BorderHi)
       | bgcolor(focused ? Theme::FocusedBG : Theme::BG);
}

// ─────────────────────────────────────────────────────
//  Launcher Form: renders the left pane display
//  Real Input components are wired in main.cpp and
//  passed as Elements here for compositing.
// ─────────────────────────────────────────────────────
inline Element LauncherFormDisplay(const LauncherState& ls,
                                    Element dir_input_el,
                                    Element cmd_input_el) {
    // Directory section
    auto dir_section = vbox(Elements{
        text(" DIRECTORY") | color(Theme::TextDim),
        StyledInput("📁", std::move(dir_input_el), ls.dir_focused, Theme::Lavender),
    });

    // Command section
    auto cmd_section = vbox(Elements{
        text(""),
        text(" COMMAND") | color(Theme::TextDim),
        StyledInput("$", std::move(cmd_input_el), ls.cmd_focused, Theme::Peach),
    });

    // Quick-launch chips — 2 per row (fits better in 44 chars)
    Elements chips;
    chips.push_back(text(""));
    chips.push_back(text(" QUICK LAUNCH") | color(Theme::TextDim));
    chips.push_back(text(""));
    Elements chip_row;
    for (int i = 0; i < (int)kQuickLaunch.size(); ++i) {
        chip_row.push_back(QuickLaunchChip(kQuickLaunch[i], i == ls.quick_focused) | flex);
        chip_row.push_back(text(" "));
        if ((i + 1) % 2 == 0) {
            chips.push_back(hbox(chip_row));
            chip_row.clear();
            chips.push_back(text(""));
        }
    }
    if (!chip_row.empty()) chips.push_back(hbox(chip_row));

    // Run button: flex width, borderRounded, Sky (or Rose when killing)
    auto run_btn = hbox(Elements{
        text(ls.running ? "  ■ Kill Build  " : "  ⏎ Run Build  ")
            | color(ls.running ? Theme::Rose : Theme::Sky) | bold,
    }) | borderRounded
       | color(ls.running ? Theme::Rose : Theme::Sky)
       | flex;

    // Clear button: fixed width, borderRounded, TextDim
    auto clear_btn = hbox(Elements{
        text("  ✕ Clear  ") | color(Theme::TextDim),
    }) | borderRounded | color(Theme::BorderHi);

    auto btn_row = hbox(Elements{ run_btn, text(" "), clear_btn });

    auto form_body = vbox(Theme::make_elements(
        dir_section,
        cmd_section,
        vbox(chips),
        text(""),
        btn_row,
        text(""),
        LastRunTile(ls.last_run)
    ));

    // Panel header: "⌁ Launch New Build" in Peach
    auto header = hbox(Theme::make_elements(
        text(" ⌁ ")              | color(Theme::Peach),
        text("Launch New Build") | color(Theme::Peach) | bold
    ));

    return window(header, form_body | flex)
        | borderRounded
        | color(Theme::BorderHi);
}

// ─────────────────────────────────────────────────────
//  Full Launcher layout (left: form, right: log output)
//  real_stats is used for the status bar chrome
// ─────────────────────────────────────────────────────
inline Element LauncherView(const LauncherState& ls,
                             int                  active_tab,
                             const SystemStats&   real_stats,
                             Element              dir_input_el,
                             Element              cmd_input_el) {
    static const std::vector<std::pair<std::string,std::string>> kHints = {
        {"Ctrl+N", "next field"}, {"Ctrl+P", "prev field"},
        {"Tab", "autocomplete"}, {"Enter", "run"}, {"Esc", "back"},
    };

    auto left  = LauncherFormDisplay(ls, std::move(dir_input_el), std::move(cmd_input_el))
                    | size(WIDTH, EQUAL, 44);
    auto right = LogPanel(ls.output_log, 20) | flex;

    auto content = hbox(Elements{
        left,
        text(" "),
        right,
    }) | flex | bgcolor(Theme::BG);

    return vbox(Elements{
        StatusBar(real_stats),
        TabBar(active_tab),
        Theme::Rule(),
        content,
        BottomBar(kHints),
    }) | bgcolor(Theme::BG);
}

} // namespace UI
