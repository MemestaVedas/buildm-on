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

// ─────────────────────────────────────────────────────
//  Renders a quick-launch pill chip
// ─────────────────────────────────────────────────────
inline Element QuickLaunchChip(const std::string& cmd, bool focused) {
    auto c = focused ? Theme::Lavender : Theme::TextDim;
    return hbox(Elements{
        text(" " + cmd + " ") | color(c),
    })  | color(c);
}

// ─────────────────────────────────────────────────────
//  Last run status tile
// ─────────────────────────────────────────────────────
struct LastRunInfo {
    bool        has_result = false;
    bool        success    = false;
    std::string duration;   // "38.2s"
    int         error_count = 0;
};

inline Element LastRunTile(const LastRunInfo& r) {
    if (!r.has_result) {
        return hbox(Elements{
            text("  No recent run") | color(Theme::TextDim),
        })  | color(Theme::BorderHi) | bgcolor(Theme::Surface);
    }

    auto status_col = vbox(Elements{
        text(" Status") | color(Theme::TextDim),
        text(r.success ? " ✓ success" : " ✕ failed")
            | color(r.success ? Theme::Sage : Theme::Rose) | bold,
    });

    auto duration_col = vbox(Elements{
        text("Duration") | color(Theme::TextDim),
        text(r.duration) | color(Theme::Sky) | bold,
    });

    auto errors_col = vbox(Elements{
        text("Errors") | color(Theme::TextDim),
        text(std::to_string(r.error_count))
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
    })  | color(Theme::BorderHi) | bgcolor(Theme::Surface);
}

// ─────────────────────────────────────────────────────
//  Launcher form — renders the left-side input pane
//  Actual interactivity is wired with FTXUI Components
//  in the app. This renders the *display* of the state.
// ─────────────────────────────────────────────────────
struct LauncherState {
    std::string  directory;       // current directory input
    std::string  command;         // current command input
    bool         dir_focused   = false;
    bool         cmd_focused   = false;
    bool         running       = false;
    int          quick_focused = -1;   // which quick-launch is highlighted
    LastRunInfo  last_run;
    std::vector<LogLine> output_log;
};

inline Element InputField(const std::string& prompt_icon,
                           const std::string& value,
                           bool               focused,
                           Theme::Color       accent = Theme::Lavender) {
    auto cursor = focused
        ? text("█") | color(accent) | blink
        : text(" ") | color(Theme::TextDim);

    return hbox(Elements{
        text(" " + prompt_icon + " ") | color(accent),
        text(value.empty() && !focused ? "…" : value)
            | color(focused ? Theme::Text : Theme::TextSub),
        cursor,
        text(" "),
    }) 
       | color(focused ? accent : Theme::BorderHi)
       | bgcolor(focused ? Theme::FocusedBG : Theme::BG);
}

inline Element LauncherForm(const LauncherState& ls) {
    // Directory input
    auto dir_section = vbox(Elements{
        text(" DIRECTORY") | color(Theme::TextDim),
        InputField("📁", ls.directory, ls.dir_focused, Theme::Lavender),
    });

    // Command input
    auto cmd_section = vbox(Elements{
        text(""),
        text(" COMMAND") | color(Theme::TextDim),
        InputField("$", ls.command, ls.cmd_focused, Theme::Peach),
    });

    // Quick-launch chips
    Elements chips;
    chips.push_back(text(""));
    chips.push_back(text(" QUICK LAUNCH") | color(Theme::TextDim));
    chips.push_back(text(""));
    Elements chip_row;
    for (int i = 0; i < (int)kQuickLaunch.size(); ++i) {
        chip_row.push_back(QuickLaunchChip(kQuickLaunch[i], i == ls.quick_focused));
        chip_row.push_back(text(" "));
        if ((i + 1) % 3 == 0) {
            chips.push_back(hbox(chip_row));
            chip_row.clear();
            chips.push_back(text(""));
        }
    }
    if (!chip_row.empty()) chips.push_back(hbox(chip_row));

    // Run / Clear buttons
    auto run_btn = hbox(Elements{
        text(ls.running ? "  ■ Kill Build  " : "  ⏎ Run Build  ")
            | color(ls.running ? Theme::Rose : Theme::Sky) | bold,
    })  | color(ls.running ? Theme::Rose : Theme::Sky) | flex;

    auto clear_btn = hbox(Elements{
        text("  ✕ Clear  ") | color(Theme::TextDim),
    })  | color(Theme::BorderHi);

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

    auto header = hbox(Theme::make_elements(
        text(" ⌁ ") | color(Theme::Peach),
        text("Launch New Build") | color(Theme::Peach) | bold
    ));

    return window(header, form_body | flex)
        | color(Theme::BorderHi);
}

// ─────────────────────────────────────────────────────
//  Full Launcher layout (left: form, right: output)
// ─────────────────────────────────────────────────────
inline Element LauncherView(const LauncherState& ls, int active_tab) {
    static const std::vector<std::pair<std::string,std::string>> kHints = {
        {"Shift+Tab", "next field"}, {"Enter", "run"}, {"Ctrl+C", "kill"}, {"Esc", "back"},
    };

    auto left  = LauncherForm(ls) | size(WIDTH, EQUAL, 44);
    auto right = LogPanel(ls.output_log, 20) | flex;

    Elements content_el;
    content_el.push_back(left);
    content_el.push_back(text(" "));
    content_el.push_back(right);
    auto content = hbox(std::move(content_el)) | flex | bgcolor(Theme::BG);

    Elements final_el;
    final_el.push_back(StatusBar(ls.last_run.has_result
            ? SystemStats{}   // pass in real stats from app
            : SystemStats{}));
    final_el.push_back(TabBar(active_tab));
    final_el.push_back(Theme::Rule());
    final_el.push_back(content);
    final_el.push_back(BottomBar(kHints));
    return vbox(std::move(final_el)) | bgcolor(Theme::BG);
}

} // namespace UI
