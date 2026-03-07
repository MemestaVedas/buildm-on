// plugin_manager.hpp — Lua plugin system for Buildm-on
// Plugins live in ~/.buildm-on/plugins/*.lua
// Lifecycle hooks: on_build_start, on_build_end, on_error, on_output_line
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>
#include <cstdlib>

#include "error_parser.hpp"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────
// Plugin Descriptor
// ─────────────────────────────────────────────

struct Plugin {
    std::string name;
    std::string path;
    std::string description;
    bool enabled = true;
    std::string last_status;  // "ok", "error: ..."
    std::time_t last_run = 0;
    std::vector<std::string> triggers;  // "build_start", "build_end", "error", "output_line"
};

// ─────────────────────────────────────────────
// Build context passed to plugins
// ─────────────────────────────────────────────

struct PluginBuildCtx {
    std::string command;
    std::string project;
    std::string tool;
    std::string status;   // "started", "finished", "failed"
    int duration_s = 0;
    std::vector<ParsedError> errors;
    std::string current_line; // for on_output_line
};

// ─────────────────────────────────────────────
// Minimal Lua runner (no external deps required)
// We use a simple subprocess approach: write a temp script and execute lua5.x
// This avoids embedding Lua as a C++ dependency while still enabling Lua plugins
// ─────────────────────────────────────────────

class PluginManager {
public:
    std::vector<Plugin> plugins;
    std::mutex mtx;

    // Discover all .lua plugins in the plugins directory
    void load_plugins() {
        std::lock_guard<std::mutex> lk(mtx);
        plugins.clear();
        std::string dir = get_plugin_dir();
        std::error_code ec;
        if (!fs::exists(dir, ec)) return;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".lua") {
                Plugin p;
                p.name    = entry.path().stem().string();
                p.path    = entry.path().string();
                p.enabled = !is_disabled(p.name);
                p.last_status = "not run";
                p.triggers = get_triggers(p.path);
                p.description = read_description(p.path);
                plugins.push_back(p);
            }
        }
    }

    // Install a plugin from a file path
    bool install(const std::string& src_path) {
        std::string dir = get_plugin_dir();
        fs::create_directories(dir);
        fs::path src(src_path);
        fs::path dst = fs::path(dir) / src.filename();
        std::error_code ec;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
        load_plugins();
        return true;
    }

    void enable(const std::string& name)  { set_disabled(name, false); reload(name); }
    void disable(const std::string& name) { set_disabled(name, true);  reload(name); }

    // Fire a lifecycle event for all matching plugins
    void fire(const std::string& event, const PluginBuildCtx& ctx) {
        std::vector<Plugin*> targets;
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (auto& p : plugins) {
                if (!p.enabled) continue;
                bool match = p.triggers.empty(); // empty = all
                for (auto& t : p.triggers) if (t == event) { match = true; break; }
                if (match) targets.push_back(&p);
            }
        }
        for (auto* p : targets) {
            std::thread([this, p, event, ctx]() {
                run_plugin(*p, event, ctx);
            }).detach();
        }
    }

    static std::string get_plugin_dir() {
        const char* home = getenv("HOME");
        return home ? std::string(home) + "/.buildm-on/plugins" : ".buildm-on/plugins";
    }

    // Create the built-in example plugins if they don't already exist
    static void ensure_builtin_plugins() {
        std::string dir = get_plugin_dir();
        fs::create_directories(dir);
        write_if_missing(dir + "/webhook-notifier.lua", WEBHOOK_PLUGIN);
        write_if_missing(dir + "/sound-alerts.lua",     SOUND_PLUGIN);
    }

