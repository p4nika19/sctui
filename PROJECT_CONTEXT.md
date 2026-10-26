# SoundCloud TUI (`sctui`) - Comprehensive Project & Codebase Context

> **Target Audience**: Future AI coding agents, autonomous LLM tools (Claude Code, Aider, Cursor, Copilot, etc.), and human maintainers.  
> **Purpose**: Single source of truth detailing the architecture, codebase organization, data models, IPC protocols, build system, deployment mechanisms, edge cases, and development guidelines for `sctui`.

---

## 1. Executive Summary & Vision

**SoundCloud TUI (`sctui`)** is a high-performance, native Terminal User Interface (TUI) client for SoundCloud written in modern **C++23**. It is engineered for responsiveness, minimal latency, rich terminal visuals, and deep Linux desktop integration.

### Core Capabilities:
- **Streaming & Search**: High-throughput querying of SoundCloud tracks, playlists, stream, likes, and user profiles.
- **Audio Playback**: Low-latency playback powered by `miniaudio`, with 16-band real-time spectrum analysis, volume boosting up to 150%, and gapless state transitions.
- **Widevine DRM Support**: Integrated headless satellite daemon using **CastLabs Electron** (`v44.9.3+wvcus`) to decrypt and stream protected CBC/CTR and Go+ preview/full tracks.
- **Modern Terminal Visuals**: Terminal cover art rendering supporting Kitty Graphics Protocol, iTerm2 Inline Images, and half-block ANSI fallbacks, plus dynamic theme symbols (Emoji, Unicode, ASCII).
- **System Integrations**: Native MPRIS 2.2 D-Bus server for media key daemon integration (`playerctl`, desktop widgets) and Discord Rich Presence (RPC) via local IPC sockets.
- **Zero-Friction Auth**: Automatic extraction of SoundCloud OAuth tokens from local browser SQLite databases (Firefox, Chrome, Chromium, Brave, Edge, Opera, Vivaldi).

---

## 2. Tech Stack & Engineering Decisions

| Domain | Technology / Tool | Rationale & Configuration |
|---|---|---|
| **Language** | C++23 (`-std=c++23`) | Leverages `std::expected`, `std::jthread`, `std::format`, ranges, and modern language semantics. |
| **Compiler** | LLVM Clang 18+ | Enforces strict ISO C++ compliance, clean diagnostics, and aggressive optimization pipelines. |
| **Linker** | `mold` (`-fuse-ld=mold`) | Ultra-fast parallel linking, critical for rapid iteration and Thin-LTO linking. |
| **Build System** | `xmake` (Lua-based) | Modern, expressive package management (`xrepo`) and build scripting. Self-bootstrapping. |
| **TUI Framework** | FTXUI `v7.0.3` | Functional Terminal UI library with a declarative DOM model, responsive flexbox layout, and event bubbling. |
| **Audio Core** | `miniaudio 0.11.25` | Lightweight, single-header multi-backend audio engine (ALSA, PulseAudio, JACK) running in a dedicated real-time thread. |
| **JSON Parsers** | `simdjson v4.6.11` & `nlohmann_json v3.12.0` | `simdjson` for SIMD-accelerated high-throughput API parsing; `nlohmann_json` for ergonomic state & IPC serialization. |
| **HTTP & Networking**| `libcurl` | Robust HTTP/2 and HTTP/1.1 transport supporting DNS-over-HTTPS (DoH), custom resolvers, proxies, and streaming. |
| **DRM Daemon** | Castlabs Electron `44.9.3+wvcus` | Custom Chromium build with Google Widevine Verified Media Path (VMP) and CDM support for encrypted HLS streams. |
| **Database & Crypto**| `sqlite3`, `OpenSSL` | Used for local browser cookie decryption and token extraction. |
| **Image Decoding** | `stb_image` / `stb_image_resize2` | Header-only image loading and high-quality downsampling for terminal cover art rendering. |

---

## 3. Architecture & Concurrency Model

