#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>
#include "ws_server.hpp"
using json = nlohmann::json;

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
#include <deque>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/inotify.h>
#endif

using namespace ftxui;
namespace fs = std::filesystem;

// ─────────────────────────────────────────────
// Data Types
// ─────────────────────────────────────────────

struct Config {
    std::string theme = "default";
    bool notifications = true;
};

Config load_config() {
    Config cfg;
#ifndef _WIN32
    const char* home = getenv("HOME");
    if (!home) return cfg;
    std::ifstream f(std::string(home) + "/.buildm-on.toml");
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("theme") != std::string::npos && line.find("=") != std::string::npos) {
            if (line.find("ocean") != std::string::npos) cfg.theme = "ocean";
            else if (line.find("matrix") != std::string::npos) cfg.theme = "matrix";
        }
        if (line.find("notifications") != std::string::npos && line.find("false") != std::string::npos) {
            cfg.notifications = false;
        }
    }
#endif
    return cfg;
}

void send_notification(const std::string& summary, const std::string& body) {
#ifndef _WIN32
    std::string cmd = "notify-send \"" + summary + "\" \"" + body + "\" &";
    system(cmd.c_str());
#endif
}

struct BuildJob {
    std::string project;   // folder name of the process
    std::string tool;      // cargo, npm, gcc, etc.
    std::string status;    // Compiling, Bundling, Linking...
    float progress;        // 0.0 to 1.0
    int pid;
    int duration_seconds = 0;
};

void send_ipc_update(const BuildJob& job) {
#ifndef _WIN32
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return;
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/buildm-on.sock", sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        std::string msg = job.project + "|" + job.tool + "|" + job.status + "|" + std::to_string(job.progress);
        write(sock, msg.c_str(), msg.size());
    }
    close(sock);
#endif
}

// ─────────────────────────────────────────────
// WebSocket Server
// ─────────────────────────────────────────────

WsServer g_ws_server;
UdpBroadcaster g_discovery;

json job_to_json(const BuildJob& job) {
    return {
        {"project",          job.project},
        {"tool",             job.tool},
        {"status",           job.status},
        {"progress",         job.progress},
        {"pid",              job.pid},
        {"duration_seconds", job.duration_seconds}
    };
}

void broadcast_update(const std::vector<BuildJob>& jobs, float cpu) {
    json msg;
    msg["type"]         = "update";
    msg["timestamp"]    = std::time(nullptr);
    msg["cpu"]          = cpu;
    msg["active_count"] = jobs.size();
    msg["builds"]       = json::array();
    for (const auto& job : jobs)
        msg["builds"].push_back(job_to_json(job));
    g_ws_server.broadcast(msg.dump());
}

void broadcast_event(const std::string& type, const BuildJob& job,
                     bool success, const std::string& error_line = "") {
    json msg;
    msg["type"]             = type;
    msg["project"]          = job.project;
    msg["tool"]             = job.tool;
    msg["duration_seconds"] = job.duration_seconds;
    msg["success"]          = success;
    if (!error_line.empty())
        msg["error_line"]   = error_line;
    g_ws_server.broadcast(msg.dump());
}

void start_server() {
    g_ws_server.start(8765);
}

// ─────────────────────────────────────────────
// Process Scanner
// Reads /proc to find active build processes
// ─────────────────────────────────────────────

// Map of known build tools to their display status
const std::map<std::string, std::string> BUILD_TOOLS = {
    {"cargo",   "Compiling (Rust)"},
    {"rustc",   "Compiling (Rust)"},
    {"gcc",     "Compiling (C)"},
    {"g++",     "Compiling (C++)"},
    {"clang",   "Compiling (Clang)"},
    {"clang++", "Compiling (Clang)"},
    {"make",    "Building (Make)"},
    {"cmake",   "Configuring"},
    {"npm",     "Bundling (Node)"},
    {"node",    "Running (Node)"},
    {"gradle",  "Building (Gradle)"},
    {"javac",   "Compiling (Java)"},
    {"python3", "Running (Python)"},
    {"go",      "Building (Go)"},
};

std::string get_proc_cmdline(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline");
    std::string line, result;
    if (std::getline(f, line)) {
        // cmdline uses null bytes as delimiters
        for (char c : line)
            result += (c == '\0') ? ' ' : c;
    }
    return result;
}

