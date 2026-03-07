// os_utils.hpp — OS-specific resource and process abstraction layer
#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#else
#include <fstream>
#include <sstream>
#include <unistd.h>
#endif

#include "error_parser.hpp"

namespace fs = std::filesystem;

struct BuildJob {
    std::string project;
    std::string tool;
    std::string status;
    float progress = 0.0f;
    int pid = 0;
    int duration_seconds = 0;
    time_t timestamp = 0;
    time_t start_time = 0;
    std::vector<std::string> log_lines;
    std::vector<ParsedError> errors;
    bool auto_scroll = true;
    int log_scroll_pos = 0;
};

struct SystemStats {
    float cpu = 0.0f;
    float ram = 0.0f;
};

struct NetStats {
    float down = 0.0f;
    float up = 0.0f;
};

class OSUtils {
public:
    static SystemStats get_system_stats() {
        SystemStats stats;
#ifdef _WIN32
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        if (GlobalMemoryStatusEx(&memInfo)) {
            stats.ram = 100.0f * (1.0f - (float)memInfo.ullAvailPhys / memInfo.ullTotalPhys);
        }

        static FILETIME prevSysIdle, prevSysKernel, prevSysUser;
        FILETIME sysIdle, sysKernel, sysUser;
        if (GetSystemTimes(&sysIdle, &sysKernel, &sysUser)) {
            ULARGE_INTEGER u_prevSysIdle, u_prevSysKernel, u_prevSysUser;
            u_prevSysIdle.LowPart = prevSysIdle.dwLowDateTime; u_prevSysIdle.HighPart = prevSysIdle.dwHighDateTime;
            u_prevSysKernel.LowPart = prevSysKernel.dwLowDateTime; u_prevSysKernel.HighPart = prevSysKernel.dwHighDateTime;
            u_prevSysUser.LowPart = prevSysUser.dwLowDateTime; u_prevSysUser.HighPart = prevSysUser.dwHighDateTime;

            ULARGE_INTEGER u_sysIdle, u_sysKernel, u_sysUser;
            u_sysIdle.LowPart = sysIdle.dwLowDateTime; u_sysIdle.HighPart = sysIdle.dwHighDateTime;
            u_sysKernel.LowPart = sysKernel.dwLowDateTime; u_sysKernel.HighPart = sysKernel.dwHighDateTime;
            u_sysUser.LowPart = sysUser.dwLowDateTime; u_sysUser.HighPart = sysUser.dwHighDateTime;

            ULONGLONG sysIdleDiff = u_sysIdle.QuadPart - u_prevSysIdle.QuadPart;
            ULONGLONG sysKernelDiff = u_sysKernel.QuadPart - u_prevSysKernel.QuadPart;
            ULONGLONG sysUserDiff = u_sysUser.QuadPart - u_prevSysUser.QuadPart;
            ULONGLONG sysTotalDiff = (sysKernelDiff + sysUserDiff);

            if (sysTotalDiff > 0 && prevSysUser.dwLowDateTime != 0) {
                stats.cpu = 100.0f * (sysTotalDiff - sysIdleDiff) / sysTotalDiff;
            }

            prevSysIdle = sysIdle;
            prevSysKernel = sysKernel;
            prevSysUser = sysUser;
        }
#else
        // Linux RAM
        std::ifstream fm("/proc/meminfo");
        std::string key;
        long total = 0, available = 0;
        while (fm >> key) {
            if (key == "MemTotal:") fm >> total;
            else if (key == "MemAvailable:") fm >> available;
        }
        if (total > 0) stats.ram = 100.0f * (1.0f - (float)available / total);

        // Linux CPU
        static long prev_idle = 0, prev_total = 0;
        std::ifstream fc("/proc/stat");
        std::string line;
        if (std::getline(fc, line)) {
            std::istringstream ss(line);
            std::string cpu;
            long u, n, s, i, iw, irq, si;
            if (ss >> cpu >> u >> n >> s >> i >> iw >> irq >> si) {
                long total_c = u + n + s + i + iw + irq + si;
                long diff_idle = i - prev_idle;
                long diff_total = total_c - prev_total;
                prev_idle = i; prev_total = total_c;
                if (diff_total > 0) {
                    stats.cpu = 100.0f * (1.0f - (float)diff_idle / diff_total);
                }
            }
        }
#endif
        return stats;
    }

