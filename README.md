# SoundCloud TUI (`sctui`)

A high-performance, responsive, and visually polished native Terminal User Interface (TUI) client for SoundCloud written in modern C++23.

```text
╭──────────────────────────────────────────────────────────────────────────────╮
│  ☁ SOUNDCLOUD TUI │ 🔍 Search tracks, artists... │ [?] Help  [q] Exit        │
├─────────┬────────────────────────────────────────────────────────────────────┤
│   NAV   │  #  TITLE                          ARTIST       TIME               │
│ ─────── │ ─── ────────────────────────────── ──────────── ────               │
│ ▶ Search│  1  The Chillwave Tape Collection  Meredith J   05:28              │
│   Stream│  2  Chillwave                      LSPLASH      03:51              │
│   Likes │  3  Afterlife (Remix) 💖           Ark Patrol   02:03              │
│   Playlists                                                                  │
│   Queue │                                                                    │
├─────────┴────────────────────────────────────────────────────────────────────┤
│  ♫  The Chillwave Tape Collection • Meredith J     ▂▃▄▅▆▇█  🔊 75%           │
│ 01:24 ━━━━━━━━━━━━━━━━━━━━━╸━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 05:28       │
│  ▶ PLAYING  │ 🔀 SHUF │ 🔁 ALL │  [Space] Play/Pause [h/l] ±5s [+/-] Vol     │
╰──────────────────────────────────────────────────────────────────────────────╯
```

---

## ⚡ Key Highlights & Architecture

- **High-Performance Native Toolchain**:
  - Compiler: **LLVM Clang** enforcing strict C++23 standards (`-std=c++23`).
  - Linker: **`mold`** (`-fuse-ld=mold`) with Thin-LTO (`-flto`).
  - Target Optimizations: `-march=native -mtune=native` with vectorization (`-fvectorize -funroll-loops`) and low-overhead floating point execution.
  - Toolchain rules and linker flags are embedded directly in `xmake.lua`—no redundant manual flags needed.
- **Decoupled Three-Tier Concurrency**:
  - **Main / UI Thread**: Dedicated strictly to non-blocking FTXUI DOM rendering, terminal resize handling (`SIGWINCH`), and user interaction.
  - **Worker Thread Pool**: Asynchronous API fetching, live query debounce, high-throughput metadata resolution, and account synchronization.
  - **Audio Engine Thread**: Low-latency `miniaudio` playback engine running device callbacks, volume scaling, and 16-band real-time spectrum analysis.
- **Lock-Free Communication**:
  - Cache-line padded lock-free **Single-Producer Single-Consumer (SPSC)** queue for zero-latency audio engine commands.
  - Thread-safe **Bounded Channel** for asynchronous UI events. Zero global locks in the playback hot loop.
- **Widevine DRM Engine**:
  - Built-in headless **Castlabs Electron** (`v44.9.3+wvcus`) satellite daemon to decrypt and stream protected CBC/CTR tracks.
  - Automated download and integrity checking during setup or launch when DRM is enabled.
- **Smart Subscription Awareness**:
  - Automatically identifies Free, GO, and GO+ subscription statuses via SoundCloud's user profile API.
  - Smoothly skips GO / GO+ previews when an account lacks the required subscription level.
- **Terminal Graphics & Cover Art**:
  - Native high-res album cover art support via **Kitty Graphics Protocol**, **iTerm2 Inline Images**, and half-block ANSI fallbacks.
  - Perfectly proportioned square covers calibrated for monospace terminal cell ratios.
- **System Integration**:
  - **MPRIS 2.2 D-Bus Server** for seamless desktop media keys and widget controls (`playerctl`).
  - **Discord Rich Presence** showcasing current track, artist, and playback progress.
  - Multi-browser cookie extraction (Firefox, Chrome, Chromium, Brave, Edge, Opera, Vivaldi) for effortless one-click authentication.

---

## 🚀 Quick Automated Installation

Run the universal installer script. It automatically detects your distribution's package manager, installs all build dependencies, bootstraps `xmake`, compiles the optimized release binary, sets up the DRM satellite, and installs `sctui` system-wide:

```bash
./install.sh
```

Supported package managers out of the box: `pacman` (Arch / Manjaro), `apt` (Debian / Ubuntu / Mint), `dnf` (Fedora / RHEL), `zypper` (openSUSE), `apk` (Alpine), `xbps-install` (Void), `emerge` (Gentoo), and `brew` (Linux / macOS).

