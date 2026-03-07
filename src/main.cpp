#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>
#include "ws_server.hpp"

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
};

struct BuildJob {
    std::string project;
    std::string tool;
    std::string status;
    float progress;
    int pid;
    int duration_seconds = 0;
    std::time_t timestamp = 0;
};

void to_json(json& j, const BuildJob& b) {
    j = json{{"project", b.project}, {"tool", b.tool}, {"status", b.status}, 
             {"progress", b.progress}, {"pid", b.pid}, {"duration", b.duration_seconds},
             {"timestamp", (long long)b.timestamp}};
}

void from_json(const json& j, BuildJob& b) {
    j.at("project").get_to(b.project);
    j.at("tool").get_to(b.tool);
    j.at("status").get_to(b.status);
    j.at("progress").get_to(b.progress);
    j.at("pid").get_to(b.pid);
    j.at("duration").get_to(b.duration_seconds);
    if (j.contains("timestamp")) {
        long long ts = j.at("timestamp").get<long long>();
        b.timestamp = (std::time_t)ts;
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
            if (line.find("notifications") != std::string::npos && line.find("false") != std::string::npos) {
                cfg.notifications = false;
            }
        }
    }
#endif
    return cfg;
}

void send_notification(const std::string& summary, const std::string& body) {
#ifndef _WIN32
    std::string cmd = "notify-send \"" + summary + "\" \"" + body + "\" &";
    auto res = system(cmd.c_str());
    (void)res;
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
                for(int i=0; i<7; i++) ss >> junk;
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
// WebSocket & Server Logic
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
        msg["builds"].push_back({
            {"project", job.project},
            {"tool", job.tool},
            {"status", job.status},
            {"progress", job.progress},
            {"pid", job.pid}
        });
    }
    g_ws_server.broadcast(msg.dump());
}

// ─────────────────────────────────────────────
// Process Scanner
// ─────────────────────────────────────────────

const std::map<std::string, std::string> BUILD_TOOLS = {
    {"cargo", "Rust"}, {"npm", "Node"}, {"make", "Make"}, {"gcc", "C/C++"}, {"go", "Go"}
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
    ssize_t len = readlink(("/proc/" + std::to_string(pid) + "/cwd").c_str(), buf, sizeof(buf)-1);
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
                j.push_back({proj, tool, "Active", 0.5f, pid, 0, std::time(nullptr)});
                break;
            }
        }
    }
    return j;
}

// ─────────────────────────────────────────────
// Renderer Helpers
// ─────────────────────────────────────────────

ftxui::Element render_progress_bar(float progress, int width = 30) {
    int filled = static_cast<int>(progress * (float)width);
    std::string bar = "[";
    for (int i = 0; i < width; i++) bar += (i < filled) ? "█" : "░";
    bar += "] " + std::to_string((int)(progress * 100)) + "%";
    return ftxui::text(bar) | ftxui::color(progress > 0.8f ? ftxui::Color::Green : ftxui::Color::Yellow);
}

// ─────────────────────────────────────────────
// Main Dashboard TUI
// ─────────────────────────────────────────────