`sctui` utilizes a decoupled, three-tier asynchronous concurrency model to guarantee that the terminal UI never freezes, audio callbacks never drop samples, and network latencies never block user interaction.

```
┌────────────────────────────────────────────────────────────────────────┐
│                        MAIN / UI THREAD                                │
│  - FTXUI Event Loop & Screen Interactive Rendering                     │
│  - Terminal Resizes (SIGWINCH), Keyboard & Mouse Processing            │
│  - View State Machines (Search, Stream, Likes, Playlists, Settings)    │
└───────────────▲────────────────────────────────────────▲───────────────┘
                │ AsyncUIEvents                          │ Telemetry & State
                │ (BoundedChannel)                       │ (Atomics)
┌───────────────┴────────────────────────┐ ┌─────────────┴───────────────┐
│          WORKER THREAD POOL            │ │        AUDIO ENGINE          │
│  - SoundCloud Web API Requests (curl)  │ │  - Real-time Audio Callback │
│  - Browser Cookie Extraction           │ │  - miniaudio Pipeline       │
│  - Waveform & Artwork Downloader       │ │  - 16-Band FFT Visualizer   │
│  - Track Metadata & Cache Indexing     │ │  - Lock-Free SPSC Commands  │
└────────────────────────────────────────┘ └─────────────▲───────────────┘
                                                         │ NDJSON IPC (stdin/stdout)
                                           ┌─────────────┴───────────────┐
                                           │   DRM SATELLITE (Electron)  │
                                           │  - Widevine CDM Decryption  │
                                           │  - Headless Chromium Audio  │
                                           │  - Local HTTP Proxy Server  │
                                           └─────────────────────────────┘
```

1. **Main UI Thread**:
   - Executes `ftxui::ScreenInteractive::Loop()`.
   - Strictly handles DOM drawing and immediate input events.
   - Listens to incoming UI updates dispatched through a `BoundedChannel<AsyncUIEvent>`.
2. **Worker Thread Pool (`ThreadPool`)**:
   - Thread pool with dynamic task queues.
   - Executes asynchronous network I/O, SoundCloud API queries, image fetching, and cookie extraction.
   - Pushes UI notification events back to the UI thread via `AsyncUIEvent`.
3. **Audio Engine Thread (`AudioEngine`)**:
   - Manages the hardware audio output device (`miniaudio`).
   - Receives commands from the UI thread via a cache-line aligned lock-free **Single-Producer Single-Consumer (SPSC)** queue (`SPSCQueue<AudioCommand, 64>`).
   - Telemetry (playback position, state, buffer percentage, visualizer frequency bands, RMS volume) is exposed via lock-free atomics and read directly by the UI.
4. **DRM Satellite Process (`DrmSatelliteDaemon`)**:
   - Spawns as a detached child process running Castlabs Electron.
   - Communicates with C++ through non-blocking stdin/stdout using Newline-Delimited JSON (NDJSON).
   - Monitored by a parent PID heartbeat to prevent orphan/zombie processes.

---

## 4. Complete Repository Tree & File Manifest

