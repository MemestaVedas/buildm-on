#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/dom/elements.hpp>

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
    std::ifstream f(std::string(home) + "/.buildmon.toml");
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
};

void send_ipc_update(const BuildJob& job) {
#ifndef _WIN32
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return;
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/buildmon.sock", sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        std::string msg = job.project + "|" + job.tool + "|" + job.status + "|" + std::to_string(job.progress);
        write(sock, msg.c_str(), msg.size());
    }
    close(sock);
#endif
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
            printf("BuildMon - Terminal Build Monitor\n\n");
            printf("Usage:\n");
            printf("  buildmon                  Start passive monitoring (Scanner Mode)\n");
            printf("  buildmon run <cmd>        Run a build command and parse exact progress (Wrapper Mode)\n");
            printf("  buildmon --help, -h       Show this help message\n");
            printf("  buildmon --version, -v    Show version information\n");
            return 0;
        }
        if (arg == "--version" || arg == "-v" || arg == "version") {
            printf("BuildMon v1.0\n");
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
        printf("Usage: buildmon run <command>\n");
        return 1;
    }

    auto screen = ScreenInteractive::TerminalOutput();
    Config cfg = load_config();

    std::vector<BuildJob> jobs;
    std::vector<BuildJob> history_jobs;
    float cpu_usage = 0.0f;
    std::mutex data_mutex;
    int uptime_seconds = 0;
    bool is_running = true;

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
            std::regex cargo_rx(R"(\[(\d+)/(\d+)\])");
            std::regex webpack_rx(R"((\d+)%)");
            std::regex make_rx(R"(\[\s*(\d+)%\])");
            
            BuildJob w_job = {project_name, tool_name, "Running", 0.0f, 0};
            
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
            strncpy(addr.sun_path, "/tmp/buildmon.sock", sizeof(addr.sun_path) - 1);
            unlink("/tmp/buildmon.sock");
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
            unlink("/tmp/buildmon.sock");
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
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                if (!is_running) break;
                uptime_seconds++;
            }
            screen.PostEvent(Event::Custom);
        }
    });
    uptime_tracker.detach();

    auto quit = screen.ExitLoopClosure();
    auto quit_btn = Button("  Quit (q)  ", [&] {
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            is_running = false;
        }
        quit();
    });

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
            proj = fs::path(dir_input_str).filename().string();
        }

        std::string final_cmd = cmd_input_str + " 2>&1";
        if (!dir_input_str.empty()) {
            std::string d = dir_input_str;
            // Quote the path for safety
            if (d.find(' ') != std::string::npos || d.find('!') != std::string::npos) {
                d = "\"" + d + "\"";
            }
#ifdef _WIN32
            final_cmd = "cd /d " + d + " && " + final_cmd;
#else
            final_cmd = "cd " + d + " && " + final_cmd;
#endif
        }

        run_command_logic(final_cmd, tool, proj);
        cmd_input_str = ""; // Clear after run
        screen.PostEvent(Event::Custom);
    };

    auto run_btn = Button(" [ RUN ] ", run_action, ButtonOption::Animated(Color::Green, Color::Black, Color::GreenLight, Color::White));

    InputOption dir_opt = InputOption::Default();
    dir_opt.cursor_position = &dir_cursor;
    dir_opt.on_enter = run_action;
    auto dir_input = Input(&dir_input_str, "e.g. ./target", dir_opt);
    auto dir_input_caught = CatchEvent(dir_input, [&](Event e) {
        if (e == Event::Tab) {
            autocomplete_path(dir_input_str, dir_cursor);
            screen.PostEvent(Event::Custom);
            return true;
        }
        return false;
    });

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

    auto layout = Container::Vertical({
        dir_input_caught,
        cmd_input_caught,
        Container::Horizontal({
            run_btn,
            quit_btn
        })
    });

    auto renderer = Renderer(layout, [&]() -> Element {
        std::lock_guard<std::mutex> lock(data_mutex);

        Color primary_color = Color::Cyan;
        Color accent_color = Color::Green;
        Color bg_color = Color::Black;
        
        if (cfg.theme == "ocean") {
            primary_color = Color::BlueLight;
            accent_color = Color::Cyan;
        } else if (cfg.theme == "matrix") {
            primary_color = Color::Green;
            accent_color = Color::GreenLight;
        }

        // Format uptime
        int h = uptime_seconds / 3600;
        int m = (uptime_seconds % 3600) / 60;
        int s = uptime_seconds % 60;
        char uptime_str[16];
        snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d:%02d", h, m, s);

        // Build job elements
        Elements job_elements;
        if (jobs.empty()) {
            job_elements.push_back(
                text("  No active builds detected.") | color(Color::GrayDark)
            );
            job_elements.push_back(
                text("  Start a build (cargo build, npm run build, make...)") | color(Color::GrayDark)
            );
        } else {
            for (auto& job : jobs) {
                job_elements.push_back(separator());
                job_elements.push_back(
                    hbox({
                        text("  Project : ") | color(primary_color),
                        text(job.project) | bold
                    })
                );
                job_elements.push_back(
                    hbox({
                        text("  Tool    : ") | color(primary_color),
                        text(job.tool)
                    })
                );
                job_elements.push_back(
                    hbox({
                        text("  Status  : ") | color(primary_color),
                        text(job.status) | color(Color::Yellow)
                    })
                );
                job_elements.push_back(
                    hbox({ text("  "), render_progress_bar(job.progress) })
                );
                job_elements.push_back(text(""));
            }
        }

        // Render history
        Elements hist_elements;
        if (!history_jobs.empty()) {
            hist_elements.push_back(separator());
            hist_elements.push_back(text("  Recent History:") | color(Color::GrayDark));
            for (const auto& hj : history_jobs) {
                Color s_color = (hj.status == "Failed") ? Color::Red : Color::Green;
                hist_elements.push_back(
                    hbox({ text("  - " + hj.project + " [" + hj.tool + "] "), text(hj.status) | color(s_color) })
                );
            }
            hist_elements.push_back(text(""));
        }

        // Footer stats
        Color cpu_color = cpu_usage > 80.0f ? Color::Red
                        : cpu_usage > 50.0f ? Color::Yellow
                        : Color::Green;

        return vbox({
            // Header
            hbox({
                text(" BuildMon ") | bold | color(primary_color),
                text(wrapper_mode ? "(Wrapper Mode)" : "v1.0") | color(Color::GrayDark),
                filler(),
                text("Active Builds: ") | color(Color::GrayDark),
                text(std::to_string(jobs.size())) | bold | color(accent_color),
                text("  "),
            }) | bgcolor(bg_color),

            separator(),

            // Interactive Inputs
            vbox({
                hbox({
                    text(" Directory: ") | color(primary_color) | size(WIDTH, EQUAL, 12),
                    dir_input_caught->Render() | flex | borderLight | color(accent_color),
                }),
                hbox({
                    text(" Command:   ") | color(primary_color) | size(WIDTH, EQUAL, 12),
                    cmd_input_caught->Render() | flex | borderLight | color(accent_color),
                }),
                hbox({
                    filler(),
                    run_btn->Render(),
                    text("  "),
                    quit_btn->Render(),
                }),
            }),

            separator(),

            // Build jobs
            vbox(job_elements),
            vbox(hist_elements),

            separator(),

            // Footer
            hbox({
                text("  CPU : ") | color(Color::GrayDark),
                text(std::to_string((int)cpu_usage) + "%") | color(cpu_color),
                text("   Uptime : ") | color(Color::GrayDark),
                text(uptime_str),
                filler(),
                text(" [ Enter ] to Run   [ Tab ] to Complete   [ q ] to Quit "),
                text(" "),
            }),
        }) | border | color(primary_color);
    });

    // Allow 'q' to quit
    auto with_keys = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Character('q')) {
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                is_running = false;
            }
            quit();
            return true;
        }
        return false;
    });

    screen.Loop(with_keys);
    return 0;
}