std::string get_proc_cwd(int pid) {
#ifdef _WIN32
    return ""; // Not implemented for Windows
#else
    std::string link = "/proc/" + std::to_string(pid) + "/cwd";
    char buf[512];
    ssize_t len = readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (len != -1) {
        buf[len] = '\0';
        return std::string(buf);
    }
    return "";
#endif
}

std::vector<BuildJob> scan_processes() {
    std::vector<BuildJob> jobs;
    std::map<std::string, BuildJob> seen; // deduplicate by cwd

    std::error_code ec;
    if (!fs::exists("/proc", ec)) {
        return jobs;
    }

    for (auto& entry : fs::directory_iterator("/proc", ec)) {
        if (ec) continue;
        std::string name = entry.path().filename().string();

        // Only look at numeric entries (PIDs)
        if (!std::all_of(name.begin(), name.end(), ::isdigit)) continue;

        int pid = std::stoi(name);
        std::string cmdline = get_proc_cmdline(pid);
        if (cmdline.empty()) continue;

        // Check if the command matches a build tool
        for (auto& [tool, status] : BUILD_TOOLS) {
            if (cmdline.find(tool) != std::string::npos) {
                std::string cwd = get_proc_cwd(pid);
                std::string project = cwd.empty() ? "unknown"
                    : fs::path(cwd).filename().string();

                // Simulate progress (replace with real parsing for Approach 2)
                float fake_progress = 0.3f + (pid % 60) / 100.0f;
                fake_progress = std::min(fake_progress, 0.99f);

                if (seen.find(project) == seen.end()) {
                    seen[project] = {project, tool, status, fake_progress, pid};
                }
                break;
            }
        }
    }

    for (auto& [k, v] : seen)
        jobs.push_back(v);

    return jobs;
}

// ─────────────────────────────────────────────
// CPU Usage
// Reads /proc/stat for total CPU utilization
// ─────────────────────────────────────────────

float get_cpu_usage() {
#ifdef _WIN32
    return 0.0f; // Not implemented for Windows
#else
    static long prev_idle = 0, prev_total = 0;

    std::ifstream f("/proc/stat");
    std::string line;
    if (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cpu;
        long user, nice, system, idle, iowait, irq, softirq;
        ss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq;

        long total = user + nice + system + idle + iowait + irq + softirq;
        long diff_idle  = idle - prev_idle;
        long diff_total = total - prev_total;

        prev_idle  = idle;
        prev_total = total;

        if (diff_total == 0) return 0.0f;
        return 100.0f * (1.0f - (float)diff_idle / diff_total);
    }
    return 0.0f;
#endif
}

// ─────────────────────────────────────────────
// Progress Bar Renderer
// ─────────────────────────────────────────────

Element render_progress_bar(float progress, int width = 30) {
    int filled = static_cast<int>(progress * width);
    std::string bar = "[";
    for (int i = 0; i < width; i++)
        bar += (i < filled) ? "█" : "░";
    bar += "] ";
    bar += std::to_string((int)(progress * 100)) + "%";
    return text(bar) | color(progress > 0.8f ? Color::Green : Color::Yellow);
}

// ─────────────────────────────────────────────
// Autocomplete Helpers
// ─────────────────────────────────────────────

void autocomplete_path(std::string& input, int& cursor_pos) {
    fs::path p(input);
    fs::path dir = p.parent_path().empty() ? "." : p.parent_path();
    std::string prefix = p.filename().string();
    if (input.empty() || input.back() == '/' || input.back() == '\\') {
        dir = input.empty() ? "." : input;
        prefix = "";
    }
    
    std::error_code ec;
    std::string best_match = "";
    if (fs::exists(dir, ec) && fs::is_directory(dir, ec)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            std::string name = entry.path().filename().string();
            std::string name_lower = name;
            std::string prefix_lower = prefix;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            std::transform(prefix_lower.begin(), prefix_lower.end(), prefix_lower.begin(), ::tolower);
            
            if (name_lower.rfind(prefix_lower, 0) == 0) {
                best_match = entry.path().string();
                if (fs::is_directory(entry.path(), ec)) {
                    best_match += "/";
                }
                break;
            }
        }
    }
    
    if (!best_match.empty()) {
#ifdef _WIN32
        std::replace(best_match.begin(), best_match.end(), '\\', '/');
#endif
        if (best_match.substr(0, 2) == "./" && input.substr(0, 2) != "./") {
            best_match = best_match.substr(2);
        }
        input = best_match;
        cursor_pos = (int)input.size();
    }
}