Once finished, launch the app from any terminal with:

```bash
sctui
```

---

## 🛠️ Manual Building & Running

If you prefer building manually with `xmake`:

### 1. Requirements
- Linux (x86_64)
- `clang++` (LLVM 18+) & `mold` linker
- `xmake` (v2.8+)
- Development libraries: `libcurl`, `alsa`, `pulseaudio`, `systemd`, `sqlite3`, `openssl`
- Node.js & npm (v18+, for optional Widevine DRM satellite)

### 2. Build & Run
```bash
# Compile release binary (Clang & mold are automatically configured by xmake.lua)
xmake build sctui

# (Optional) Run the complete verification test suite
xmake run test_suite

# Launch sctui
xmake run sctui
```

---

## ⌨️ Vim-Inspired Keybindings

### Navigation & Global Controls
| Key | Action |
|:---:|:---|
| **`j` / `k`** or **`↓` / `↑`** | Move selection down / up |
| **`Enter`** | Play selected track / Open playlist |
| **`Space`** | Toggle Play / Pause |
| **`/`** | Focus global header Search input |
| **`Alt + /`** | Search & filter in real time inside **Likes** or **Playlists** tab |
| **`Esc`** | Exit search / Back to playlist view / Return focus to sidebar |
| **`Tab` / `Shift+Tab`** | Cycle active panel (Search → Sidebar → Tracks → Player → Queue) |
| **`f` / `F`** | Toggle Fullscreen playback mode |
| **`1` .. `7`** | Switch views (`1`: Search, `2`: Stream, `3`: Likes, `4`: Playlists, `5`: Queue, `6`: Account, `7`: Settings) |
| **`F5` / `R`** | Refresh current view |
| **`Ctrl + R`** | Force reload view from scratch |
| **`?`** | Open / close Keyboard Cheat Sheet modal |
| **`q`** | Gracefully quit application |

### Playback & Volume
| Key | Action |
|:---:|:---|
| **`h` / `l`** or **`←` / `→`** | Seek backward / forward 5 seconds |
| **`H` / `L`** | Seek backward / forward 30 seconds |
| **`n` / `>`** | Skip to next track |
| **`p` / `<`** | Go to previous track |
| **`+` / `-`** | Increase / decrease volume (5% steps, up to 150%) |
| **`m`** | Toggle Mute |
| **`s`** | Toggle Shuffle mode |
| **`r`** | Cycle Repeat mode (`Off` → `One` → `All`) |

### Queue & Library Management
| Key | Action |
|:---:|:---|
| **`a`** | Add selected track to end of queue |
| **`A` / `Ctrl+N`** | Queue selected track next (play right after current) |
| **`l`** | Toggle Like / Favorite (repeated like unlikes track) |
| **`J` / `K`** | Reorder track down / up in Queue view |
| **`d` / `x`** | Remove selected track from Queue |
| **`C`** | Clear entire queue |

---

## ⚙️ Configuration File

Configuration is saved in `~/.config/soundcloud-tui/config.json`:

```json
{
    "client_id": "",
    "auth_token": "",
    "volume": 75,
    "repeat_mode": 0,
    "shuffle": false,
    "search_limit": 20,
    "auto_extract_client_id": true,
    "accent_color": "#FF5500",
    "cache_dir": "~/.cache/soundcloud-tui",
    "mouse_control": false,
    "icon_style": 0,
    "fullscreen_visualizer_style": 0,
    "drm_engine_enabled": true,
    "download_drm_tracks_in_likes": false,
    "discord_rpc_enabled": true
}
```

- **`auth_token`**: Your SoundCloud OAuth token. Can be extracted automatically from your local browser cookies during the first-run onboarding wizard.
- **`drm_engine_enabled`**: Enables the Castlabs Electron Widevine daemon for playing DRM-protected SoundCloud tracks.
- **`fullscreen_visualizer_style`**: Fullscreen visualizer display mode: Stretched full width (`0`), Low frequencies centered & mirrored (`1`), or High frequencies centered & mirrored (`2`).
- **`icon_style`**: Selects between Emoji (`0`), Unicode Clean (`1`), or Default ASCII (`2`) symbols.
