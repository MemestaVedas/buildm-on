#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>
#include "ws_server.hpp"
#include "os_utils.hpp"
#include "plugin_manager.hpp"
#include "github_actions.hpp"

#include "ui/theme.hpp"
#include "ui/statusbar.hpp"
#include "ui/buildcard.hpp"
#include "ui/errorpanel.hpp"
#include "ui/widgets.hpp"
#include "ui/dashboard.hpp"
#include "ui/launcher.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <map>
#include <algorithm>
#include <regex>
#include <cstdlib>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

// ─────────────────────────────────────────────
// Data Types & Persistence
// ─────────────────────────────────────────────

struct Config {
    std::string theme = "default";
    bool notifications = true;
    bool sound_enabled = true;
    std::string sound_success = "/usr/share/sounds/freedesktop/stereo/complete.oga";
    std::string sound_failure = "/usr/share/sounds/freedesktop/stereo/dialog-error.oga";
    int slow_build_threshold_s = 30;
};

void to_json(json& j, const BuildJob& b) {
    j = json{
        {"project", b.project}, {"tool", b.tool}, {"status", b.status},
        {"progress", b.progress}, {"pid", b.pid}, {"duration", b.duration_seconds},
        {"timestamp", (long long)b.timestamp},
        {"errors", b.errors}
    };
}

void from_json(const json& j, BuildJob& b) {
    j.at("project").get_to(b.project);
    j.at("tool").get_to(b.tool);
    j.at("status").get_to(b.status);
    j.at("progress").get_to(b.progress);
    j.at("pid").get_to(b.pid);
    j.at("duration").get_to(b.duration_seconds);
    if (j.contains("timestamp")) {
        b.timestamp = (std::time_t)j.at("timestamp").get<long long>();
    }
    if (j.contains("errors")) {
        try { b.errors = j.at("errors").get<std::vector<ParsedError>>(); } catch (...) {}
    }
}

// ─────────────────────────────────────────────
// WebSocket
// ─────────────────────────────────────────────

WsServer g_ws_server;
UdpBroadcaster g_discovery;

void broadcast_update(const std::vector<BuildJob>& jobs, float cpu, float ram) {
    json msg;
    msg["type"] = "update";
    msg["cpu"] = cpu;
    msg["ram"] = ram;
    msg["active_count"] = jobs.size();
    msg["builds"] = json::array();
    for (const auto& job : jobs) {
        json b;
        b["project"]  = job.project;
        b["tool"]     = job.tool;
        b["status"]   = job.status;
        b["progress"] = job.progress;
        b["pid"]      = job.pid;
        b["duration"] = job.duration_seconds;
        b["errors"]   = json::array();
        for (auto& e : job.errors) {
            b["errors"].push_back({
                {"file", e.file}, {"line", e.line},
                {"message", e.message}, {"severity", severity_str(e.severity)}
            });
        }
        msg["builds"].push_back(b);
    }
    g_ws_server.broadcast(msg.dump());
}

std::string get_history_path() {
    const char* home = getenv("HOME");
    return home ? std::string(home) + "/.buildm-on_history.json" : ".buildm-on_history.json";
}

void save_history(const std::vector<BuildJob>& history) {
    std::ofstream f(get_history_path());
    if (f.is_open()) {
        json j = history;
        f << j.dump(4);
    }
}

std::vector<BuildJob> load_history() {
    std::vector<BuildJob> history;
    std::ifstream f(get_history_path());
    if (f.is_open()) {
        try {
            json j; f >> j;
            history = j.get<std::vector<BuildJob>>();
        } catch (...) {}
    }
    return history;
}

Config load_config() {
    Config cfg;
#ifndef _WIN32
    const char* home = getenv("HOME");
    if (!home) return cfg;
    std::ifstream f(std::string(home) + "/.buildm-on.toml");
    std::string line;
    if (f.is_open()) {
        while (std::getline(f, line)) {
            if (line.find("notifications") != std::string::npos && line.find("false") != std::string::npos)
                cfg.notifications = false;
            if (line.find("sound") != std::string::npos && line.find("enabled") != std::string::npos && line.find("false") != std::string::npos)
                cfg.sound_enabled = false;
            if (line.find("slow_build_threshold") != std::string::npos && line.find("=") != std::string::npos) {
                try {
                    auto pos = line.find("=");
                    cfg.slow_build_threshold_s = std::stoi(line.substr(pos + 1));
                } catch (...) {}
            }
        }
    }
#endif
    return cfg;
}