private:
    void run_plugin(Plugin& p, const std::string& event, const PluginBuildCtx& ctx) {
        // Build a small driver script that injects the context and calls the handler
        std::string driver = build_driver(p.path, event, ctx);
        std::string tmp = "/tmp/buildm_plugin_runner_XXXXXX.lua";
        // write driver to temp file
        char tmp_path[256];
        std::snprintf(tmp_path, sizeof(tmp_path), "/tmp/buildm_plugin_%s_%s.lua", p.name.c_str(), event.c_str());
        {
            std::ofstream f(tmp_path);
            if (!f.is_open()) return;
            f << driver;
        }

        // Run via lua interpreter (prefer lua5.4, then lua5.3, then lua)
        std::string cmd = "(lua5.4 " + std::string(tmp_path) + " 2>&1"
                        + " || lua5.3 " + std::string(tmp_path) + " 2>&1"
                        + " || lua " + std::string(tmp_path) + " 2>&1) ; rm -f " + std::string(tmp_path);
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            std::lock_guard<std::mutex> lk(mtx);
            p.last_status = "error: cannot exec lua";
            p.last_run = std::time(nullptr);
            return;
        }
        std::string output;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) output += buf;
        pclose(pipe);

        std::lock_guard<std::mutex> lk(mtx);
        p.last_run = std::time(nullptr);
        p.last_status = output.empty() ? "ok" : ("ok: " + output.substr(0, 80));
    }

    // Build a Lua driver that provides the build context and HTTP/shell/notify API stubs,
    // then calls the plugin's handler function
    static std::string build_driver(const std::string& plugin_path, const std::string& event, const PluginBuildCtx& ctx) {
        std::string s;
        // Inject API stubs
        s += R"(
-- Buildm-on plugin API
http = {}
function http.post(url, data)
    local body = ""
    if type(data) == "table" then
        for k, v in pairs(data) do body = body .. k .. "=" .. tostring(v) .. "&" end
    else
        body = tostring(data)
    end
    os.execute('curl -sf -X POST ' .. url .. ' -d "' .. body:gsub('"', '\\"') .. '" >/dev/null 2>&1 &')
end
function http.get(url)
    local f = io.popen('curl -sf "' .. url .. '" 2>/dev/null')
    local r = f and f:read("*a") or ""
    if f then f:close() end
    return r
end

shell = {}
function shell.exec(cmd)
    local f = io.popen(cmd .. " 2>&1")
    local r = f and f:read("*a") or ""
    if f then f:close() end
    return r
end

ui = {}
function ui.show_banner(msg) io.write("[plugin] " .. msg .. "\n") end
function ui.append_log(msg)  io.write("[plugin] " .. msg .. "\n") end
)";

#ifdef _WIN32
        s += R"lua(
notify = {}
function notify.desktop(title, body)
    local ps = 'powershell.exe -w hidden -Command "Add-Type -AssemblyName System.Windows.Forms; ' ..
               '[System.Windows.Forms.MessageBox]::Show(\'' .. string.gsub((body or ""), "'", "''") .. '\', \'' .. string.gsub(title, "'", "''") .. '\')"'
    os.execute(ps)
end
function notify.sound(path)
    os.execute('powershell -w hidden -c "(new-object System.Media.SoundPlayer \'' .. path .. '\').PlaySync()"')
end
)lua";
#else
        s += R"lua(
notify = {}
function notify.desktop(title, body)
    os.execute('notify-send "' .. title .. '" "' .. (body or "") .. '" &')
end
function notify.sound(path)
    os.execute('paplay "' .. path .. '" 2>/dev/null || aplay "' .. path .. '" 2>/dev/null &')
