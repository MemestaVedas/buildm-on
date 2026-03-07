#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>
#include "ws_server.hpp"
#include "error_parser.hpp"
#include "plugin_manager.hpp"

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

struct BuildJob {
    std::string project;
    std::string tool;
    std::string status;
    float progress = 0.0f;
    int pid = 0;
    int duration_seconds = 0;
    std::time_t timestamp = 0;
    std::vector<ParsedError> errors;
    std::vector<std::string> log_lines;
    bool auto_scroll = true;
    int log_scroll_pos = 0;
    std::time_t start_time = 0;
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
// System Stats
// ─────────────────────────────────────────────

float get_ram_usage() {
#ifdef _WIN32
    return 0.0f;
#else
    std::ifstream f("/proc/meminfo");
    std::string key;
    long total = 0, available = 0;
    while (f >> key) {
        if (key == "MemTotal:") f >> total;
        else if (key == "MemAvailable:") f >> available;
    }
    if (total == 0) return 0.0f;
    return 100.0f * (1.0f - (float)available / total);
#endif
}

float get_cpu_usage() {
#ifdef _WIN32
    return 0.0f;
#else
    static long prev_idle = 0, prev_total = 0;
    std::ifstream f("/proc/stat");
    std::string line;
    if (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cpu;
        long u, n, s, i, iw, irq, si;
        if (!(ss >> cpu >> u >> n >> s >> i >> iw >> irq >> si)) return 0.0f;
        long total = u + n + s + i + iw + irq + si;
        long diff_idle = i - prev_idle;
        long diff_total = total - prev_total;
        prev_idle = i; prev_total = total;
        if (diff_total == 0) return 0.0f;
        return 100.0f * (1.0f - (float)diff_idle / diff_total);
    }
    return 0.0f;
#endif
}

struct NetStats { float down; float up; };
NetStats get_net_stats() {
#ifdef _WIN32
    return {0, 0};
#else
    static long long prev_rx = 0, prev_tx = 0;
    std::ifstream f("/proc/net/dev");
    std::string line;
    long long rx = 0, tx = 0;
    if (f.is_open()) {
        while (std::getline(f, line)) {
            if (line.find(":") != std::string::npos) {
                std::stringstream ss(line.substr(line.find(":") + 1));
                long long r, t, junk;
                if (!(ss >> r)) continue;
                for (int i = 0; i < 7; i++) ss >> junk;
                if (!(ss >> t)) continue;
                rx += r; tx += t;
            }
        }
    }
    float drx = (rx - prev_rx) / 1024.0f / 1024.0f;
    float dtx = (tx - prev_tx) / 1024.0f / 1024.0f;
    prev_rx = rx; prev_tx = tx;
    return {std::max(0.0f, drx), std::max(0.0f, dtx)};
#endif
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

// ─────────────────────────────────────────────
// Process Scanner
// ─────────────────────────────────────────────

const std::map<std::string, std::string> BUILD_TOOLS = {
    {"cargo",   "Rust"}, {"npm",    "Node"}, {"make",  "Make"},
    {"gcc",     "C/C++"}, {"g++",  "C/C++"}, {"clang", "C/C++"},
    {"go",      "Go"}, {"cmake", "CMake"}, {"bazel",  "Bazel"},
    {"gradle",  "Gradle"}, {"mvn", "Maven"}, {"yarn",  "Node"},
    {"bun",     "Node"}, {"tsc",  "TypeScript"}, {"webpack", "Node"}
};

std::string get_proc_cmdline(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline");
    std::string line, result;
    if (std::getline(f, line)) {
        for (char c : line) result += (c == '\0') ? ' ' : c;
    }
    return result;
}

std::string get_proc_cwd(int pid) {
#ifndef _WIN32
    char buf[512];
    ssize_t len = readlink(("/proc/" + std::to_string(pid) + "/cwd").c_str(), buf, sizeof(buf) - 1);
    if (len != -1) { buf[len] = '\0'; return std::string(buf); }
#endif
    return "";
}

std::vector<BuildJob> scan_processes() {
    std::vector<BuildJob> j;
    std::error_code ec;
    if (!fs::exists("/proc", ec)) return j;
    for (auto& entry : fs::directory_iterator("/proc", ec)) {
        std::string name = entry.path().filename().string();
        if (!std::all_of(name.begin(), name.end(), ::isdigit)) continue;
        int pid = std::stoi(name);
        std::string cmd = get_proc_cmdline(pid);
        for (auto& [tool, label] : BUILD_TOOLS) {
            if (cmd.find(tool) != std::string::npos) {
                std::string cwd = get_proc_cwd(pid);
                std::string proj = cwd.empty() ? "unknown" : fs::path(cwd).filename().string();
                BuildJob bj;
                bj.project = proj; bj.tool = tool; bj.status = "Active";
                bj.progress = 0.5f; bj.pid = pid;
                bj.timestamp = std::time(nullptr);
                bj.start_time = std::time(nullptr);
                j.push_back(bj);
                break;
            }
        }
    }
    return j;
}

// ─────────────────────────────────────────────
// Formatting Helpers
// ─────────────────────────────────────────────

std::string format_duration(int seconds) {
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    char buf[16];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%02dh%02dm%02ds", h, m, s);
    else if (m > 0) std::snprintf(buf, sizeof(buf), "%02dm%02ds", m, s);
    else std::snprintf(buf, sizeof(buf), "%ds", s);
    return std::string(buf);
}

std::string format_timestamp(std::time_t t) {
    if (t == 0) return "—";
    char buf[32];
    struct tm* tm_info = localtime(&t);
    if (!tm_info) return "—";
    strftime(buf, sizeof(buf), "%m-%d %H:%M", tm_info);
    return std::string(buf);
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

    ErrorParser parser;
    PluginManager::ensure_builtin_plugins();
    state.plugins.load_plugins();
    g_ws_server.start(8765);
    g_discovery.start(8766, "BUILDM-ON_DISCOVERY");

    // ── Build runner ─────────────────────────────
    auto run_cmd_logic = [&](std::string cmd, std::string tool, std::string proj) {
        std::thread([&state, &parser, cmd, tool, proj]() {
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
                        auto errs = parser.parse({line}, tool);
                        j.errors.insert(j.errors.end(), errs.begin(), errs.end());
                        state.all_errors.insert(state.all_errors.end(), errs.begin(), errs.end());
                    }
                }
            }
            int exit_code = pclose(pipe);
            bool success = (exit_code == 0);

            auto final_errors = parser.parse(output_lines, tool);

            std::lock_guard<std::mutex> lk(state.mtx);
            for (auto it = state.jobs.begin(); it != state.jobs.end(); ) {
                if (it->project == proj) {
                    it->status   = success ? "Finished" : "Failed";
                    it->progress = 1.0f;
                    it->duration_seconds = (int)(std::time(nullptr) - it->start_time);
                    it->errors   = final_errors;
                    state.last_diff  = diff_errors(state.prev_errors, final_errors);
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

    // ── No-TUI daemon mode ────────────────────────
    if (no_tui) {
        printf("[buildm-on] Daemon mode started. WS: 8765, UDP: 8766\n");
        if (!daemon_cmd.empty()) {
            std::string tool = daemon_cmd.substr(0, daemon_cmd.find(' '));
            run_cmd_logic(daemon_cmd + " 2>&1", tool, "daemon");
        }
        while (true) {
            std::lock_guard<std::mutex> lk(state.mtx);
            auto new_jobs = scan_processes();
            state.cpu = get_cpu_usage();
            state.ram = get_ram_usage();
            state.net = get_net_stats();
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
                state.cpu = get_cpu_usage();
                state.ram = get_ram_usage();
                state.net = get_net_stats();
                state.uptime_secs++;
                auto new_scanned = scan_processes();
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
    std::string cmd_str = "", dir_str = "";
    auto dir_input = Input(&dir_str, "Directory (e.g. ~/my-app)");
    auto cmd_input = Input(&cmd_str, "Command (e.g. cargo build)");
    auto run_btn = Button(" > RUN BUILD ", [&] {
        if (cmd_str.empty()) return;
        std::string tool = cmd_str.substr(0, cmd_str.find(' '));
        std::string proj = dir_str.empty() ? "terminal" : fs::path(dir_str).filename().string();
        std::string final_cmd = cmd_str + " 2>&1";
        if (!dir_str.empty()) final_cmd = "cd " + dir_str + " && " + final_cmd;
        run_cmd_logic(final_cmd, tool, proj);
        cmd_str = "";
    }, ButtonOption::Animated(Color::Green, Color::White));

    // ── Dummy component for pure renderers ────────
    auto empty_comp = Container::Vertical({});

    // ═══════════════════════════════════════
    // TAB 1: Dashboard
    // ═══════════════════════════════════════
    auto dashboard_view = Renderer(empty_comp, [&] {
        std::lock_guard<std::mutex> lk(state.mtx);
        Elements build_rows;
        if (state.jobs.empty()) {
            build_rows.push_back(text("  No active builds detected.") | dim);
        } else {
            for (auto& j : state.jobs) {
                bool slow = (j.duration_seconds > state.cfg.slow_build_threshold_s);
                Color dur_color = slow ? Color::Red : Color::GrayLight;
                std::string err_badge = " [" + std::to_string(j.errors.size()) + " err]";
                Color err_color = j.errors.empty() ? Color::GrayDark : Color::Red;
                build_rows.push_back(hbox({
                    text("  "),
                    text(j.project) | bold | size(WIDTH, EQUAL, 16),
                    text(j.tool) | dim | size(WIDTH, EQUAL, 8),
                    text(j.status) | color(j.status == "Building" ? Color::Yellow : Color::Cyan) | size(WIDTH, EQUAL, 9),
                    render_progress_bar(j.progress),
                    filler(),
                    text(format_duration(j.duration_seconds)) | color(dur_color),
                    slow ? text(" SLOW") | color(Color::Red) : text(""),
                    text(err_badge) | color(err_color)
                }) | size(HEIGHT, EQUAL, 1));
            }
        }

        // Diff banner
        Elements diff_els;
        auto& diff = state.last_diff;
        if (!diff.new_errors.empty() || !diff.resolved_errors.empty() || !diff.persisting.empty()) {
            diff_els.push_back(separator());
            diff_els.push_back(hbox({
                text("  Delta: "),
                text("+" + std::to_string(diff.new_errors.size()) + " new ") | color(Color::Red),
                text("-" + std::to_string(diff.resolved_errors.size()) + " fixed ") | color(Color::Green),
                text("=" + std::to_string(diff.persisting.size()) + " persisting") | color(Color::Yellow)
            }));
        }

        Elements right_col;
        right_col.push_back(text(" SYSTEM") | bold | color(Color::Green));
        right_col.push_back(separator());
        right_col.push_back(hbox({text(" CPU: "), gauge(state.cpu / 100.0f) | color(Color::Yellow), text(" " + std::to_string((int)state.cpu) + "%")}));
        right_col.push_back(hbox({text(" RAM: "), gauge(state.ram / 100.0f) | color(Color::Magenta), text(" " + std::to_string((int)state.ram) + "%")}));
        right_col.push_back(hbox({text(" NET: "), text(std::to_string((int)(state.net.down * 10.0f) / 10) + "MB/s down") | color(Color::Cyan)}));
        right_col.push_back(hbox({text(" UP:  "), text(format_duration(state.uptime_secs)) | color(Color::White)}));
        right_col.push_back(separator());
        right_col.push_back(text(g_ws_server.is_active() ? " Mobile: Connected" : " Mobile: Waiting") | color(g_ws_server.is_active() ? Color::Green : Color::Yellow));

        Elements left_col;
        left_col.push_back(hbox({text(" ACTIVE BUILDS (" + std::to_string(state.jobs.size()) + ")") | bold | color(Color::Cyan), filler()}));
        left_col.push_back(separator());
        for (auto& e : build_rows) left_col.push_back(e);
        for (auto& e : diff_els)  left_col.push_back(e);

        return hbox({
            vbox(std::move(left_col)) | flex | border,
            vbox(std::move(right_col)) | size(WIDTH, EQUAL, 38) | border
        }) | flex;
    });

    // ═══════════════════════════════════════
    // TAB 2: Run Command
    // ═══════════════════════════════════════
    auto run_view = Renderer(Container::Vertical({dir_input, cmd_input, run_btn}), [&] {
        return vbox({
            text(" LAUNCH NEW BUILD") | bold | color(Color::Cyan),
            separator(),
            hbox({text("  Directory: ") | size(WIDTH, EQUAL, 14), dir_input->Render() | border}),
            hbox({text("  Command:   ") | size(WIDTH, EQUAL, 14), cmd_input->Render() | border}),
            separator(),
            run_btn->Render() | hcenter,
            filler(),
            text("  [Enter] Run  [Tab] Switch fields  [1-5] Change tab") | dim | hcenter
        }) | flex | border;
    });

    // ═══════════════════════════════════════
    // TAB 3: Build Log
    // ═══════════════════════════════════════
    auto log_view = Renderer(empty_comp, [&] {
        std::lock_guard<std::mutex> lk(state.mtx);
        Elements log_lines;
        BuildJob* active = nullptr;
        if (!state.jobs.empty()) active = &state.jobs.front();

        if (!active || active->log_lines.empty()) {
            log_lines.push_back(text("  No log output. Start a build to see output here.") | dim);
        } else {
            int total = (int)active->log_lines.size();
            int visible = 32;
            int start = active->auto_scroll
                ? std::max(0, total - visible)
                : std::max(0, std::min(active->log_scroll_pos, total - visible));
            for (int i = start; i < std::min(start + visible, total); i++) {
                const std::string& ln = active->log_lines[i];
                bool is_err  = ln.find("error") != std::string::npos || ln.find("Error") != std::string::npos;
                bool is_warn = ln.find("warning") != std::string::npos;
                Color c = is_err ? Color::Red : (is_warn ? Color::Yellow : Color::White);
                log_lines.push_back(text("  " + ln) | color(c));
            }
        }

        std::string scroll_hint = (active && !active->auto_scroll)
            ? "  [S] Resume auto-scroll" : "  [S] Lock scroll / [Arrow] Scroll";

        return vbox({
            hbox({text(" BUILD LOG") | bold | color(Color::Cyan), filler(), text(scroll_hint) | dim}),
            separator(),
            vbox(std::move(log_lines)) | flex
        }) | flex | border;
    });

    // ═══════════════════════════════════════
    // TAB 4: History
    // ═══════════════════════════════════════
    auto history_view = Renderer(empty_comp, [&] {
        std::lock_guard<std::mutex> lk(state.mtx);
        Elements el;
        el.push_back(hbox({
            text("  Project") | bold | size(WIDTH, EQUAL, 18),
            text("Tool") | bold | size(WIDTH, EQUAL, 9),
            text("Status") | bold | size(WIDTH, EQUAL, 10),
            text("Dur") | bold | size(WIDTH, EQUAL, 9),
            text("Err") | bold | size(WIDTH, EQUAL, 5),
            text("Time") | bold
        }) | color(Color::GrayLight));
        el.push_back(separator());

        if (state.history.empty()) {
            el.push_back(text("  No history yet.") | dim);
        } else {
            int start = std::max(0, history_scroll);
            int end   = std::min((int)state.history.size(), start + 25);
            for (int i = start; i < end; i++) {
                auto& h = state.history[i];
                bool failed = (h.status == "Failed");
                int   ec    = (int)h.errors.size();
                el.push_back(hbox({
                    text("  " + h.project) | bold | size(WIDTH, EQUAL, 18),
                    text(h.tool) | dim | size(WIDTH, EQUAL, 9),
                    text(h.status) | color(failed ? Color::Red : Color::Green) | size(WIDTH, EQUAL, 10),
                    text(format_duration(h.duration_seconds)) | size(WIDTH, EQUAL, 9),
                    text(std::to_string(ec)) | color(ec > 0 ? Color::Red : Color::GrayLight) | size(WIDTH, EQUAL, 5),
                    text(format_timestamp(h.timestamp)) | dim
                }));
            }
        }

        return vbox({
            hbox({text(" BUILD HISTORY") | bold | color(Color::Cyan), filler(), text("  [Up/Down] Scroll  [C] Copy errors") | dim}),
            separator(),
            vbox(std::move(el)) | flex
        }) | flex | border;
    });

    // ═══════════════════════════════════════
    // TAB 5: Errors
    // ═══════════════════════════════════════
    auto errors_view = Renderer(empty_comp, [&] {
        std::lock_guard<std::mutex> lk(state.mtx);
        Elements el;

        // Diff banner
        auto& diff = state.last_diff;
        if (!diff.new_errors.empty() || !diff.resolved_errors.empty()) {
            el.push_back(hbox({
                text("  Delta: "),
                text("+" + std::to_string(diff.new_errors.size()) + " new  ") | color(Color::Red),
                text("-" + std::to_string(diff.resolved_errors.size()) + " fixed  ") | color(Color::Green),
                text("=" + std::to_string(diff.persisting.size()) + " same") | color(Color::Yellow)
            }));
            el.push_back(separator());
        }

        if (state.all_errors.empty()) {
            el.push_back(text("  No errors — clean build!") | color(Color::Green));
        } else {
            int start = std::max(0, errors_scroll);
            int end   = std::min((int)state.all_errors.size(), start + 30);
            for (int i = start; i < end; i++) {
                auto& e = state.all_errors[i];
                std::string loc = e.file;
                if (e.line > 0) loc += ":" + std::to_string(e.line);
                if (e.column > 0) loc += ":" + std::to_string(e.column);
                std::string code_part = e.code.empty() ? "" : " [" + e.code + "]";

                el.push_back(hbox({
                    text("  " + severity_icon(e.severity)) | color(severity_color(e.severity)),
                    text(" "),
                    text(loc.empty() ? "(no location)" : loc) | color(Color::Cyan) | size(WIDTH, EQUAL, 30),
                    text(e.message + code_part) | color(severity_color(e.severity))
                }));
                if (!e.file.empty()) {
                    el.push_back(text("       vim +" + std::to_string(e.line) + " " + e.file) | dim | color(Color::GrayDark));
                }
            }
        }

        int ec = (int)state.all_errors.size();
        std::string badge = " ERRORS [" + std::to_string(ec) + "]";
        return vbox({
            hbox({
                text(badge) | bold | color(ec == 0 ? Color::Green : Color::Red),
                filler(),
                text("  [Up/Down] Scroll  [C] Copy  [Esc] Dismiss overlay") | dim
            }),
            separator(),
            vbox(std::move(el)) | flex
        }) | flex | border;
    });

    // ═══════════════════════════════════════
    // TAB 6: Plugins
    // ═══════════════════════════════════════
    auto plugins_view = Renderer(empty_comp, [&] {
        std::lock_guard<std::mutex> lk(state.mtx);
        Elements el;
        el.push_back(hbox({
            text("  Plugin Name") | bold | size(WIDTH, EQUAL, 25),
            text("Status") | bold | size(WIDTH, EQUAL, 10),
            text("Triggers") | bold | size(WIDTH, EQUAL, 20),
            text("Last Output") | bold
        }) | color(Color::GrayLight));
        el.push_back(separator());

        if (state.plugins.plugins.empty()) {
            el.push_back(text("  No plugins installed. (Add .lua files to ~/.buildm-on/plugins/)") | dim);
        } else {
            int start = std::max(0, plugins_scroll);
            int end   = std::min((int)state.plugins.plugins.size(), start + 25);
            for (int i = start; i < end; i++) {
                auto& p = state.plugins.plugins[i];
                std::string triggers = p.triggers.empty() ? "all" : "";
                if (triggers.empty()) {
                    for (size_t t = 0; t < p.triggers.size(); t++) {
                        triggers += p.triggers[t] + (t + 1 < p.triggers.size() ? "," : "");
                    }
                }
                Color status_c = p.enabled ? Color::Green : Color::GrayDark;
                std::string indicator = (i == plugins_scroll) ? "> " : "  ";
                el.push_back(hbox({
                    text(indicator + p.name) | color((i == plugins_scroll) ? Color::White : Color::GrayLight) | size(WIDTH, EQUAL, 25),
                    text(p.enabled ? "Enabled" : "Disabled") | color(status_c) | size(WIDTH, EQUAL, 10),
                    text(triggers) | dim | size(WIDTH, EQUAL, 20),
                    text(p.last_status) | size(WIDTH, EQUAL, 35)
                }));
                if (!p.description.empty()) {
                    el.push_back(text("    " + p.description) | dim | color(Color::Cyan));
                }
            }
        }

        return vbox({
            hbox({text(" PLUGINS") | bold | color(Color::Cyan), filler(), text("  [Up/Down] Select  [(E)nable / (D)isable]  [R] Reload") | dim}),
            separator(),
            vbox(std::move(el)) | flex
        }) | flex | border;
    });

    // ── Tab menu ──────────────────────────────────
    auto make_tab_entries = [&]() -> std::vector<std::string> {
        int ec = 0;
        { std::lock_guard<std::mutex> lk(state.mtx); ec = (int)state.all_errors.size(); }
        std::string err_label = " ERRORS";
        if (ec > 0) err_label += " [" + std::to_string(ec) + "]";
        return {" DASHBOARD ", " RUN ", " LOG ", " HISTORY ", err_label + " ", " PLUGINS "};
    };
    std::vector<std::string> tab_entries = make_tab_entries();
    auto tab_menu = Menu(&tab_entries, &selected_tab);
    auto tab_container = Container::Tab({
        dashboard_view, run_view, log_view, history_view, errors_view, plugins_view
    }, &selected_tab);
    auto main_container = Container::Vertical({tab_menu, tab_container});

    // ── Main renderer (with overlay) ───────────────
    auto renderer = Renderer(main_container, [&] {
        tab_entries = make_tab_entries();

        Element base = vbox({
            hbox({
                text(" Buildm-on v1.5 ") | bold | bgcolor(Color::Blue) | color(Color::White),
                filler(),
                text("WS:") | color(Color::GrayLight),
                text(g_ws_server.is_active() ? " Live " : " Wait ") | color(g_ws_server.is_active() ? Color::Green : Color::Yellow),
                text("| CPU:") | color(Color::GrayLight),
                text(std::to_string((int)state.cpu) + "% ") | color(Color::Yellow),
                text("RAM:") | color(Color::GrayLight),
                text(std::to_string((int)state.ram) + "% ") | color(Color::Magenta)
            }) | border,
            tab_menu->Render() | hcenter | color(Color::Cyan),
            tab_container->Render() | flex,
            text("  q:Quit  1-6:Tabs  S:Scroll  C:Copy-errors  Enter:Confirm  Esc:Dismiss") | dim
        });

        // Failure overlay
        if (state.show_failure_overlay) {
            std::lock_guard<std::mutex> lk(state.mtx);
            int ec = (int)state.all_errors.size();
            std::map<std::string, int> file_freq;
            for (auto& e : state.all_errors) if (!e.file.empty()) file_freq[e.file]++;
            std::string top_file = "—";
            int top_count = 0;
            for (auto& [ff, cnt] : file_freq) if (cnt > top_count) { top_file = ff; top_count = cnt; }
            std::string first_loc, first_msg;
            for (auto& e : state.all_errors) {
                if (e.severity == ErrorSeverity::Error) {
                    first_loc = e.file + ":" + std::to_string(e.line);
                    first_msg = e.message;
                    break;
                }
            }
            auto overlay = vbox({
                text("  BUILD FAILED — " + state.last_failed_project) | bold | color(Color::Red),
                separator(),
                hbox({text("  Errors:   "), text(std::to_string(ec)) | color(Color::Red)}),
                hbox({text("  Top file: "), text(top_file) | color(Color::Yellow)}),
                hbox({text("  Location: "), text(first_loc) | color(Color::Cyan)}),
                text("  " + first_msg.substr(0, 60)) | color(Color::White),
                separator(),
                text("  [C] Copy errors  [5] Errors tab  [Esc] Dismiss") | dim | hcenter
            }) | border | color(Color::Red) | size(WIDTH, EQUAL, 64);
            return dbox({base, overlay | hcenter | vcenter});
        }

        // Clipboard copy toast
        if (state.clipboard_copied) {
            auto toast = vbox({
                filler(),
                hbox({filler(), text("  Copied to clipboard!  ") | bold | bgcolor(Color::Green) | color(Color::Black) | border})
            });
            return dbox({base, toast});
        }

        return base;
    });

    // ── Event handler ─────────────────────────────
    auto event_handler = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Character('q'))   { is_running = false; screen.ExitLoopClosure()(); return true; }
        if (e == Event::Character('1'))   { selected_tab = 0; return true; }
        if (e == Event::Character('2'))   { selected_tab = 1; return true; }
        if (e == Event::Character('3'))   { selected_tab = 2; return true; }
        if (e == Event::Character('4'))   { selected_tab = 3; return true; }
        if (e == Event::Character('5'))   { selected_tab = 4; return true; }
        if (e == Event::Character('6'))   { selected_tab = 5; return true; }

        if (e == Event::Escape) {
            state.show_failure_overlay = false;
            state.clipboard_copied     = false;
            return true;
        }

        // [S] auto-scroll toggle
        if (e == Event::Character('s') || e == Event::Character('S')) {
            std::lock_guard<std::mutex> lk(state.mtx);
            if (!state.jobs.empty()) {
                auto& j = state.jobs.front();
                j.auto_scroll = !j.auto_scroll;
            }
            return true;
        }

        // [C] copy errors
        if (e == Event::Character('c') || e == Event::Character('C')) {
            std::string text_buf;
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                auto& src = (selected_tab == 3 && !state.history.empty())
                    ? state.history.front().errors
                    : state.all_errors;
                for (auto& err : src) {
                    text_buf += severity_str(err.severity) + ": ";
                    if (!err.file.empty()) text_buf += err.file + ":" + std::to_string(err.line) + ": ";
                    if (!err.code.empty()) text_buf += "[" + err.code + "] ";
                    text_buf += err.message + "\n";
                }
            }
            if (!text_buf.empty()) {
                copy_to_clipboard(text_buf);
                state.clipboard_copied = true;
                std::thread([&] {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    state.clipboard_copied = false;
                    screen.PostEvent(Event::Custom);
                }).detach();
            }
            return true;
        }

        // Plugin actions
        if (selected_tab == 5) { // Plugins tab
            if (e == Event::Character('r') || e == Event::Character('R')) {
                state.plugins.load_plugins();
                return true;
            }
            if (e == Event::Character('e') || e == Event::Character('E') ||
                e == Event::Character('d') || e == Event::Character('D')) {
                bool enable = (e == Event::Character('e') || e == Event::Character('E'));
                std::lock_guard<std::mutex> lk(state.mtx);
                if (!state.plugins.plugins.empty()) {
                    int idx = plugins_scroll;
                    if (idx >= 0 && idx < (int)state.plugins.plugins.size()) {
                        auto name = state.plugins.plugins[idx].name;
                        if (enable) state.plugins.enable(name);
                        else state.plugins.disable(name);
                    }
                }
                return true;
            }
            if (e == Event::ArrowDown) { plugins_scroll = std::min((int)state.plugins.plugins.size() - 1, plugins_scroll + 1); return true; }
            if (e == Event::ArrowUp)   { plugins_scroll = std::max(0, plugins_scroll - 1); return true; }
        }

        // Scroll arrows
        if (selected_tab == 4) { // Errors tab
            if (e == Event::ArrowDown) { errors_scroll++; return true; }
            if (e == Event::ArrowUp)   { errors_scroll = std::max(0, errors_scroll - 1); return true; }
        }
        if (selected_tab == 3) { // History tab
            if (e == Event::ArrowDown) { history_scroll++; return true; }
            if (e == Event::ArrowUp)   { history_scroll = std::max(0, history_scroll - 1); return true; }
        }
        if (selected_tab == 2) { // Log tab
            std::lock_guard<std::mutex> lk(state.mtx);
            if (!state.jobs.empty()) {
                if (e == Event::ArrowDown) { state.jobs.front().auto_scroll = false; state.jobs.front().log_scroll_pos++; return true; }
                if (e == Event::ArrowUp)   { state.jobs.front().auto_scroll = false; state.jobs.front().log_scroll_pos = std::max(0, state.jobs.front().log_scroll_pos - 1); return true; }
            }
        }
        return false;
    });

    screen.Loop(event_handler);
    is_running = false;
    return 0;
}