    static NetStats get_net_stats() {
        NetStats stats;
#ifdef _WIN32
        // Simplification for Windows: returning 0 for now as GetIfTable requires Iphlpapi.lib and is complex
        // Can be implemented via Performance Counters or GetIfTable if strictly required.
        return stats;
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
        stats.down = std::max(0.0f, drx);
        stats.up = std::max(0.0f, dtx);
        return stats;
#endif
    }

    static std::vector<BuildJob> scan_processes() {
        std::vector<BuildJob> jobs;
        
        const std::map<std::string, std::string> BUILD_TOOLS = {
            {"cargo",   "Rust"}, {"npm",    "Node"}, {"make",  "Make"},
            {"gcc",     "C/C++"}, {"g++",  "C/C++"}, {"clang", "C/C++"},
            {"go",      "Go"}, {"cmake", "CMake"}, {"bazel",  "Bazel"},
            {"gradle",  "Gradle"}, {"mvn", "Maven"}, {"yarn",  "Node"},
            {"bun",     "Node"}, {"tsc",  "TypeScript"}, {"webpack", "Node"},
            // Windows native additions
            {"cl.exe", "MSVC"}, {"MSBuild.exe", "MSBuild"}, {"ninja", "Ninja"}
        };

#ifdef _WIN32
        HANDLE hProcessSnap;
        PROCESSENTRY32 pe32;
        hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hProcessSnap == INVALID_HANDLE_VALUE) return jobs;

        pe32.dwSize = sizeof(PROCESSENTRY32);
        if (!Process32First(hProcessSnap, &pe32)) {
            CloseHandle(hProcessSnap);
            return jobs;
        }

        do {
            std::string exeName = pe32.szExeFile;
            for (auto& [tool, label] : BUILD_TOOLS) {
                if (exeName.find(tool) != std::string::npos) {
                    // CWD extraction on Windows is tricky without injecting threads,
                    // we default to "unknown" or process executable directory.
                    BuildJob bj;
                    bj.project = "windows-proc"; 
                    bj.tool = tool; bj.status = "Active";
                    bj.progress = 0.5f; bj.pid = pe32.th32ProcessID;
                    bj.timestamp = std::time(nullptr);
                    bj.duration_seconds = 0;
                    jobs.push_back(bj);
                    break;
                }
            }
        } while (Process32Next(hProcessSnap, &pe32));
        CloseHandle(hProcessSnap);
#else
        std::error_code ec;
        if (!fs::exists("/proc", ec)) return jobs;
        for (auto& entry : fs::directory_iterator("/proc", ec)) {
            std::string name = entry.path().filename().string();
            if (!std::all_of(name.begin(), name.end(), ::isdigit)) continue;
            int pid = std::stoi(name);
            
            std::ifstream fc("/proc/" + name + "/cmdline");
            std::string cmd;
            if (std::getline(fc, cmd)) {
                for (char& c : cmd) { if (c == '\0') c = ' '; }
            }

            for (auto& [tool, label] : BUILD_TOOLS) {
                if (cmd.find(tool) != std::string::npos) {
                    char buf[512] = {0};
                    std::string proj = "unknown";
                    if (readlink(("/proc/" + name + "/cwd").c_str(), buf, sizeof(buf) - 1) != -1) {
                        proj = fs::path(buf).filename().string();
                    }
                    BuildJob bj;
                    bj.project = proj; bj.tool = tool; bj.status = "Active";
                    bj.progress = 0.5f; bj.pid = pid;
                    bj.timestamp = std::time(nullptr);
                    bj.duration_seconds = 0;
                    jobs.push_back(bj);
                    break;
                }
            }
        }
#endif
        return jobs;
    }
};