void autocomplete_cmd(std::string& input, int& cursor_pos) {
    if (input.find('/') != std::string::npos || input.find('\\') != std::string::npos) {
        autocomplete_path(input, cursor_pos);
        return;
    }
    
    std::vector<std::string> known_cmds = {"cargo", "npm", "make", "gcc", "g++", "clang", "cmake", "node", "python3", "go", "buildmon"};
    std::string best = "";
    std::string input_lower = input;
    std::transform(input_lower.begin(), input_lower.end(), input_lower.begin(), ::tolower);

    for (const auto& c : known_cmds) {
        if (c.rfind(input_lower, 0) == 0) {
            best = c;
            break;
        }
    }
    if (!best.empty()) {
        input = best + " ";
        cursor_pos = (int)input.size();
        return;
    }
    autocomplete_path(input, cursor_pos);
}

// ─────────────────────────────────────────────
// Main — TUI Dashboard
// ─────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--help" || arg == "-h" || arg == "help") {
            printf("Buildm-on - Terminal Build Monitor\n\n");
            printf("Usage:\n");
            printf("  buildm-on                  Start passive monitoring (Scanner Mode)\n");
            printf("  buildm-on run <cmd>        Run a build command and parse exact progress (Wrapper Mode)\n");
            printf("  buildm-on --help, -h       Show this help message\n");
            printf("  buildm-on --version, -v    Show version information\n");
            return 0;
        }
        if (arg == "--version" || arg == "-v" || arg == "version") {
            printf("Buildm-on v1.0\n");
            return 0;
        }
    }

    bool wrapper_mode = (argc > 1 && std::string(argv[1]) == "run");
    std::string wrapper_cmd = "";
    std::string wrapper_tool = "";
    if (wrapper_mode && argc > 2) {
        wrapper_tool = argv[2];
        for (int i = 2; i < argc; i++) {
            wrapper_cmd += argv[i];
            if (i < argc - 1) wrapper_cmd += " ";
        }
        wrapper_cmd += " 2>&1";
    } else if (wrapper_mode) {
        printf("Usage: buildm-on run <command>\n");
        return 1;
    }

    auto screen = ScreenInteractive::Fullscreen();
    Config cfg = load_config();

    std::vector<BuildJob> jobs;
    std::vector<BuildJob> history_jobs;
    float cpu_usage = 0.0f;
    std::mutex data_mutex;
    int uptime_seconds = 0;
    bool is_running = true;

    // Build output log
    std::deque<std::string> output_lines;
    int output_scroll_y = 0;
    const int MAX_OUTPUT_LINES = 500;

    // Get local IP for display & discovery
    std::string pc_ip = "127.0.0.1";
#ifdef _WIN32
    system("ipconfig > ip.txt");
    std::ifstream ip_file("ip.txt");
    std::string line;
    std::vector<std::string> ips;
    while (std::getline(ip_file, line)) {
        if (line.find("IPv4 Address") != std::string::npos) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                std::string ip = line.substr(pos + 2);
                // Remove any trailing whitespace
                ip.erase(ip.find_last_not_of(" \n\r\t") + 1);
                // Prioritize 192.168.x.x or 10.x.x.x
                if (ip.find("192.168.") == 0 || ip.find("10.") == 0 || ip.find("172.") == 0) {
                    pc_ip = ip;
                    break; 
                }
                ips.push_back(ip);
            }
        }
    }
    if (pc_ip == "127.0.0.1" && !ips.empty()) pc_ip = ips[0];
    ip_file.close();