```
sctui/
├── assets/                     # Artwork, banners, screenshots
├── include/
│   └── sctui/
│       ├── audio/
│       │   ├── audio_engine.hpp    # miniaudio wrapper, SPSC command dispatcher, visualizer
│       │   └── drm_daemon.hpp      # C++ controller for the Electron DRM satellite
│       ├── core/
│       │   ├── channel.hpp         # Thread-safe bounded communication channel
│       │   ├── config.hpp          # JSON configuration model, DoH, visualizer & icon settings
│       │   ├── cookie_extractor.hpp# Multi-browser SQLite cookie extraction (OAuth tokens)
│       │   ├── cover_art.hpp       # Kitty, iTerm2, and Half-Block terminal image rendering
│       │   ├── entropy.hpp         # High-entropy RNG combining /dev/urandom, RDRAND, jitter
│       │   ├── logger.hpp          # Thread-safe rotating logger with leveled output
│       │   ├── spsc_queue.hpp      # Lock-free, cache-line padded ring buffer
│       │   ├── thread_pool.hpp     # Dynamic worker thread pool
│       │   ├── track_cache.hpp     # Local track audio, cover, and metadata cache manager
│       │   └── types.hpp           # Error codes, Result<T> (std::expected), utility formats
│       ├── discord/
│       │   └── discord_rpc.hpp     # Discord Rich Presence IPC client
│       ├── models/
│       │   ├── state.hpp           # Global application state, queue, shuffle logic, navigation
│       │   ├── track.hpp           # Track, Transcoding, Playlist, User, AccessType models
│       │   └── user.hpp            # User profile, subscription status, badge models
│       ├── mpris/
│       │   └── mpris_server.hpp    # MPRIS 2.2 D-Bus media controller server
│       ├── net/
│       │   ├── http_client.hpp     # RAII libcurl wrapper (DoH, custom DNS, proxies, streams)
│       │   └── soundcloud_api.hpp  # Client-ID auto-resolver, search, likes, playlists, streams
│       └── ui/
│           ├── app.hpp             # Central application coordinator, event loop, view manager
│           ├── theme_symbols.hpp   # Emoji, Unicode Clean, and ASCII glyph definition tables
│           └── views/              # Specialized UI views (header, sidebar, list, player, etc.)
├── satellite/
│   ├── .npmrc                  # npm config enforcing allow-git=all
│   ├── hls.min.js              # Local HLS.js bundle for Widevine video/audio element
│   ├── main.js                 # Electron main process (IPC listener, HTTP local server, CDM)
│   ├── package.json            # Castlabs Electron dependency specification
│   ├── player.html             # Headless HTML5 player utilizing EME (audio-only keySystem)
│   └── preload.js              # Secure Electron IPC bridge for player window
├── src/
│   ├── audio/
│   │   ├── audio_engine.cpp    # miniaudio callbacks, spectrum FFT, lock-free telemetry
│   │   └── drm_daemon.cpp      # Satellite process management, pipe I/O, IPC parser
│   ├── core/
│   │   ├── config.cpp          # Config loader/migrator (~/.config/sctui/config.json)
│   │   ├── cookie_extractor.cpp# Chromium & Firefox SQLite cookie parsers
│   │   ├── cover_art.cpp       # STB image decoding, color quantization, terminal protocols
│   │   ├── logger.cpp          # Thread-safe file output (~/.config/sctui/sctui.log)
│   │   ├── thread_pool.cpp     # Thread pool worker implementation
│   │   └── track_cache.cpp     # Cache indexation, LRU eviction, like pinning
│   ├── discord/
│   │   └── discord_rpc.cpp     # Linux Unix Domain Socket IPC for Discord client
│   ├── main.cpp                # CLI entry point, argument parsing, signal & termios handlers
│   ├── models/
│   │   └── track.cpp           # Model serialization & transcoding helpers
│   ├── mpris/
│   │   └── mpris_server.cpp    # sd-bus / D-Bus implementation of org.mpris.MediaPlayer2
│   ├── net/
│   │   ├── http_client.cpp     # cURL configuration, SSL, DNS overrides
│   │   └── soundcloud_api.cpp  # Scraping client_id, v2 API requests, JSON deserialization
│   └── ui/
│       ├── app.cpp             # Event dispatching, view rendering, keybinding router
│       └── views/
│           ├── account_view.cpp    # User profile, follower count, uploaded tracks
│           ├── header_view.cpp     # Search bar, logo, status indicator, connection badges
│           ├── help_popup.cpp      # Vim keybinding cheatsheet modal dialog
│           ├── onboarding_view.cpp # First-run interactive setup & cookie extraction wizard
│           ├── player_view.cpp     # Visualizer, scrub bar, metadata, track access badges
│           ├── queue_view.cpp      # Interactive play queue with reordering and clearing
│           ├── settings_view.cpp   # Network, visualizer, cache, and theme settings
│           ├── sidebar_view.cpp    # Navigation sidebar (Search, Stream, Likes, etc.)
│           └── track_list_view.cpp # Scrollable track list, in-tab search, like status
├── tests/
│   └── test_suite.cpp          # Comprehensive unit & integration tests (32 test suites)
├── get.sh                      # Universal curl-to-bash one-line installer
├── install.sh                  # Multi-distro package detector, compiler, and installer
├── uninstall.sh                # Clean uninstaller (binaries, satellite, cache, configs)
├── todo                        # Developer scratchpad and upcoming roadmap
└── xmake.lua                   # Build definitions, compiler flags, and dependency rules
```

