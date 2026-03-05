# Buildm-on 🚀

A real-time terminal build monitor in C++ with FTXUI. Buildm-on detects and tracks builds happening on your system, whether they are initiated via compilers like `gcc`, package managers like `npm`, or build tools like `cargo` and `make`.

![Buildm-on Demo Screenshot](https://raw.githubusercontent.com/ArthurSonzogni/FTXUI/master/doc/FTXUI_v5.0.gif) *(Example FTXUI Output)*

## Features ✨

* **Zero-Setup Process Scanning**: Reads `/proc` to instantly discover and display active builds running anywhere on the OS.
* **Wrapper Mode**: Prefix your build command with `buildm-on run <cmd>` to capture real `stdout` progress percentages via regex parsing.
* **Build History**: Tracks the last 5 completed or failed builds.
* **Desktop Notifications**: Uses `notify-send` on Linux to alert you when builds finish.
* **Themes**: Supports color themes via `~/.buildm-on.toml` config file.
* **Live System Stats**: Displays constant uptime and overall CPU usage.

## Requirements 🐧

This application natively leverages specific Linux APIs:
- `/proc` (for discovering running process commands and current working directories)
- `inotify` (for file-system watcher mode)
- `UNIX Domain Sockets` (for the IPC server to aggregate daemonized statistics)

**Operating System**: Linux natively, or Windows Subsystem for Linux (WSL).
**Dependencies**:
* A C++17 compliant compiler (`g++` 11+ or `clang`)
* CMake (3.14+)

## Installation 🛠️

If you are on Windows, ensure you have WSL installed and are running these commands inside a Linux terminal (like Ubuntu on WSL).

1. **Clone or Download the Repository**:
   Navigate to the directory where you saved `buildm-on` (`d:\Kushal\projects\build-on\buildm-on`).

2. **Configure the Project**:
   ```bash
   mkdir build && cd build
   cmake ..
   ```
   *(Note: The first time you run this, CMake will automatically download the FTXUI library, which may take a minute).*

3. **Compile**:
   ```bash
   cmake --build .
   ```

4. **Install Globally (Optional but Recommended)**:
   Add the compiled `buildm-on` binary to your system PATH so you can run it from any terminal.

   **Windows (PowerShell)**:
   We included an install script for you. Run this from the root of the project:
   ```powershell
   .\install.ps1
   ```
   *(Restart your terminal afterward to apply the new PATH)*

   **Linux / WSL / macOS**:
   Use CMake's built-in install target:
   ```bash
   sudo cmake --install .
   ```

## Usage 🏃

### Strategy 1: Passive Monitoring (Scanner Mode)
Simply launch `buildm-on`. It will automatically scan your system every second for active build tools.
```bash
buildm-on
```
While Buildm-on is running, open another terminal window and start a build (like `npm run build` or `cargo build`). Buildm-on will instantly detect it.

### Strategy 2: Active Monitoring (Wrapper Mode)
If you want accurate progress percentages instead of heuristics, run your build command through Buildm-on:
```bash
buildm-on run cargo build
```
Buildm-on will spawn the command, intercept the output to parse progress bars, and display the real-time exact progress.

## Configuration (Optional) ⚙️

Create a file named `.buildm-on.toml` in your home directory (`~/.buildm-on.toml`) to customize Buildm-on:

```toml
theme = "ocean"        # Options: default, ocean, matrix
notifications = true   # Set to false to disable notify-send alerts
```

## Contributing 🤝

Contributions, issues, and feature requests are welcome! Feel free to check the issues page.