int main(int argc, char** argv) {
    using namespace ftxui;

    auto screen = ScreenInteractive::Fullscreen();
    Config cfg = load_config();
    std::vector<BuildJob> jobs;
    std::vector<BuildJob> history_jobs = load_history();
    std::mutex data_mutex;
    float cpu_usage = 0, ram_usage = 0;
    NetStats net = {0, 0};
    bool is_running = true;
    int selected_tab = 0;
    int uptime_seconds = 0;

    g_ws_server.start(8765);
    g_discovery.start(8766, "BUILDM-ON_DISCOVERY");

    auto run_cmd_logic = [&](std::string cmd, std::string tool, std::string proj) {
        std::thread([&, cmd, tool, proj] {
            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) return;
            BuildJob b = {proj, tool, "Building", 0.0f, 0, 0, std::time(nullptr)};
            { std::lock_guard<std::mutex> lk(data_mutex); jobs.push_back(b); }
            char buf[512];
            while (fgets(buf, sizeof(buf), pipe)) {
                std::lock_guard<std::mutex> lk(data_mutex);
                for (auto& j : jobs) if (j.project == proj) j.progress = std::min(j.progress + 0.05f, 0.99f);
                screen.PostEvent(Event::Custom);
            }
            pclose(pipe);
            {
                std::lock_guard<std::mutex> lk(data_mutex);
                for (auto it = jobs.begin(); it != jobs.end(); ) {
                    if (it->project == proj) {
                        it->status = "Finished"; it->progress = 1.0f;
                        history_jobs.insert(history_jobs.begin(), *it);
                        if (history_jobs.size() > 50) history_jobs.pop_back();
                        save_history(history_jobs);
                        it = jobs.erase(it); break;
                    } else ++it;
                }
            }
            if (cfg.notifications) send_notification(proj, "Build Finished");
            screen.PostEvent(Event::Custom);
        }).detach();
    };

    std::thread([&] {
        while (is_running) {
            auto new_scanned = scan_processes();
            {
                std::lock_guard<std::mutex> lk(data_mutex);
                cpu_usage = get_cpu_usage();
                ram_usage = get_ram_usage();
                net = get_net_stats();
                uptime_seconds++;
                for (auto& nj : new_scanned) {
                    bool exists = false;
                    for (auto& oj : jobs) if (oj.pid == nj.pid) { oj.status = "Active"; exists = true; break; }
                    if (!exists) jobs.push_back(nj);
                }
                broadcast_update(jobs, cpu_usage, ram_usage);
            }
            screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();

    std::string cmd_str = "", dir_str = "";
    auto dir_input = ftxui::Input(&dir_str, "Directory (e.g. ~/my-app)");
    auto cmd_input = ftxui::Input(&cmd_str, "Command (e.g. cargo build)");
    
    auto run_btn = ftxui::Button(" RUN BUILD ", [&] {
        if (cmd_str.empty()) return;
        std::string tool = cmd_str.substr(0, cmd_str.find(' '));
        std::string proj = dir_str.empty() ? "terminal" : fs::path(dir_str).filename().string();
        std::string final_cmd = cmd_str + " 2>&1";
        if (!dir_str.empty()) final_cmd = "cd " + dir_str + " && " + final_cmd;
        run_cmd_logic(final_cmd, tool, proj);
        cmd_str = "";
    }, ButtonOption::Animated(Color::Green, Color::White));

    auto dashboard_view = Renderer([&] {
        std::lock_guard<std::mutex> lk(data_mutex);
        Elements build_rows;
        if (jobs.empty()) {
            build_rows.push_back(text("No active builds.") | dim);
        } else {
            for (auto& j : jobs) {
                build_rows.push_back(hbox({text(j.project) | size(WIDTH, EQUAL, 20), filler(), render_progress_bar(j.progress)}));
            }
        }
        
        return vbox({
            hbox({
                vbox({
                    text("ACTIVE BUILDS") | bold | color(Color::Cyan), separator(),
                    vbox(std::move(build_rows))
                }) | flex | border,
                vbox({
                    text("SYSTEM STATS") | bold | color(Color::Green), separator(),
                    hbox({text("CPU: ") | size(WIDTH, EQUAL, 6), gauge(cpu_usage/100.0f) | color(Color::Yellow), text(" " + std::to_string((int)cpu_usage) + "%")}),
                    hbox({text("RAM: ") | size(WIDTH, EQUAL, 6), gauge(ram_usage/100.0f) | color(Color::Magenta), text(" " + std::to_string((int)ram_usage) + "%")}),
                    hbox({text("NET: ") | size(WIDTH, EQUAL, 6), text("↓ " + std::to_string((int)(net.down*10)/10.0f) + " MB/s  ↑ " + std::to_string((int)(net.up*10)/10.0f) + " MB/s") | color(Color::Cyan)}),
                    hbox({text("UP:  ") | size(WIDTH, EQUAL, 6), text(std::to_string(uptime_seconds/3600) + "h " + std::to_string((uptime_seconds%3600)/60) + "m") | color(Color::White)})
                }) | size(WIDTH, EQUAL, 40) | border
            }) | flex
        });
    });

    auto run_view = Renderer(Container::Vertical({dir_input, cmd_input, run_btn}), [&] {
        return vbox({
            text("LAUNCH NEW BUILD") | bold | color(Color::Cyan), separator(),
            hbox({text("Directory: ") | size(WIDTH, EQUAL, 12), dir_input->Render() | border}),
            hbox({text("Command:   ") | size(WIDTH, EQUAL, 12), cmd_input->Render() | border}),
            run_btn->Render() | hcenter,
            filler(),
            text("Shortcuts: [Enter] in Command field to Run, [Tab] to switch fields.") | dim | hcenter
        }) | flex | border;
    });

    auto history_view = Renderer([&] {
        std::lock_guard<std::mutex> lk(data_mutex);
        Elements el;
        for (auto& h : history_jobs) {
            el.push_back(hbox({text(h.project) | bold, filler(), text(h.tool) | dim, text(" [" + h.status + "]") | color(h.status=="Finished"?Color::Green:Color::Red)}));
        }
        return vbox({text("BUILD HISTORY (Persistent)") | bold, separator(), vbox(std::move(el)) | yframe}) | flex | border;
    });

    auto help_view = Renderer([&] {
        return vbox({
            text("HELP & SHORTCUTS") | bold, separator(),
            text("1-4: Switch Tabs"), text("q:   Quit Application"),
            text("Buildm-on is a terminal dashboard for developers.")
        }) | flex | border;
    });

    std::vector<std::string> tab_entries = {" DASHBOARD ", " RUN COMMAND ", " HISTORY ", " HELP "};
    auto tab_menu = Menu(&tab_entries, &selected_tab);
    
    auto tab_container = Container::Tab({dashboard_view, run_view, history_view, help_view}, &selected_tab);
    auto main_container = Container::Vertical({tab_menu, tab_container});

    auto renderer = Renderer(main_container, [&] {
        return vbox({
            hbox({
                text(" Buildm-on v1.0 ") | bold | color(Color::White) | bgcolor(Color::Blue), 
                filler(), 
                text("Mobile: ") | color(Color::White), 
                text(g_ws_server.is_active() ? "Active" : "Ready") | color(g_ws_server.is_active() ? Color::Green : Color::Yellow)
            }) | border,
            tab_menu->Render() | hcenter | color(Color::Cyan),
            tab_container->Render() | flex,
            hbox({text(" [q] Quit  [1-4] Tabs  [Enter] Confirm ")}) | dim
        });
    });

    auto event_handler = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Character('q')) { is_running = false; screen.ExitLoopClosure()(); return true; }
        if (e == Event::Character('1')) { selected_tab = 0; return true; }
        if (e == Event::Character('2')) { selected_tab = 1; return true; }
        if (e == Event::Character('3')) { selected_tab = 2; return true; }
        if (e == Event::Character('4')) { selected_tab = 3; return true; }
        return false;
    });

    screen.Loop(event_handler);
    is_running = false;
    return 0;
}