---

## 5. Detailed Subsystem Architecture

### 5.1. Audio Engine (`src/audio/audio_engine.cpp`)
- **Backend**: Built atop `miniaudio`. Manages audio pipeline in custom worker threads.
- **Lock-Free Communication**: UI thread posts `AudioCommand` items into `SPSCQueue<AudioCommand, 64>`. Zero mutex contention in the audio playback loop.
- **Spectrum Analyzer**: Calculates 16 frequency bands in real-time. Features logarithmic frequency binning with psychoacoustic weighting, dynamic AGC (automatic gain control), and exponential smoothing for fluid 60 FPS visualizer rendering in the TUI.
- **Volume Scaling**: Supports linear scaling from 0% up to 150%. Uses a calibrated logarithmic curve to preserve dynamic range and avoid clipping.
- **Playback States**: `Stopped`, `Loading`, `Buffering`, `Playing`, `Paused`, `Finished`, `Error`.

### 5.2. DRM Satellite Subsystem (`satellite/` & `src/audio/drm_daemon.cpp`)
SoundCloud uses Widevine DRM (CBC / CTR encryption) for select licensed tracks and Go+ previews. Standard open-source audio frameworks (FFmpeg, miniaudio) cannot decrypt Widevine streams without CDM binaries and Verified Media Path (VMP).
- **Solution**: A headless CastLabs Electron satellite runs in the background.
- **IPC Protocol**: Newline-Delimited JSON (NDJSON) over standard Unix pipes (`stdin` for commands, `stdout` for events, `stderr` for debug logs).
- **Parent Process Heartbeat**: The satellite checks parent PID existence every 1,000 ms via `process.kill(parentPid, 0)`. If `sctui` crashes or exits, the satellite terminates immediately, preventing zombie processes.
- **EME Configuration (`player.html`)**: Requests `com.widevine.alpha` specifically with `audioCapabilities: [{ contentType: 'audio/mp4; codecs="mp4a.40.2"' }, { contentType: 'audio/webm; codecs="opus"' }]` and explicit video exclusion to prevent Linux CDM `keySystemNoAccess` errors.
- **Automated Re-initialization**: Automatically triggers a one-time restart if the Widevine CDM requires an engine reload upon first component installation on Linux.

### 5.3. UI Layer & Event Loop (`src/ui/app.cpp` & views)
- **Framework**: FTXUI `v7.0.3`.
- **View Navigation Modes (`ViewMode`)**:
  - `1: Search`: Query tracks and playlists across SoundCloud.
  - `2: Stream`: User dashboard stream / newly released tracks from followed artists.
  - `3: Likes`: User's favorited tracks, with instant local filtering (`Alt + /`).
  - `4: Playlists`: User's created and liked playlists.
  - `5: Queue`: Active playback queue with live cursor, drag-like reordering (`J`/`K`), and clearing (`C`).
  - `6: Account`: User profile stats, bio, avatar, and uploaded tracks.
  - `7: Settings`: Audio visualizer styles, cache management, DoH configuration, icon themes, mouse toggle.
- **Navigation Shortcuts**: Full Vim-like keybindings (`j`/`k`, `/`, `Esc`, `Space`, `h`/`l`, `H`/`L`, `n`/`p`, `a`/`A`).

