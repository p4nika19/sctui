# SoundCloud TUI (`sctui`)

A high-performance, responsive, and visually polished Terminal User Interface (TUI) client for SoundCloud written in modern C++23.

```
╭──────────────────────────────────────────────────────────────────────────────╮
│  ☁ SOUNDCLOUD TUI │ 🔍 Search tracks, artists... │ [?] Help  [q] Exit        │
├─────────┬────────────────────────────────────────────────────────────────────┤
│   NAV   │  #  TITLE                          ARTIST       TIME               │
│ ─────── │ ─── ────────────────────────────── ──────────── ────               │
│ ▶ Search│  1  The Chillwave Tape Collection  Meredith J   05:28              │
│   Stream│  2  Chillwave                      LSPLASH      03:51              │
│   Likes │  3  Afterlife (Remix)              Ark Patrol   02:03              │
│   Queue │                                                                    │
├─────────┴────────────────────────────────────────────────────────────────────┤
│  ♫  The Chillwave Tape Collection • Meredith J     ▂▃▄▅▆▇█  🔊 75%           │
│ 01:24 ━━━━━━━━━━━━━━━━━━━━━╸━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 05:28       │
│  ▶ PLAYING  │ 🔀 SHUF │ 🔁 ALL │  [Space] Play/Pause [h/l] ±5s [+/-] Vol     │
╰──────────────────────────────────────────────────────────────────────────────╯
```

---

## 🚀 Key Architectural Features

- **Toolchain & Extreme Optimization Profile**:
  - Compiler: **LLVM Clang 18+ / 23+** enforcing strict C++23 diagnostics (`-Wall -Wextra -Wpedantic -Wshadow -D_FORTIFY_SOURCE=2`).
  - Linker: **`mold`** (`-fuse-ld=mold`) with Thin-LTO / full Link-Time Optimization (`-flto`).
  - Hardware Acceleration: `-march=native -mtune=native` with vectorization hints (`-fvectorize -funroll-loops`) and zero-cost floating point optimizations (`-fno-math-errno -fno-trapping-math -fomit-frame-pointer`).
- **Decoupled Concurrency Model**:
  - **Main / UI Thread**: Dedicated strictly to non-blocking FTXUI DOM rendering, terminal resize handling (`SIGWINCH`), and event dispatch.
  - **Worker Thread Pool**: Asynchronous API fetching, live query debounce, progressive audio stream resolution, and metadata parsing.
  - **Audio Engine Thread**: Low-latency `miniaudio` playback engine running hardware device callbacks, RMS level tracking, and 16-band real-time spectrum analysis.
- **Lock-Free Communication**:
  - Cache-line padded lock-free **Single-Producer Single-Consumer (SPSC)** queue for zero-latency audio engine commands.
  - Thread-safe **Bounded Channel** for asynchronous UI mutations.
  - Zero global locks and zero mutex contention in the audio and rendering hot loops.
- **Robust Error Handling**:
  - Fully typed error architecture with C++23 `std::expected` / `std::unexpected`.
  - Zero unhandled exceptions.
  - Automatic fallback between dynamic web bundle client ID scraping and hardcoded client credentials.

---

## 📦 Dependencies

Managed via `xrepo` / system packages:
- **FTXUI** (`v7.0.3`): Functional Terminal User Interface engine.
- **miniaudio** (`0.11.25`): Lightweight low-latency audio device & decoder backend.
- **libcurl**: High-throughput multi-protocol network client.
- **simdjson** / **nlohmann_json**: High-speed JSON parsing.

---

## 🛠️ Building & Running

### Requirements
- Linux (x86_64)
- `clang++` (LLVM 18+)
- `mold` linker
- `xmake` (v2.8+)
- `libcurl`, `alsa`, `pulseaudio` development headers

### Build
```bash
# Configure toolchain with Clang and mold
xmake f -m release --toolchain=clang --ld="mold"

# Build sctui binary
xmake build sctui

# Run test suite
xmake run test_suite

# Launch sctui
xmake run sctui
```

---

## ⌨️ Vim-Inspired Keybindings

| Key | Action |
|:---:|:---|
| **`j` / `k`** or **`↓` / `↑`** | Move selection down / up |
| **`Enter`** | Play selected track / Activate item |
| **`Space`** | Toggle Play / Pause |
| **`/`** | Focus Search input bar |
| **`Tab` / `Shift+Tab`** | Cycle active panel (Search → Sidebar → Tracks → Player → Queue) |
| **`h` / `l`** or **`←` / `→`** | Seek backward / forward 5 seconds |
| **`H` / `L`** | Seek backward / forward 30 seconds |
| **`+` / `-`** | Increase / decrease volume (perceptual log gain) |
| **`m`** | Toggle Mute |
| **`s`** | Toggle Shuffle mode |
| **`r`** | Cycle Repeat mode (`Off` → `One` → `All`) |
| **`a`** | Add selected track to end of queue |
| **`A`** | Play selected track next (queue after current) |
| **`l`** | Toggle Favorite / Like |
| **`J` / `K`** | Reorder track down / up in Queue view |
| **`d` / `x`** | Remove selected track from Queue |
| **`C`** | Clear entire queue |
| **`1` .. `5`** | Switch views (Search, Stream, Likes, Playlists, Queue) |
| **`?`** | Open / Close Keyboard Cheat Sheet modal |
| **`q` / `Ctrl+C`** | Gracefully quit and restore terminal state |

---

## ⚙️ Configuration File

Configuration is automatically stored in `~/.config/soundcloud-tui/config.json`:

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
    "cache_dir": "~/.cache/soundcloud-tui"
}
```

*Note: If `client_id` is left empty, `sctui` will automatically extract an active client ID directly from SoundCloud's web client assets.*