// ─────────────────────────────────────────────
// Notifications & Sound
// ─────────────────────────────────────────────

void send_notification(const std::string& summary, const std::string& body) {
#ifndef _WIN32
    std::string cmd = "notify-send \"" + summary + "\" \"" + body + "\" &";
    auto res = system(cmd.c_str());
    (void)res;
#endif
}

void play_sound(const Config& cfg, bool success) {
#ifndef _WIN32
    if (!cfg.sound_enabled) return;
    const std::string& path = success ? cfg.sound_success : cfg.sound_failure;
    std::string cmd = "paplay \"" + path + "\" 2>/dev/null || aplay \"" + path + "\" 2>/dev/null &";
    auto res = system(cmd.c_str());
    (void)res;
#endif
}

void copy_to_clipboard(const std::string& text) {
#ifndef _WIN32
    FILE* p = popen("xclip -selection clipboard 2>/dev/null || xsel --clipboard --input 2>/dev/null", "w");
    if (p) {
        fwrite(text.c_str(), 1, text.size(), p);
        pclose(p);
    }
#endif
}

// ─────────────────────────────────────────────
// WebSocket
// ─────────────────────────────────────────────

// ─────────────────────────────────────────────
// UI Data Conversion Helpers
// ─────────────────────────────────────────────

UI::BuildStatus to_ui_status(const std::string& s) {
    if (s == "Building" || s == "Active") return UI::BuildStatus::Running;
    if (s == "Finished") return UI::BuildStatus::Success;
    if (s == "Failed") return UI::BuildStatus::Failed;
    if (s == "Queued") return UI::BuildStatus::Pending;
    return UI::BuildStatus::Cancelled;
}

UI::BuildEntry to_ui_build(const BuildJob& b) {
    UI::BuildEntry e;
    e.pid = b.pid;
    e.tool = b.tool;
    e.icon = (b.tool == "cargo" ? "🦀" : (b.tool == "npm" ? "📦" : "🔧"));
    e.command = b.project; // In existing app, project often holds the cmd or name
    e.directory = "";
    e.status = to_ui_status(b.status);
    e.progress = b.progress;
    e.elapsed_secs = b.duration_seconds;
    e.error_count = b.errors.size();
    e.detail = b.status;
    return e;
}

UI::ErrorSeverity to_ui_severity(ErrorSeverity s) {
    switch (s) {
        case ErrorSeverity::Error: return UI::ErrorSeverity::Error;
        case ErrorSeverity::Warning: return UI::ErrorSeverity::Warning;
        case ErrorSeverity::Hint: return UI::ErrorSeverity::Hint;
        case ErrorSeverity::Note: return UI::ErrorSeverity::Note;
    }
    return UI::ErrorSeverity::Error;
}

UI::ErrorEntry to_ui_error(const ParsedError& p) {
    UI::ErrorEntry e;
    e.severity = to_ui_severity(p.severity);
    e.diff_state = p.is_new ? UI::ErrorDiffState::New : UI::ErrorDiffState::Persisting;
    e.file = p.file;
    e.line = p.line;
    e.col = p.col;
    e.code = p.code;
    e.message = p.message;
    
    // Extract first line of context as snippet
    if (!p.context.empty()) {
        std::stringstream ss(p.context);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find(">>") == 0) {
                e.snippet_line = line.substr(2); // Remove ">>"
                break;
            }
        }
    }
    return e;
}

// ─────────────────────────────────────────────
// Renderer Helpers
// ─────────────────────────────────────────────

ftxui::Element render_progress_bar(float progress, int width = 22) {
    using namespace ftxui;
    int filled = static_cast<int>(progress * (float)width);
    std::string bar = "[";
    for (int i = 0; i < width; i++) bar += (i < filled) ? "█" : "░";
    bar += "] " + std::to_string((int)(progress * 100)) + "%";
    Color c = (progress >= 1.0f) ? Color::Green : (progress > 0.5f ? Color::Yellow : Color::Cyan);
    return text(bar) | color(c);
}