### 5.4. Networking & SoundCloud API (`src/net/`)
- **Transport**: `HttpClient` wrapping `libcurl`.
- **Client-ID Auto-Resolution**: When no `client_id` is specified, `SoundCloudApi::resolve_client_id()` fetches `https://soundcloud.com`, discovers active client asset bundles via regex, downloads the JavaScript bundle, and parses the live `client_id`.
- **Geo-Unblocking & Privacy**:
  - **DNS-over-HTTPS (DoH)**: Native DoH integration (Cloudflare, Google, Quad9, or Custom URL) using `CURLOPT_DOH_URL`.
  - **Custom DNS & SOCKS5/HTTP Proxy**: `CURLOPT_DNS_SERVERS` and `CURLOPT_PROXY`.
- **Likes Pagination**: Asynchronously fetches paginated likes (`next_href`) in chunks of 100, updating the UI incrementally without blocking the thread.

### 5.5. Caching & Persistence (`src/core/track_cache.cpp`)
- **Cache Location**: `~/.cache/sctui/tracks/`, `~/.cache/sctui/covers/`, and `~/.cache/sctui/metadata/`.
- **Liked Tracks Pinning**: Option `lock_liked_tracks_in_cache` protects liked songs from being purged during LRU eviction cycles.
- **Independent Size Accounting**: Option `dont_count_liked_in_general_cache` allows liked songs to be cached permanently without consuming the standard track cache quota (`max_cache_size_mb`).

### 5.6. Authentication & Cookie Extraction (`src/core/cookie_extractor.cpp`)
- **Direct Local DB Parsing**:
  - **Firefox**: Queries `cookies.sqlite` for host `soundcloud.com` and cookie `oauth_token`.
  - **Chromium / Chrome / Brave / Edge / Opera / Vivaldi**: Queries `Cookies` (SQLite) and performs Linux `v10`/`v11` DPAPI/SecretService decryption or plain token extraction.
- **Onboarding Wizard**: On first run, scans the user's system, lists detected browsers, and automatically imports the session cookie with one keypress.

### 5.7. System Integrations
- **MPRIS 2.2 (`src/mpris/mpris_server.cpp`)**:
  - Registers D-Bus service `org.mpris.MediaPlayer2.sctui` via `sd-bus` / `systemd`.
  - Handles `Play`, `Pause`, `PlayPause`, `Next`, `Previous`, `Stop`, `Seek`, `SetPosition`, `Volume`.
  - Emits `PropertiesChanged` signals for `PlaybackStatus`, `Metadata` (track ID, title, artist, art URL, duration), and `Position`.
- **Discord RPC (`src/discord/discord_rpc.cpp`)**:
  - Connects to Discord's local IPC Unix domain socket (`$XDG_RUNTIME_DIR/discord-ipc-0`).
  - Sends rich presence updates displaying track title, artist name, elapsed time, total duration, and playback status icons.

---

## 6. IPC Protocol Specification (C++ <-> DRM Satellite)

Communication between `DrmSatelliteDaemon` (C++) and `satellite/main.js` (Node/Electron) occurs over stdin/stdout as newline-delimited JSON objects.

### Commands (C++ → Satellite):
```json
{"command": "play", "track_id": 123456, "url": "https://api-v2.soundcloud.com/media/..."}
{"command": "pause"}
{"command": "resume"}
{"command": "seek", "position_ms": 45000}
{"command": "set_volume", "volume": 1.25}
{"command": "stop"}
```

### Events (Satellite → C++):
```json
{"event": "ready"}
{"event": "state", "state": "playing", "position_ms": 1230, "duration_ms": 184000}
{"event": "timeupdate", "position_ms": 2500, "duration_ms": 184000}
{"event": "finished", "track_id": 123456}
{"event": "error", "message": "Failed to load DRM key"}
{"event": "spectrum", "bands": [0.1, 0.4, 0.8, ...]}
```

