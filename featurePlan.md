# Buildm-on — Feature Roadmap 🚀

**Generated:** March 2026  
**Audience:** Solo Developers  
**Current Version:** v1.x (Linux, FTXUI, Kotlin Android companion)

---

## Executive Summary

Based on your answers, the roadmap is structured around four pillars in priority order:

1. **Better Error Analysis** — your #1 pain point is hard-to-debug failures
2. **Plugin Ecosystem (Lua/Python)** — extensibility without bloat
3. **Mobile Enhancements** — build on the existing Kotlin Android app
4. **CI/CD Integration** — GitHub Actions support
5. **Platform Expansion** — Windows native support (longer-term)

---

## Phase 1 — Error Intelligence Engine 🔍
*Priority: Highest | Estimated effort: Large*

This is your stated #1 pain point: **hard-to-debug failures**. The goal is to make Buildm-on the smartest error companion for Rust/Cargo, Node, and Bazel.

### 1.1 — Smart Error Parser & Highlighter
- Parse compiler output from `rustc`, `gcc`, `clang`, `tsc`, `eslint`, `bazel` and extract structured error objects (file, line, column, error code, message)
- Display errors in a dedicated **Errors tab** with syntax-highlighted snippets from the offending file
- Color-code by severity: error (red), warning (yellow), hint (cyan)
- Clickable error entries that open the file path in `$EDITOR` or print a `vim +LINE file` command

### 1.2 — Error Diffing Between Builds
- When the same build is run multiple times, diff the error list: show **new errors**, **resolved errors**, and **persisting errors**
- "Delta mode" view in the Errors tab: `[+3 new] [-1 fixed] [=5 unchanged]`
- Store structured error snapshots in `~/.buildm-on_history.json` alongside existing build records

### 1.3 — Build Failure Summary Panel
- On build failure, auto-open a compact summary overlay: error count, most frequent file, first error with file/line
- Add a `[Copy Errors]` shortcut key to copy all errors to clipboard for pasting into a chat or bug report
- Sound alert on failure (distinct from success tone — maps to your notification preference)

### 1.4 — Slow Build Visibility
- Track and display per-phase timing where possible: configure/link/compile breakdown for `cmake`, `cargo`, `bazel`
- Highlight phases that exceed a configurable threshold (e.g., > 30s) in orange
- "Bottleneck" annotation: label the slowest phase in the current build

---

## Phase 2 — Plugin & Scripting System 🧩
*Priority: High | Estimated effort: Large*

You want **Lua or Python scripting**. This makes Buildm-on infinitely customizable without requiring C++ knowledge.

### 2.1 — Plugin Runtime
- Embed **Lua 5.4** via `sol2` (header-only, CMake-friendly) as the primary scripting runtime
- Optional **Python 3** support via `pybind11` as a secondary runtime for users who prefer it
- Plugin directory: `~/.buildm-on/plugins/`
- Each plugin is a single `.lua` or `.py` file with a defined lifecycle: `on_build_start`, `on_build_end`, `on_error`, `on_output_line`

### 2.2 — Plugin API Surface
Expose the following to scripts:

```lua
-- Example Lua plugin: post to a webhook on failure
function on_build_end(build)
  if build.status == "failed" then
    http.post("https://hooks.slack.com/...", {
      text = "❌ Build failed: " .. build.command .. " (" .. build.duration .. "s)"
    })
  end
end
```

Core API objects: `build` (command, directory, status, duration, errors), `notify` (desktop, sound), `http` (get, post), `shell` (exec), `ui` (show_banner, append_log)

### 2.3 — Plugin Manager Tab
- New **Plugins** tab (tab 5) in the TUI listing installed plugins with enable/disable toggle
- Show plugin name, description, trigger events, last execution status
- `buildm-on plugin install <path>` CLI subcommand to install from a file or URL

### 2.4 — Built-in Plugin: Slack/Discord Webhook
- Ship a first-party `webhook-notifier.lua` plugin out of the box
- Configurable via `~/.buildm-on.toml`:
  ```toml
  [plugins.webhook]
  enabled = true
  url = "https://hooks.slack.com/services/..."
  on = ["failure", "success"]
  ```
- This replaces a hardcoded Slack feature and lets users wire any webhook