#endif
    
    start_server();
    g_discovery.start(8766, "BUILDMON_DISCOVERY:" + pc_ip);

    // Define the execution logic as a lambda so it can be called from multiple places
    auto run_command_logic = [&](std::string cmd_to_run, std::string tool_name, std::string project_name) {
        std::thread runner([&, cmd_to_run, tool_name, project_name] {
#ifdef _WIN32
            FILE* pipe = _popen(cmd_to_run.c_str(), "r");
#else
            FILE* pipe = popen(cmd_to_run.c_str(), "r");
#endif
            if (!pipe) return;

            char buf[512];
            std::regex cargo_rx(R"((\d+)/(\d+))"); // More permissive [1/100]
            std::regex webpack_rx(R"((\d+)%)");
            std::regex make_rx(R"(\[\s*(\d+)%\])");
            std::regex ready_rx(R"(ready in|Ready in|Network:|Finished|Success|Done|listening on)");
            
            BuildJob w_job = {project_name, tool_name, "Running", 0.0f, 0, 0};
            
            // Add job to list immediately so it appears even if quiet
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                bool found = false;
                for (auto& j : jobs) {
                    if (j.project == project_name && j.tool == tool_name) {
                        found = true; break;
                    }
                }
                if (!found) jobs.push_back(w_job);
            }
            screen.PostEvent(Event::Custom);

            while (fgets(buf, sizeof(buf), pipe)) {
                std::string line(buf);
                // Strip trailing \r\n
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();

                std::smatch match;
                try {
                    if (std::regex_search(line, match, cargo_rx)) {
                        float current = std::stof(match[1].str());
                        float total = std::stof(match[2].str());
                        if (total > 0) w_job.progress = current / total;
                        w_job.status = "Compiling";
                    } else if (std::regex_search(line, match, webpack_rx)) {
                        w_job.progress = std::stof(match[1].str()) / 100.0f;
                        w_job.status = "Bundling";
                    } else if (std::regex_search(line, match, make_rx)) {
                        w_job.progress = std::stof(match[1].str()) / 100.0f;
                        w_job.status = "Building";
                    } else if (std::regex_search(line, match, ready_rx)) {
                        w_job.progress = 1.0f;
                        w_job.status = "Ready/Watching";
                    }
                } catch (...) {}

                {
                    std::lock_guard<std::mutex> lock(data_mutex);
                    // Update or add this job to the list
                    bool found = false;
                    for (auto& j : jobs) {
                        if (j.project == project_name && j.tool == tool_name) {
                            j.progress = w_job.progress;
                            j.status = w_job.status;
                            found = true;
                            break;
                        }
                    }
                    if (!found) jobs.push_back(w_job);
                    cpu_usage = get_cpu_usage();
                    // Append to output log
                    output_lines.push_back(line);
                    if ((int)output_lines.size() > MAX_OUTPUT_LINES)
                        output_lines.pop_front();
                    // Auto-scroll to bottom
                    output_scroll_y = (int)output_lines.size();
                }
                send_ipc_update(w_job);
                screen.PostEvent(Event::Custom);
            }
#ifdef _WIN32
            _pclose(pipe);
#else
            int exit_code = pclose(pipe);
            if (exit_code != 0) w_job.status = "Failed";
            else w_job.status = "Finished";
#endif
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                for (auto it = jobs.begin(); it != jobs.end(); ) {
                    if (it->project == project_name && it->tool == tool_name) {
                        it->status = w_job.status;
                        it->progress = 1.0f;
                        history_jobs.insert(history_jobs.begin(), *it);
                        if (history_jobs.size() > 5) history_jobs.pop_back();

                        bool success = (w_job.status == "Finished" || w_job.status == "Ready/Watching");
                        std::string error_line = success ? "" : "Build command failed"; 
                        broadcast_event(success ? "finished" : "failed", *it, success, error_line);

                        it = jobs.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (cfg.notifications) send_notification(project_name + " Build", "Status: " + w_job.status);
            }
            screen.PostEvent(Event::Custom);
        });
        runner.detach();
    };

    if (wrapper_mode) {
        run_command_logic(wrapper_cmd, wrapper_tool, "wrapped");
    } else {
        std::thread scanner([&] {
            while (true) {
                auto new_jobs = scan_processes();
                float new_cpu  = get_cpu_usage();

                {
                    std::lock_guard<std::mutex> lock(data_mutex);
                    if (!is_running) break;
                    
                    // 1. Identify jobs that are in 'jobs' but NOT in 'new_jobs'
                    // If they have a real PID (scanned jobs), they are finished.
                    // Manual jobs (pid == 0) stay until they finish themselves.
                    for (auto it = jobs.begin(); it != jobs.end(); ) {
                        bool found_in_scan = false;
                        if (it->pid != 0) { // Only check scanned jobs
                            for (const auto& nj : new_jobs) {
                                if (it->project == nj.project && it->pid == nj.pid) {
                                    found_in_scan = true;
                                    break;
                                }
                            }
                        } else {
                            found_in_scan = true; // Keep manual jobs
                        }

                        if (!found_in_scan) {
                            BuildJob fin = *it;
                            fin.status = "Finished";
                            fin.progress = 1.0f;
                            history_jobs.insert(history_jobs.begin(), fin);
                            if (history_jobs.size() > 5) history_jobs.pop_back();
                            if (cfg.notifications) send_notification("Build Finished", "Project: " + fin.project);
                            
                            broadcast_event("finished", fin, true, "");
                            
                            it = jobs.erase(it);
                        } else {
                            ++it;
                        }
                    }

                    // 2. Add/Update jobs from the scan
                    for (const auto& nj : new_jobs) {
                        bool existing = false;
                        for (auto& oj : jobs) {
                            if (oj.pid == nj.pid && oj.project == nj.project) {
                                oj.progress = nj.progress;
                                oj.status = nj.status;
                                existing = true;
                                break;
                            }
                        }
                        if (!existing) jobs.push_back(nj);
                    }

                    cpu_usage = new_cpu;
                }

                screen.PostEvent(Event::Custom); // trigger re-render
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
        scanner.detach();

#ifndef _WIN32
        std::thread ipc_server([&] {
            int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (server_fd < 0) return;
            struct sockaddr_un addr;
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, "/tmp/buildm-on.sock", sizeof(addr.sun_path) - 1);
            unlink("/tmp/buildm-on.sock");
            if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) return;
            listen(server_fd, 5);
            
            while (is_running) {
                // Use a timeout or poll in real app to allow clean exit, blocking accept for simplicity here
                int client = accept(server_fd, NULL, NULL);
                if (client < 0) continue;
                char buf[512];
                int n = read(client, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    std::string payload(buf);
                    auto pos1 = payload.find('|');
                    auto pos2 = payload.find('|', pos1 + 1);
                    auto pos3 = payload.find('|', pos2 + 1);
                    if (pos1 != std::string::npos && pos2 != std::string::npos && pos3 != std::string::npos) {
                        std::string proj = payload.substr(0, pos1);
                        std::string t = payload.substr(pos1 + 1, pos2 - pos1 - 1);
                        std::string stat = payload.substr(pos2 + 1, pos3 - pos2 - 1);
                        float prog = std::stof(payload.substr(pos3 + 1));
                        
                        std::lock_guard<std::mutex> lock(data_mutex);
                        bool found = false;
                        for (auto& j : jobs) {
                            if (j.project == proj) {
                                j.progress = prog; j.status = stat; found = true; break;
                            }
                        }
                        if (!found) jobs.push_back({proj, t, stat, prog, -1});
                    }
                }
                close(client);
                screen.PostEvent(Event::Custom);
            }
            close(server_fd);
            unlink("/tmp/buildm-on.sock");
        });
        ipc_server.detach();

        std::thread inotify_thread([&] {
            int fd = inotify_init();
            if (fd < 0) return;
            int wd = inotify_add_watch(fd, "./target", IN_CREATE | IN_MODIFY);
            if (wd < 0) wd = inotify_add_watch(fd, "./build", IN_CREATE | IN_MODIFY);
            if (wd < 0) { close(fd); return; }

            char buf[4096];
            while (is_running) {
                // In a real app we would use non-blocking read or poll to check is_running
                int length = read(fd, buf, sizeof(buf));
                if (length > 0) {
                    std::lock_guard<std::mutex> lock(data_mutex);
                    bool found = false;
                    for (auto& job : jobs) {
                        if (job.project == "local_dir") {
                            job.progress = std::min(job.progress + 0.05f, 0.99f);
                            job.status = "Building artifacts";
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        jobs.push_back({"local_dir", "inotify", "Building artifacts", 0.05f, -1});
                    }
                }
                screen.PostEvent(Event::Custom);
            }
            close(fd);
        });
        inotify_thread.detach();
#endif
    }

    std::thread uptime_tracker([&] {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::vector<BuildJob> current_jobs_copy;
            float current_cpu;
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                if (!is_running) break;
                uptime_seconds++;
                for (auto& job : jobs) {
                    job.duration_seconds++;
                }
                current_jobs_copy = jobs;
                current_cpu = cpu_usage;
            }
            broadcast_update(current_jobs_copy, current_cpu);
            screen.PostEvent(Event::Custom);
        }
    });
    uptime_tracker.detach();

    // ── Pastel palette ──────────────────────────────────────
    // Soft lavender for primary elements
    const Color C_LAVENDER   = Color(179, 157, 219); // #B39DDB
    // Soft mint/teal for accents
    const Color C_MINT       = Color(128, 203, 196); // #80CBC4
    // Soft peach for labels
    const Color C_PEACH      = Color(255, 183, 139); // #FFB78B
    // Soft pink for highlights
    const Color C_PINK       = Color(240, 157, 181); // #F09DB5
    // Muted green for success
    const Color C_SAGE       = Color(149, 204, 141); // #95CC8D
    // Soft yellow for warnings
    const Color C_BUTTER     = Color(255, 220, 130); // #FFDC82
    // Muted red for errors
    const Color C_ROSE       = Color(239, 154, 154); // #EF9A9A
    // Dim gray for secondary text
    const Color C_MUTED      = Color(150, 150, 160);
    // Background for header and footer
    const Color C_DARK_BG    = Color(30, 28, 38);    // deep dark purple
    // Border color
    const Color C_BORDER     = C_LAVENDER;

    auto quit = screen.ExitLoopClosure();

    // ── Quit button ─────────────────────────────────────────
    auto quit_btn = Button(" Quit(q) ", [&] {
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            is_running = false;
        }
        quit();
    }, ButtonOption::Animated(Color(80,60,120), C_LAVENDER, Color(100,80,150), Color::White));

    std::string dir_input_str = "";
    int dir_cursor = 0;

    std::string cmd_input_str = "";
    int cmd_cursor = 0;

    auto run_action = [&] {
        if (cmd_input_str.empty()) return;

        std::string tool = "build";
        auto pos = cmd_input_str.find(' ');
        if (pos != std::string::npos) tool = cmd_input_str.substr(0, pos);
        else tool = cmd_input_str;

        std::string proj = "custom";
        if (!dir_input_str.empty()) {
            fs::path p(dir_input_str);
            if (p.has_filename()) {
                proj = p.filename().string();
            } else if (p.has_parent_path()) {
                proj = p.parent_path().filename().string();
            }
        }

        std::string final_cmd = cmd_input_str + " 2>&1";
        if (!dir_input_str.empty()) {
            std::string d = dir_input_str;
            if (d.find(' ') != std::string::npos || d.find('!') != std::string::npos) {
                d = "\"" + d + "\"";
            }
#ifdef _WIN32
            final_cmd = "cd /d " + d + " && " + final_cmd;
#else
            final_cmd = "cd " + d + " && " + final_cmd;
#endif
        }

        {
            std::lock_guard<std::mutex> lock(data_mutex);
            output_lines.clear();
            output_scroll_y = 0;
        }
        run_command_logic(final_cmd, tool, proj);
        cmd_input_str = "";
        screen.PostEvent(Event::Custom);
    };

    // ── Run button ──────────────────────────────────────────
    auto run_btn = Button("  Run  ", run_action,
        ButtonOption::Animated(Color(60,120,80), C_SAGE, Color(80,160,100), Color::White));

    // ── Directory input ─────────────────────────────────────
    InputOption dir_opt = InputOption::Default();
    dir_opt.cursor_position = &dir_cursor;
    dir_opt.on_enter = [&] { /* move focus to cmd */ screen.PostEvent(Event::TabReverse); };
    auto dir_input = Input(&dir_input_str, "e.g. /path/to/project", dir_opt);
    auto dir_input_caught = CatchEvent(dir_input, [&](Event e) {
        if (e == Event::Tab) {
            autocomplete_path(dir_input_str, dir_cursor);
            screen.PostEvent(Event::Custom);
            return true;
        }
        return false;
    });

    // ── Command input ────────────────────────────────────────
    InputOption cmd_opt = InputOption::Default();
    cmd_opt.cursor_position = &cmd_cursor;
    cmd_opt.on_enter = run_action;
    auto cmd_input = Input(&cmd_input_str, "e.g. npm run build", cmd_opt);
    auto cmd_input_caught = CatchEvent(cmd_input, [&](Event e) {
        if (e == Event::Tab) {
            autocomplete_cmd(cmd_input_str, cmd_cursor);
            screen.PostEvent(Event::Custom);
            return true;
        }
        return false;
    });

    // ── Layout container ─────────────────────────────────────
    auto layout = Container::Vertical({
        dir_input_caught,
        cmd_input_caught,
        Container::Horizontal({
            run_btn,
            quit_btn,
        })
    });

    // ── Renderer ─────────────────────────────────────────────
    auto renderer = Renderer(layout, [&]() -> Element {
        std::lock_guard<std::mutex> lock(data_mutex);

        // Format uptime
        int h = uptime_seconds / 3600;
        int m = (uptime_seconds % 3600) / 60;
        int s = uptime_seconds % 60;
        char uptime_str[16];
        snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d:%02d", h, m, s);

        // ── Header ───────────────────────────────────────────
        auto header = hbox({
            text(" "),
            text("BuildM-on") | bold | color(C_LAVENDER),
            text(" "),
            text(wrapper_mode ? "(Wrapper)" : "v0.1.1") | color(C_MUTED),
            filler(),
            text("Active builds: ") | color(C_MUTED),
            text(std::to_string(jobs.size())) | bold | color(C_MINT),
            text("  "),
        });

        // ── Input section ─────────────────────────────────────
        auto input_section = vbox({
            hbox({
                text(" Directory: ") | color(C_PEACH) | size(WIDTH, EQUAL, 13),
                dir_input_caught->Render() | flex | color(C_MINT),
            }) | xflex,
            hbox({
                text(" Command:   ") | color(C_PEACH) | size(WIDTH, EQUAL, 13),
                cmd_input_caught->Render() | flex | color(C_BUTTER),
            }) | xflex,
            hbox({
                filler(),
                run_btn->Render() | color(C_SAGE),
                text(" "),
                quit_btn->Render() | color(C_LAVENDER),
                text(" "),
            }),
        });

        // ── Build output log (scrollable) ────────────────────
        Elements log_lines;
        if (output_lines.empty()) {
            log_lines.push_back(
                text("  Waiting for build output...") | color(C_MUTED)
            );
        } else {
            // Show last N lines that fit
            for (const auto& l : output_lines) {
                // Colour lines that contain error keywords
                Color lc = Color::White;
                std::string ll = l;
                std::transform(ll.begin(), ll.end(), ll.begin(), ::tolower);
                if (ll.find("error") != std::string::npos ||
                    ll.find("failed") != std::string::npos)
                    lc = C_ROSE;
                else if (ll.find("warn") != std::string::npos)
                    lc = C_BUTTER;
                else if (ll.find("ok") != std::string::npos ||
                         ll.find("success") != std::string::npos ||
                         ll.find("finish") != std::string::npos)
                    lc = C_SAGE;
                log_lines.push_back(text(" " + l) | color(lc));
            }
        }

        auto output_section = vbox(std::move(log_lines)) | yframe | flex;

        // ── Active build jobs ─────────────────────────────────
        Elements job_elements;
        if (jobs.empty()) {
            job_elements.push_back(
                text("  No active builds  ") | color(C_MUTED)
            );
        } else {
            for (auto& job : jobs) {
                Color scolor = (job.progress >= 1.0f) ? C_SAGE : C_BUTTER;
                int dur_m = job.duration_seconds / 60;
                int dur_s = job.duration_seconds % 60;
                char dur_buf[16];
                snprintf(dur_buf, sizeof(dur_buf), "%02d:%02d", dur_m, dur_s);

                job_elements.push_back(
                    hbox({
                        text(" "),
                        text("●") | color(scolor),
                        text(" "),
                        text(job.project) | bold | color(C_LAVENDER),
                        text(" [") | color(C_MUTED),
                        text(job.tool) | color(C_MINT),
                        text("] ") | color(C_MUTED),
                        text(job.status) | color(scolor),
                        filler(),
                        render_progress_bar(job.progress),
                        text("  "),
                        text(dur_buf) | color(C_MUTED),
                        text(" "),
                    })
                );
            }
        }

        // ── History ───────────────────────────────────────────
        Elements hist_elements;
        if (!history_jobs.empty()) {
            hist_elements.push_back(separatorLight());
            hist_elements.push_back(text("  Recent:") | color(C_MUTED));
            for (const auto& hj : history_jobs) {
                Color s_color = (hj.status == "Failed") ? C_ROSE : C_SAGE;
                hist_elements.push_back(
                    hbox({
                        text("   " + hj.project),
                        text(" [") | color(C_MUTED),
                        text(hj.tool) | color(C_MINT),
                        text("] ") | color(C_MUTED),
                        text(hj.status) | color(s_color),
                    })
                );
            }
        }

        // ── CPU colour ────────────────────────────────────────
        Color cpu_color = cpu_usage > 80.0f ? C_ROSE
                        : cpu_usage > 50.0f ? C_BUTTER
                        : C_SAGE;

        // ── Footer ────────────────────────────────────────────
        auto footer = hbox({
            text(" CPU: ") | color(C_MUTED),
            text(std::to_string((int)cpu_usage) + "%") | color(cpu_color),
            text("  Uptime: ") | color(C_MUTED),
            text(uptime_str) | color(C_LAVENDER),
            text("  IP: ") | color(C_MUTED),
            text(pc_ip) | color(C_MINT),
            filler(),
            text("[Enter] Run  [Tab] Complete  [↑↓] Scroll  [q] Quit ") | color(C_MUTED),
        });

        // Helper: rounded box
        // FTXUI doesn't have a built-in rounded border style constant,
        // so we implement it with a custom BorderStyle struct.
        // We use the available 'border' decorator and add corner chars via
        // a custom approach using borderStyled.
        Decorator rounded_box = [C_BORDER](Element e) {
            return e | borderStyled(ROUNDED) | color(C_BORDER);
        };

        // ── Full layout ───────────────────────────────────────
        return vbox({
            // Title bar
            header | bgcolor(C_DARK_BG),

            separatorStyled(ROUNDED) | color(C_BORDER),

            // Inputs
            input_section | rounded_box,

            separatorStyled(ROUNDED) | color(C_BORDER),

            // Output label + scrollable output
            hbox({
                vbox({
                    hbox({
                        text(" Build output") | bold | color(C_MINT),
                        filler(),
                        text(std::to_string(output_lines.size()) + " lines ") | color(C_MUTED),
                    }),
                    separatorStyled(LIGHT) | color(C_BORDER),
                    output_section,
                }) | rounded_box | flex,

                text(" "),

                // Active jobs sidebar
                vbox({
                    text(" Jobs ") | bold | color(C_PEACH),
                    separatorStyled(LIGHT) | color(C_BORDER),
                    vbox(job_elements) | flex,
                    vbox(hist_elements),
                }) | size(WIDTH, EQUAL, 52) | rounded_box,
            }) | flex,

            separatorStyled(ROUNDED) | color(C_BORDER),

            // Footer
            footer | bgcolor(C_DARK_BG),
        });
    });

    // ── Key handling ──────────────────────────────────────────
    auto with_keys = CatchEvent(renderer, [&](Event e) {
        // Quit
        if (e == Event::Character('q')) {
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                is_running = false;
            }
            quit();
            return true;
        }
        // Scroll output up
        if (e == Event::ArrowUp) {
            std::lock_guard<std::mutex> lock(data_mutex);
            if (output_scroll_y > 0) output_scroll_y--;
            screen.PostEvent(Event::Custom);
            return true;
        }
        // Scroll output down
        if (e == Event::ArrowDown) {
            std::lock_guard<std::mutex> lock(data_mutex);
            if (output_scroll_y < (int)output_lines.size())
                output_scroll_y++;
            screen.PostEvent(Event::Custom);
            return true;
        }
        return false;
    });

    screen.Loop(with_keys);
    return 0;
}