---

## 7. Build System & Compilation (`xmake.lua`)

The project builds via **xmake** with embedded compiler flags enforcing strict modern C++23.

### Key Targets:
- `sctui`: Main application binary (`bin/sctui`).
- `test_suite`: Automated testing harness (`bin/test_suite`).

### Toolchain Enforcements:
- **Compiler**: Clang (`set_toolchains("clang")`).
- **Linker**: Mold (`add_ldflags("-fuse-ld=mold", {force = true})`).
- **Release Optimization Flags**:
  - `-O3`, `-flto`, `-march=native`, `-mtune=native`
  - `-fno-math-errno`, `-fno-trapping-math`, `-fomit-frame-pointer`
  - `-fvectorize`, `-funroll-loops`
- **System Libraries**: `-lcurl`, `-lpthread`, `-lm`, `-ldl`, `-lasound`, `-lpulse`, `-lsystemd`, `-lsqlite3`, `-lcrypto`.

---

## 8. Setup, Installation & Distribution

- **One-Line Installer (`get.sh`)**:
  ```bash
  curl -fsSL https://raw.githubusercontent.com/p4nika19/sctui/main/get.sh | bash
  ```
  Clones into a temporary directory, runs `install.sh`, and handles non-root (`--user`) installations smoothly.
- **Universal Installer (`install.sh`)**:
  - Automatically detects package managers: `pacman`, `apt`, `dnf`, `zypper`, `apk`, `xbps`, `emerge`, `brew`.
  - Installs required compiler packages and `xmake`.
  - Compiles `sctui` in release mode.
  - Installs binary to `/usr/local/bin/sctui` (or `~/.local/bin/sctui`) and satellite assets to `/usr/local/share/sctui/satellite` (or `~/.local/share/sctui/satellite`).
- **Complete Uninstallation (`uninstall.sh`)**:
  - Terminates running daemons and applications.
  - Removes binaries and satellite directories.
  - Optionally purges config (`~/.config/sctui`) and cache (`~/.cache/sctui`).

---

## 9. Configuration & Standard Paths

All paths adhere strictly to the **XDG Base Directory Specification**:

| Resource | Path | Description |
|---|---|---|
| **Configuration** | `~/.config/sctui/config.json` | Stores client credentials, volume, theme, and network settings. |
| **Logs** | `~/.config/sctui/sctui.log` | Leveled application runtime logs with automatic log rotation. |
| **Track Cache** | `~/.cache/sctui/tracks/` | Cached MP3/AAC audio files (`<track_id>.mp3`). |
| **Cover Art Cache** | `~/.cache/sctui/covers/` | Cached JPEG/PNG artwork (`<track_id>.jpg`). |
| **Metadata Cache** | `~/.cache/sctui/metadata/`| Track JSON metadata files for offline indexing. |
| **System Satellite** | `/usr/local/share/sctui/satellite` or `~/.local/share/sctui/satellite` | Electron runtime and Widevine CDM files. |

> **Note on Migration**: In commit `c83b5f7`, all legacy paths referencing `soundcloud-tui` were officially deprecated and migrated to `sctui`. `Config::load()` and `Logger::init()` automatically detect legacy directories and migrate them seamlessly.

---

## 10. Test Suite & Verification (`tests/test_suite.cpp`)

The codebase includes an extensive built-in test suite featuring 32 verification targets:
- Lock-free SPSC Queue & Bounded Channel concurrency verification.
- SoundCloud API JSON parsing & Client ID scraper mocking.
- Cryptographic entropy distribution & shuffle invariance.
- MPRIS D-Bus message formatting.
- DRM daemon detection, unpack logic, and EME audio-only constraints.
- Cookie extraction across browser formats.
- DoH resolution and URL formatting.
- Terminal graphics detection (Kitty/iTerm2/Half-block).
- Track cache LRU eviction and liked-track locking.
- In-tab filtering, volume curve scaling, and mouse coordinate mapping.

