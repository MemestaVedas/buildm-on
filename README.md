# Buildm-on 🚀

A real-time terminal build monitor in C++ with FTXUI. Buildm-on detects and tracks builds happening on your system, whether they are initiated via compilers like `gcc`, package managers like `npm`, or build tools like `cargo` and `make`.

![Buildm-on Dashboard Mockup](https://raw.githubusercontent.com/ArthurSonzogni/FTXUI/master/doc/FTXUI_v5.0.gif) *(Example FTXUI Output)*

## Features ✨

* **Multi-Tab Interactive Dashboard**: Seamlessly switch between the live dashboard, build launcher, history, and help menus.
* **Mobile Connectivity**: Sync build progress to your mobile device over WiFi. Supports automatic discovery via UDP.
* **Interactive Build Launcher**: Start new builds directly from within the TUI. Specify directory and command, and monitor output in real-time.
* **Zero-Setup Process Scanning**: Automatically discovers active builds running anywhere on the OS by scanning `/proc`.
* **Persistent Build History**: Keeps track of your build history across sessions, saved to `~/.buildm-on_history.json`.
* **Desktop Notifications**: Uses `notify-send` on Linux to alert you when builds finish.
* **Enhanced System Stats**: Live telemetry for CPU, RAM usage, and Network (Up/Down) speeds.

## Requirements 🐧

This application natively leverages specific Linux APIs:
- `/proc` (for discovering running processes and system stats)
- `inotify` (for file-system watcher mode)
- `WebSockets` (for mobile app syncing)
- `UDP Broadcast` (for mobile discovery)

**Operating System**: Linux (native) or WSL2.
**Dependencies**:
* A C++17 compliant compiler (`g++` 11+ or `clang`)
* CMake (3.14+)
* `nlohmann-json` and `FTXUI` (automatically pulled by CMake)

## Installation 🛠️

1. **Clone the Repository**:
   ```bash
   git clone https://github.com/yourusername/buildm-on.git
   cd buildm-on
   ```

2. **Configure and Build**:
   ```bash
   mkdir build && cd build
   cmake ..
   cmake --build .
   ```

3. **Install (Optional)**:
   ```bash
   sudo cmake --install .
   ```

## Usage 🏃

Launch Buildm-on from your terminal:
```bash
buildm-on
```

### Navigation & Shortcuts
- **1-4**: Switch between Dashboard, Launcher, History, and Help tabs.
- **Tab**: Cycle through input fields in the "Run Command" tab.
- **Enter**: Start a build in the Launcher.
- **q**: Quit the application.

### Strategy 1: Passive Monitoring (Scanner)
In the **Dashboard** tab, Buildm-on automatically detects any builds started in other terminal windows (e.g., if you run `cargo build` elsewhere).

### Strategy 2: Interactive Launch (Launcher)
Switch to the **Run Command** tab to start a build directly. Enter the project directory and the build command. Buildm-on will spawn the process and track its progress visually.

## Mobile Access 📱

Buildm-on includes a built-in WebSocket server and UDP discovery broadcase to sync with mobile companion apps.

- **WebSocket Port**: 8765
- **Discovery Port**: 8766
- **Status Overlay**: The top bar of the TUI shows "Mobile: Active" when a device is connected.

## Configuration ⚙️

Customize behavior via `~/.buildm-on.toml`:

```toml
theme = "ocean"        # Options: default, ocean, matrix
notifications = true   # Toggle notify-send alerts
```

## Contributing 🤝

Contributions, issues, and feature requests are welcome! Feel free to check the issues page.
