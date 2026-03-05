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

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace ftxui;
namespace fs = std::filesystem;

// ─────────────────────────────────────────────
// Data Types
// ─────────────────────────────────────────────

struct BuildJob {
    std::string project;   // folder name of the process
    std::string tool;      // cargo, npm, gcc, etc.
    std::string status;    // Compiling, Bundling, Linking...
    float progress;        // 0.0 to 1.0
    int pid;
};

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
// Main — TUI Dashboard
// ─────────────────────────────────────────────

int main() {
    auto screen = ScreenInteractive::TerminalOutput();

    std::vector<BuildJob> jobs;
    float cpu_usage = 0.0f;
    std::mutex data_mutex;
    int uptime_seconds = 0;

    // Background thread: scans processes every second
    std::thread scanner([&] {
        while (true) {
            auto new_jobs = scan_processes();
            float new_cpu  = get_cpu_usage();

            {
                std::lock_guard<std::mutex> lock(data_mutex);
                jobs = new_jobs;
                cpu_usage = new_cpu;
                uptime_seconds++;
            }

            screen.PostEvent(Event::Custom); // trigger re-render
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
    scanner.detach();

    auto quit = screen.ExitLoopClosure();
    auto quit_btn = Button("  Quit (q)  ", quit);

    auto layout = Container::Vertical({ quit_btn });

    auto renderer = Renderer(layout, [&]() -> Element {
        std::lock_guard<std::mutex> lock(data_mutex);

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
                        text("  Project : ") | color(Color::Cyan),
                        text(job.project) | bold
                    })
                );
                job_elements.push_back(
                    hbox({
                        text("  Tool    : ") | color(Color::Cyan),
                        text(job.tool)
                    })
                );
                job_elements.push_back(
                    hbox({
                        text("  Status  : ") | color(Color::Cyan),
                        text(job.status) | color(Color::Yellow)
                    })
                );
                job_elements.push_back(
                    hbox({ text("  "), render_progress_bar(job.progress) })
                );
                job_elements.push_back(text(""));
            }
        }

        // Footer stats
        Color cpu_color = cpu_usage > 80.0f ? Color::Red
                        : cpu_usage > 50.0f ? Color::Yellow
                        : Color::Green;

        return vbox({
            // Header
            hbox({
                text(" BuildMon ") | bold | color(Color::Cyan),
                text("v1.0") | color(Color::GrayDark),
                filler(),
                text("Active Builds: ") | color(Color::GrayDark),
                text(std::to_string(jobs.size())) | bold | color(Color::Green),
                text("  "),
            }) | bgcolor(Color::Black),

            separator(),

            // Build jobs
            vbox(job_elements),

            separator(),

            // Footer
            hbox({
                text("  CPU : ") | color(Color::GrayDark),
                text(std::to_string((int)cpu_usage) + "%") | color(cpu_color),
                text("   Uptime : ") | color(Color::GrayDark),
                text(uptime_str),
                filler(),
                quit_btn->Render(),
                text(" "),
            }),
        }) | border;
    });

    // Allow 'q' to quit
    auto with_keys = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Character('q')) { quit(); return true; }
        return false;
    });

    screen.Loop(with_keys);
    return 0;
}