ftxui::Color severity_color(ErrorSeverity s) {
    switch (s) {
        case ErrorSeverity::Error:   return ftxui::Color::Red;
        case ErrorSeverity::Warning: return ftxui::Color::Yellow;
        case ErrorSeverity::Hint:    return ftxui::Color::Cyan;
        case ErrorSeverity::Note:    return ftxui::Color::GrayLight;
    }
    return ftxui::Color::White;
}

std::string severity_icon(ErrorSeverity s) {
    switch (s) {
        case ErrorSeverity::Error:   return "[E]";
        case ErrorSeverity::Warning: return "[W]";
        case ErrorSeverity::Hint:    return "[H]";
        case ErrorSeverity::Note:    return "[N]";
    }
    return "[?]";
}

// ─────────────────────────────────────────────
// App State
// ─────────────────────────────────────────────

struct AppState {
    std::vector<BuildJob>    jobs;
    std::vector<BuildJob>    history;
    std::vector<ParsedError> all_errors;
    std::vector<ParsedError> prev_errors;
    ErrorDiff                last_diff;
    std::mutex               mtx;
    float cpu = 0, ram = 0;
    NetStats net = {0, 0};
    int uptime_secs = 0;
    bool show_failure_overlay = false;
    std::string last_failed_project;
    Config cfg;
    bool clipboard_copied = false;
    std::string copy_feedback;
    PluginManager plugins;
    gh::ActionsPoller gh_poller;
    int active_error_idx = 0;

    // Tabs & UI state
    int active_tab = 0;

    // Inputs for Launcher
    std::string launcher_dir_input;
    std::string launcher_cmd_input;

    // Bridge structs for UI
    UI::DashboardState dash_state;
    UI::LauncherState  launcher_state;
};

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────

int main(int argc, char** argv) {
    using namespace ftxui;

    // ── CLI flags ────────────────────────────────
    bool no_tui = false;
    std::string daemon_cmd;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--no-tui" || arg == "--daemon") no_tui = true;
        if ((arg == "--run" || arg == "-r") && i + 1 < argc) daemon_cmd = argv[++i];
    }

    AppState state;
    state.cfg     = load_config();
    state.history = load_history();
#ifndef _WIN32
    if (const char* home_val = getenv("HOME")) state.gh_poller.load_from_config(home_val);