end
)lua";
#endif

        // Inject build context object
        s += "build = {\n";
        s += "  command  = \"" + lua_escape(ctx.command) + "\",\n";
        s += "  project  = \"" + lua_escape(ctx.project) + "\",\n";
        s += "  tool     = \"" + lua_escape(ctx.tool) + "\",\n";
        s += "  status   = \"" + lua_escape(ctx.status) + "\",\n";
        s += "  duration = " + std::to_string(ctx.duration_s) + ",\n";
        s += "  error_count = " + std::to_string(ctx.errors.size()) + ",\n";
        s += "  errors = {\n";
        for (auto& e : ctx.errors) {
            s += "    { file=\"" + lua_escape(e.file) + "\", line=" + std::to_string(e.line) +
                 ", message=\"" + lua_escape(e.message) + "\", severity=\"" + severity_str(e.severity) + "\" },\n";
        }
        s += "  },\n";
        s += "  line = \"" + lua_escape(ctx.current_line) + "\"\n";
        s += "}\n\n";

        // Load plugin file
        s += "-- Load plugin\n";
        s += "dofile(\"" + lua_escape(plugin_path) + "\")\n\n";

        // Call the hook
        std::string fn = "on_" + event;
        s += "-- Invoke hook\n";
        s += "if type(" + fn + ") == \"function\" then\n";
        s += "  local ok, err = pcall(" + fn + ", build)\n";
        s += "  if not ok then io.stderr:write(\"plugin error: \" .. tostring(err) .. \"\\n\") end\n";
        s += "end\n";

        return s;
    }

    static std::string lua_escape(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\' || c == '\n') out += '\\';
            out += c;
        }
        return out;
    }

    static std::vector<std::string> get_triggers(const std::string& path) {
        std::vector<std::string> triggers;
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("on_build_start") != std::string::npos)    triggers.push_back("build_start");
            if (line.find("on_build_end")   != std::string::npos)    triggers.push_back("build_end");
            if (line.find("on_error")       != std::string::npos)    triggers.push_back("error");
            if (line.find("on_output_line") != std::string::npos)    triggers.push_back("output_line");
        }
        return triggers;
    }

    static std::string read_description(const std::string& path) {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("-- @description") != std::string::npos) {
                auto pos = line.find(":");
                if (pos != std::string::npos) return line.substr(pos + 2);
            }
        }
        return "";
    }

    static bool is_disabled(const std::string& name) {
        std::string path = get_plugin_dir() + "/.disabled";
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) if (line == name) return true;
        return false;
    }

    static void set_disabled(const std::string& name, bool disable) {
        std::string path = get_plugin_dir() + "/.disabled";
        std::vector<std::string> lines;
        { std::ifstream f(path); std::string l; while (std::getline(f, l)) if (!l.empty()) lines.push_back(l); }
        if (disable) { if (!is_disabled(name)) lines.push_back(name); }
        else { lines.erase(std::remove(lines.begin(), lines.end(), name), lines.end()); }
        std::ofstream f(path);
        for (auto& l : lines) f << l << "\n";
    }

    void reload(const std::string& name) {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& p : plugins) if (p.name == name) { p.enabled = !is_disabled(name); break; }
    }

    static void write_if_missing(const std::string& path, const std::string& content) {
        if (fs::exists(path)) return;
        std::ofstream f(path);
        f << content;
    }

    // ── Built-in plugin content ──────────────────────
    static constexpr const char* WEBHOOK_PLUGIN = R"lua(-- @description: Post build results to a Slack/Discord webhook
-- @triggers: on_build_end
-- Configure in ~/.buildm-on.toml: [plugins.webhook] url = "https://..."

local WEBHOOK_URL = os.getenv("BUILDM_WEBHOOK_URL") or ""

function on_build_end(build)
    if WEBHOOK_URL == "" then return end
    local icon = build.status == "failed" and ":x:" or ":white_check_mark:"
    local msg = icon .. " *" .. build.project .. "* — " ..
                build.status .. " in " .. build.duration .. "s"
    if build.error_count > 0 then
        msg = msg .. " (" .. build.error_count .. " errors)"
    end
    http.post(WEBHOOK_URL, {
        text = msg,
        username = "Buildm-on"
    })
end
)lua";

    static constexpr const char* SOUND_PLUGIN = R"lua(-- @description: Play sounds on build completion
-- @triggers: on_build_end
-- Configure via BUILDM_SOUND_SUCCESS / BUILDM_SOUND_FAILURE env vars

local SUCCESS_SND = os.getenv("BUILDM_SOUND_SUCCESS")
    or "/usr/share/sounds/freedesktop/stereo/complete.oga"
local FAILURE_SND = os.getenv("BUILDM_SOUND_FAILURE")
    or "/usr/share/sounds/freedesktop/stereo/dialog-error.oga"

function on_build_end(build)
    if build.status == "failed" then
        notify.sound(FAILURE_SND)
    else
        notify.sound(SUCCESS_SND)
    end
end
)lua";
};