### 2.5 — Built-in Plugin: Sound Alerts
- Ship a `sound-alerts.lua` plugin using `paplay` / `aplay` on Linux and `PowerShell` beep on Windows
- Configurable: different sounds for success vs failure
- Satisfies your notification preference for sound alerts without bloating core

---

## Phase 3 — Mobile App Enhancements 📱
*Priority: Medium-High | Estimated effort: Medium*

Your Kotlin Android app already shows active builds, progress %, logs, and notifications. The additions below are high-value upgrades.

### 3.1 — Remote Build Control (Start/Stop)
- Extend the WebSocket protocol to accept **command messages** from the mobile client: `{"action": "kill", "pid": 1234}` and `{"action": "launch", "command": "cargo build", "dir": "/home/user/project"}`
- The desktop daemon validates and executes these, broadcasting status updates back
- Android UI: add a **Stop** button per active build card, and a **Quick Launch** FAB with recent commands

### 3.2 — Build History View
- Expose the `~/.buildm-on_history.json` data over the WebSocket as a paginated history API
- Android: new **History** tab showing past builds with status badge, duration, timestamp, command
- Tap a history entry to see its error log and output tail

### 3.3 — Error Details & Stack Traces
- Stream structured error objects (from Phase 1.1) over WebSocket in addition to raw log lines
- Android: in the build detail screen, show a parsed **Errors** section with file, line, message — not just raw log text
- Tappable error entries that copy `file:line` to clipboard

### 3.4 — Rich Push Notifications (FCM)
- Integrate Firebase Cloud Messaging on Android for true push notifications (even when app is backgrounded/killed)
- The desktop daemon sends a push via FCM HTTP API on build completion/failure
- Notification includes: build status, command name, duration, error count
- Requires a lightweight FCM relay server or direct HTTP POST from the daemon (no extra infrastructure if using FCM directly)

### 3.5 — Desktop Tray Icon (Linux + Windows)
- Use `libappindicator` on Linux and Win32 tray API on Windows for a system tray icon
- Tray icon changes color: green (idle), yellow (building), red (failed)
- Right-click menu: Show TUI, Open History, Quit
- Satisfies your desktop tray icon notification preference

---

## Phase 4 — CI/CD Integration: GitHub Actions 🔗
*Priority: Medium | Estimated effort: Medium*

### 4.1 — GitHub Actions Status Watcher
- Poll the GitHub API (`/repos/{owner}/{repo}/actions/runs`) for workflow runs on the current git repo
- Auto-detect the repo remote URL from `.git/config` in the current directory
- Show a **CI** section in the Dashboard tab: workflow name, status (queued/running/success/failure), branch, triggered by, duration

### 4.2 — GitHub Actions as "Virtual Builds"
- Surface remote GitHub Actions runs as build entries in Buildm-on's history and dashboard — indistinguishable from local builds visually
- Badge them with a `[GH]` tag to distinguish from local builds
- Click into a GH run to see live log streaming via GitHub's log streaming API

### 4.3 — PR Build Status Overlay
- When inside a git branch, show the PR's latest CI status as a small overlay badge in the top bar: `PR #42: ✅ passing` or `PR #42: ❌ 2 failing`
- Auth via a `GITHUB_TOKEN` in `~/.buildm-on.toml` or environment variable

---

## Phase 5 — Windows Native Support 🪟
*Priority: Medium (longer-term) | Estimated effort: Very Large*

You want Windows native support (not just WSL2). This is the most complex phase.

### 5.1 — Abstraction Layer for Linux APIs
- Wrap all `/proc`-based scanning behind a `ProcessScanner` interface with a Linux and Windows implementation
- Windows implementation uses `CreateToolhelp32Snapshot` / `EnumProcesses` for process discovery
- Replace `inotify` with `ReadDirectoryChangesW` for file-system watching on Windows

### 5.2 — Windows Build Detection
- Detect MSVC (`cl.exe`), MSBuild, `cargo`, `npm`, `cmake` processes on Windows via process enumeration
- Parse Windows process command lines to extract build tool identity

### 5.3 — Windows Notifications
- Replace `notify-send` with `Windows.UI.Notifications` (WinRT Toast API) via a small helper or the `wintoast` library
- Sound alerts via `PlaySound` Win32 API