To build and run tests:
```bash
xmake build test_suite
xmake run test_suite
```

---

## 11. Critical Knowledge Base: Past Bugs, Gotchas & Edge Cases

When developing or modifying `sctui`, be mindful of these subtle edge cases resolved during development:

1. **Widevine EME Audio-Only Requirement (`satellite/player.html`)**:
   - *Problem*: Calling `navigator.requestMediaKeySystemAccess` with default configurations containing video capabilities fails on headless Linux with `keySystemNoAccess`.
   - *Fix*: Strictly pass audio-only capabilities (`audioCapabilities: [{ contentType: 'audio/mp4; codecs="mp4a.40.2"' }]`). Exclude video capabilities entirely.
2. **Npm Git Dependency Restrictions (`satellite/.npmrc`)**:
   - *Problem*: Modern npm (v10+, v12+) blocks git-based package installs with error `EALLOWGIT`.
   - *Fix*: An `.npmrc` file with `allow-git=all` must be present in `satellite/`, and installation commands must pass `--allow-git=all`.
3. **Electron Binary Unpacking (`install.js`)**:
   - *Problem*: CastLabs electron package from GitHub downloads a zip archive during install, but npm does not always automatically unpack `dist/electron` in headless environments.
   - *Fix*: `install.sh` and `DrmDaemon.cpp` explicitly invoke `node node_modules/electron/install.js` if `dist/electron` is absent.
4. **Bash `set -u` (Nounset) Trap in Third-Party Scripts**:
   - *Problem*: `install.sh` runs with `set -euo pipefail`. Sourcing `~/.xmake/profile` caused silent shell aborts because xmake’s script checks `test $FISH_VERSION` without quoting or defaults, triggering an unbound variable error.
   - *Fix*: Never source foreign profile scripts under `set -u`. Wrap in `set +u ... set -u` or export variables (`PATH`, `XMAKE_ROOTDIR`, `XMAKE_PROGRAM_DIR`) directly.
5. **Piped `curl ... | bash` Stdin Truncation**:
   - *Problem*: Running `exec </dev/tty` at the start of a script being piped into `bash` closes fd 0 (the curl pipe), immediately terminating the script midway through execution.
   - *Fix*: Never execute `exec </dev/tty` globally in a piped installer. Only redirect `< /dev/tty` to interactive subcommands or child scripts (`bash install.sh "$@" </dev/tty`).
6. **Square Cover Art Terminal Aspect Ratio**:
   - *Problem*: Terminal characters typically have a 1:2 width-to-height aspect ratio. Rendering an $N \times N$ character box yields a vertical rectangle instead of a square.
   - *Fix*: `CoverArtProcessor` renders half-block elements with a calibrated width-to-height ratio ($W \approx 2.2 \times H$) to display perfectly square album artwork across terminal emulators.
7. **Shuffle Duplicate Track Cursor Integrity**:
   - *Problem*: Shuffling tracks while keeping the active track playing could cause the current track to be replayed or skipped.
   - *Fix*: `QueueState::rebuild_shuffle()` locates the current track index in `shuffle_indices` and swaps it with index 0, guaranteeing seamless continuous playback.

---

## 12. Roadmap & Active Tasks (`todo`)

The `todo` file tracks currently targeted features and active investigations:
1. **DNS Resilience & Benchmarks**: Real-time DoH failover testing and DNS benchmark comparisons.
2. **Offline Mode Tab**: Dedicated TUI view for playing downloaded and cached tracks without an active internet connection.
3. **Custom Proxy Support**: GUI settings for HTTP / SOCKS5 proxy configuration.
4. **UI Improvements**: Replacing ASCII SC header logo with enhanced Unicode/vector rendering; auto-hiding DoH confirmation buttons.
5. **Artist & Album Navigation**: Deep-linking to dedicated artist discography and full album views from track selection menus.

---
*Generated for seamless transition to future AI coding tools and contributors.*