#endif
    state.gh_poller.start_polling();

    ErrorParser parser;
    PluginManager::ensure_builtin_plugins();
    state.plugins.load_plugins();

    // ── Build runner ─────────────────────────────
    auto run_cmd_logic = [&](std::string cmd, std::string tool, std::string proj, std::string project_dir = "") {
        std::thread([&state, &parser, cmd, tool, proj, project_dir]() {
            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) return;
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                BuildJob b;
                b.project = proj; b.tool = tool; b.status = "Building";
                b.progress = 0.0f; b.pid = 0;
                b.timestamp = std::time(nullptr);
                b.start_time = std::time(nullptr);
                b.auto_scroll = true;
                state.jobs.push_back(b);
            }
            // Fire plugin hook
            PluginBuildCtx pctx;
            pctx.command = cmd; pctx.project = proj; pctx.tool = tool;
            pctx.status = "started";
            state.plugins.fire("build_start", pctx);

            std::vector<std::string> output_lines;
            char buf[1024];
            while (fgets(buf, sizeof(buf), pipe)) {
                std::string line(buf);
                if (!line.empty() && line.back() == '\n') line.pop_back();
                output_lines.push_back(line);
                std::lock_guard<std::mutex> lk(state.mtx);
                for (auto& j : state.jobs) {
                    if (j.project == proj) {
                        j.log_lines.push_back(line);
                        if (j.log_lines.size() > 500) j.log_lines.erase(j.log_lines.begin());
                        j.progress = std::min(j.progress + 0.02f, 0.95f);
                        auto errs = parser.parse({line}, project_dir, tool);
                        j.errors.insert(j.errors.end(), errs.begin(), errs.end());
                        state.all_errors.insert(state.all_errors.end(), errs.begin(), errs.end());
                    }
                }
            }
            int exit_code = pclose(pipe);
            bool success = (exit_code == 0);

            auto final_errors = parser.parse(output_lines, project_dir, tool);

            std::lock_guard<std::mutex> lk(state.mtx);
            for (auto it = state.jobs.begin(); it != state.jobs.end(); ) {
                if (it->project == proj) {
                    it->status   = success ? "Finished" : "Failed";
                    it->progress = 1.0f;
                    it->duration_seconds = (int)(std::time(nullptr) - it->start_time);
                    it->errors   = final_errors;
                    state.last_diff  = diff_errors(state.prev_errors, final_errors);
                    for (auto& e : final_errors) {
                        for (auto& ne : state.last_diff.new_errors) {
                            if (error_key(e) == error_key(ne)) { e.is_new = true; break; }
                        }
                    }
                    state.prev_errors = final_errors;
                    if (!success) {
                        state.show_failure_overlay = true;
                        state.last_failed_project  = proj;
                    }
                    state.history.insert(state.history.begin(), *it);
                    if (state.history.size() > 50) state.history.pop_back();
                    save_history(state.history);
                    it = state.jobs.erase(it);
                    break;
                } else ++it;
            }
            state.all_errors = final_errors;

            if (state.cfg.notifications) send_notification(proj, success ? "Build Finished" : "Build FAILED");
            play_sound(state.cfg, success);
            broadcast_update(state.jobs, state.cpu, state.ram);
            // Fire plugin hook
            PluginBuildCtx pctx_end;
            pctx_end.command = cmd; pctx_end.project = proj; pctx_end.tool = tool;
            pctx_end.status = success ? "finished" : "failed";
            pctx_end.errors = final_errors;
            state.plugins.fire("build_end", pctx_end);
        }).detach();
    };

    g_ws_server.on_message = [&](std::shared_ptr<WsConn> conn, const std::string& msg) {
        try {
            json req = json::parse(msg);
            std::string type = req.value("type", "");
            if (type == "start_build") {
                std::string cmd = req.value("command", "");
                std::string dir = req.value("directory", "");
                if (cmd.empty()) return;
                std::string tool = cmd.substr(0, cmd.find(' '));
                std::string proj = dir.empty() ? "remote" : fs::path(dir).filename().string();
                std::string final_cmd = cmd + " 2>&1";
                run_cmd_logic(final_cmd, tool, proj, dir);
            } 
            else if (type == "stop_build") {
                int pid = req.value("pid", 0);
                if (pid > 0) {
                    std::string kill_cmd = "kill -9 " + std::to_string(pid) + " 2>/dev/null";
                    auto sys_ret = system(kill_cmd.c_str());
                    (void)sys_ret;
                }
            }
            else if (type == "get_history") {
                json resp;
                resp["type"] = "history";
                resp["history"] = json::array();
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    for (auto& h : state.history) {
                        json b;
                        b["project"] = h.project;
                        b["tool"] = h.tool;
                        b["status"] = h.status;
                        b["duration"] = h.duration_seconds;
                        b["errors"] = h.errors.size();
                        b["timestamp"] = (long long)h.timestamp;
                        resp["history"].push_back(b);
                    }
                }
                conn->send_text(resp.dump());
            }
        } catch (...) {}
    };

    g_ws_server.start(8765);
    g_discovery.start(8766, "BUILDM-ON_DISCOVERY");

    // ── No-TUI daemon mode ────────────────────────
    if (no_tui) {
        printf("[buildm-on] Daemon mode started. WS: 8765, UDP: 8766\n");
        if (!daemon_cmd.empty()) {
            std::string tool = daemon_cmd.substr(0, daemon_cmd.find(' '));
            run_cmd_logic(daemon_cmd + " 2>&1", tool, "daemon");
        }
        while (true) {
            std::lock_guard<std::mutex> lk(state.mtx);
            auto new_jobs = OSUtils::scan_processes();
            auto sys = OSUtils::get_system_stats();
            state.cpu = sys.cpu;
            state.ram = sys.ram;
            state.net = OSUtils::get_net_stats();
            for (auto& nj : new_jobs) {
                bool exists = false;
                for (auto& oj : state.jobs) if (oj.pid == nj.pid) { exists = true; break; }
                if (!exists) state.jobs.push_back(nj);
            }
            broadcast_update(state.jobs, state.cpu, state.ram);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return 0; // unreachable
    }

    // ── TUI ───────────────────────────────────────
    auto screen = ScreenInteractive::Fullscreen();
    bool is_running = true;
    int selected_tab = 0;
    int errors_scroll = 0;
    int history_scroll = 0;
    int plugins_scroll = 0;

    std::thread([&] {
        while (is_running) {
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                auto sys = OSUtils::get_system_stats();
                state.cpu = sys.cpu;
                state.ram = sys.ram;
                state.net = OSUtils::get_net_stats();
                state.uptime_secs++;
                auto new_scanned = OSUtils::scan_processes();
                for (auto& nj : new_scanned) {
                    bool exists = false;
                    for (auto& oj : state.jobs) if (oj.pid == nj.pid) { oj.status = "Active"; exists = true; break; }
                    if (!exists) state.jobs.push_back(nj);
                }
                for (auto& j : state.jobs) {
                    if (j.start_time > 0)
                        j.duration_seconds = (int)(std::time(nullptr) - j.start_time);
                }
                broadcast_update(state.jobs, state.cpu, state.ram);
            }
            screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();

    // ── Input components ──────────────────────────
    auto dir_input = Input(&state.launcher_dir_input, "Directory (e.g. ~/my-app)");
    auto cmd_input = Input(&state.launcher_cmd_input, "Command (e.g. cargo build)");

    // ── Renderer: assembling from modular parts ──
    auto renderer = Renderer(Container::Vertical({dir_input, cmd_input}), [&]() -> Element {
        std::lock_guard<std::mutex> lk(state.mtx);

        // 1. Update Global Stats
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream ss;
        ss << std::put_time(std::localtime(&t), "%a %b %d · %H:%M:%S");
        
        UI::SystemStats ui_stats;
        ui_stats.mobile_active = g_ws_server.is_active();
        ui_stats.cpu_percent = (int)state.cpu;
        ui_stats.ram_gb = (state.ram / 100.0f) * 16.0f; // Mock total RAM for GB display
        ui_stats.net_down_kb = state.net.down * 1024.0f;
        ui_stats.net_up_kb = state.net.up * 1024.0f;
        ui_stats.error_count = (int)state.all_errors.size();
        ui_stats.time_str = ss.str();

        // 2. Prepare Tab Data
        state.dash_state.stats = ui_stats;
        state.dash_state.selected_build = -1; // TODO: link with selection
        state.dash_state.builds.clear();
        for (auto& j : state.jobs) state.dash_state.builds.push_back(to_ui_build(j));
        state.dash_state.errors.clear();
        for (auto& e : state.all_errors) state.dash_state.errors.push_back(to_ui_error(e));
        
        // Logs for Dashboard
        state.dash_state.logs.clear();
        if (!state.jobs.empty()) {
            auto& active = state.jobs.front();
            int start = active.auto_scroll ? std::max(0, (int)active.log_lines.size() - 10) : active.log_scroll_pos;
            for (int i = start; i < (int)active.log_lines.size(); ++i) {
                UI::LogLine l;
                l.timestamp = ""; l.prefix = ""; l.body = active.log_lines[i];
                l.level = UI::LogLevel::Info;
                state.dash_state.logs.push_back(l);
            }
        }

        state.dash_state.stat_tiles = {
            { "Builds Today", std::to_string(state.history.size()), "active session", Theme::Sky },
            { "Success Rate", "100%", "no failures", Theme::Sage },
        };

        // 3. Prepare Launcher Data
        state.launcher_state.directory = state.launcher_dir_input;
        state.launcher_state.command = state.launcher_cmd_input;
        state.launcher_state.running = !state.jobs.empty();
        state.launcher_state.output_log = state.dash_state.logs;

        // 4. Render Active Tab
        Element content;
        switch (state.active_tab) {
            case 0: content = UI::DashboardScreen(state.dash_state, state.active_tab); break;
            case 1: content = UI::LauncherView(state.launcher_state, state.active_tab); break;
            case 2: { // History
                Elements elements;
                elements.push_back(UI::StatusBar(ui_stats));
                elements.push_back(UI::TabBar(state.active_tab));
                elements.push_back(Theme::Rule());
                elements.push_back(text("  History View — Modularization in progress") | color(Theme::TextDim) | flex);
                elements.push_back(UI::BottomBar({{"q", "quit"}}));
                content = vbox(std::move(elements));
                break;
            }
            case 3: { // Plugins
                Elements elements;
                elements.push_back(UI::StatusBar(ui_stats));
                elements.push_back(UI::TabBar(state.active_tab));
                elements.push_back(Theme::Rule());
                elements.push_back(text("  Plugins View — Modularization in progress") | color(Theme::TextDim) | flex);
                elements.push_back(UI::BottomBar({{"q", "quit"}}));
                content = vbox(std::move(elements));
                break;
            }
            default: { // Help
                Elements elements;
                elements.push_back(UI::StatusBar(ui_stats));
                elements.push_back(UI::TabBar(state.active_tab));
                elements.push_back(Theme::Rule());
                elements.push_back(text("  Help View — Modularization in progress") | color(Theme::TextDim) | flex);
                elements.push_back(UI::BottomBar({{"q", "quit"}}));
                content = vbox(std::move(elements));
                break;
            }
        }

        // 5. Final Assembly with Overlays
        if (state.show_failure_overlay) {
            Elements elements;
            elements.push_back(text(" ✖ Build Failed ") | bold | color(Theme::Rose) | hcenter);
            elements.push_back(separator());
            elements.push_back(text("  Errors: " + std::to_string(state.all_errors.size())) | color(Theme::Rose));
            elements.push_back(text(""));
            elements.push_back(text("  [Esc] to close") | dim | hcenter);
            
            auto overlay = vbox(std::move(elements)) | size(WIDTH, EQUAL, 40) | borderDouble | color(Theme::Rose) | center;
            return dbox(Elements{ content, overlay });
        }

        return content;
    });

    // ── Global Event Handler ──────────────────────
    auto event_handler = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Character('q')) { is_running = false; screen.ExitLoopClosure()(); return true; }
        if (e == Event::Character('1')) { state.active_tab = 0; return true; }
        if (e == Event::Character('2')) { state.active_tab = 1; return true; }
        if (e == Event::Character('3')) { state.active_tab = 2; return true; }
        if (e == Event::Character('4')) { state.active_tab = 3; return true; }
        if (e == Event::Character('5')) { state.active_tab = 4; return true; }

        if (e == Event::Escape) { state.show_failure_overlay = false; return true; }

        // Dashboard Navigation
        if (state.active_tab == 0 && !state.all_errors.empty()) {
            if (e == Event::Character('j')) { state.active_error_idx = (state.active_error_idx + 1) % state.all_errors.size(); return true; }
            if (e == Event::Character('k')) { state.active_error_idx = (state.active_error_idx + state.all_errors.size() - 1) % state.all_errors.size(); return true; }
            if (e == Event::Return) {
                auto& err = state.all_errors[state.active_error_idx];
                if (!err.file.empty()) {
                    std::string editor = getenv("EDITOR") ? getenv("EDITOR") : "vim";
                    std::string cmd = editor + " +" + std::to_string(err.line) + " " + err.file;
                    screen.ExitLoopClosure()();
                    system(cmd.c_str());
                }
                return true;
            }
        }

        // Launcher Controls
        if (state.active_tab == 1 && e == Event::Return) {
            if (state.launcher_cmd_input.empty()) return true;
            std::string tool = state.launcher_cmd_input.substr(0, state.launcher_cmd_input.find(' '));
            std::string proj = state.launcher_dir_input.empty() ? "terminal" : fs::path(state.launcher_dir_input).filename().string();
            std::string final_cmd = state.launcher_cmd_input + " 2>&1";
            if (!state.launcher_dir_input.empty()) final_cmd = "cd " + state.launcher_dir_input + " && " + final_cmd;
            run_cmd_logic(final_cmd, tool, proj, state.launcher_dir_input);
            return true;
        }

        return false;
    });

    screen.Loop(event_handler);
    is_running = false;
    return 0;
}