### 5.4 — Windows Installer
- Package as an `.exe` installer via CPack/NSIS or WiX
- Add to Windows PATH automatically during install
- MSVC and MinGW-w64 build support via CMake presets

---

## Phase 6 — Dashboard Visualizations 📊
*Priority: Medium | Estimated effort: Medium*

You want all four visualization features. These are additive TUI improvements.

### 6.1 — Build Dependency Graph
- Parse `CMakeLists.txt`, `Cargo.toml`, `package.json`, `BUILD.bazel` to extract target dependencies
- Render as an ASCII DAG in a scrollable panel using FTXUI Canvas
- Highlight the currently-building target node in yellow, completed in green, failed in red

### 6.2 — Performance Flamechart
- For `cargo build` and `bazel build`, parse timing output (`--timings` flag for cargo, `--profile` for bazel)
- Render a horizontal bar flamechart in the TUI showing compilation unit timings
- Sorted by duration descending; scrollable

### 6.3 — Side-by-Side Build Comparison
- In the History tab, select two builds and press `C` to open a split-pane comparison
- Left/right panels show: duration, error count, output line count, exit code, timestamp
- Diff view on errors: which errors appeared/disappeared between the two runs

### 6.4 — Error Highlighting & Diffing
*(Covered in Phase 1.2 and 1.3 — the foundation is laid there, with the visual diff panels living here as the History tab feature)*

---

## Phase 7 — Build Tool Deep Support 🔧
*Priority: Medium | Estimated effort: Medium*

### 7.1 — Cargo (Rust) First-Class Support
- Parse `cargo build --message-format=json` output for structured diagnostics
- Extract: crate name, compile unit progress, warning/error count per crate
- Show a per-crate progress list during compilation

### 7.2 — Node/npm/yarn/bun Support
- Detect and parse output from `npm run build`, `yarn build`, `bun build`, `vite build`, `webpack`
- Extract bundle size stats and show as a mini bar chart in the Dashboard
- Detect common error patterns: TypeScript errors, ESLint violations, missing modules

### 7.3 — Bazel/Buck/Ninja Support
- Parse Bazel's structured event protocol (BEP) output for detailed target-level status
- Show target tree with per-target pass/fail status
- Ninja: parse `.ninja_log` for per-file build times

---

## Configuration Additions (`~/.buildm-on.toml`)

```toml
theme = "ocean"
notifications = true

[ci.github]
token = "ghp_..."
poll_interval_seconds = 30

[sound]
enabled = true
success = "/usr/share/sounds/freedesktop/stereo/complete.oga"
failure = "/usr/share/sounds/freedesktop/stereo/dialog-error.oga"

[tray]
enabled = true

[plugins]
directory = "~/.buildm-on/plugins"

[plugins.webhook]
enabled = false
url = ""
on = ["failure"]
```

---

## Suggested Release Milestones

| Version | Focus | Key Deliverables |
|---------|-------|-----------------|
| v1.5 | Error Intelligence | Smart parser, error diff, failure summary, slow build highlights |
| v2.0 | Plugin System | Lua runtime, plugin API, plugin manager tab, webhook + sound plugins |
| v2.1 | Mobile++ | Remote control, history sync, push notifications (FCM), error detail stream |
| v2.5 | CI Integration | GitHub Actions watcher, virtual builds, PR status overlay |
| v3.0 | Visualizations | Dependency graph, flamechart, side-by-side comparison |
| v4.0 | Windows Native | Win32 abstraction layer, Windows installer, MSVC support |

---

## Quick Wins (Ship in Days)

These are low-effort, high-impact improvements you could ship immediately:

- **`[Copy Errors]` keybind** — copies all errors to clipboard on build failure
- **Sound alerts** — call `paplay` on build end; 3 lines of code
- **Error count badge** in tab label: `Errors [5]`
- **Auto-scroll toggle** (`S` key) in the log view — stops auto-scroll when you scroll up manually
- **Build duration in history** — if not already shown, add HH:MM:SS formatting
- **`--no-tui` flag** — run as a background daemon only, useful for scripting

---

*Built with ❤️ for solo developers who care about their build experience.*