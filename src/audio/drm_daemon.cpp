#include "sctui/audio/drm_daemon.hpp"
#include "sctui/core/logger.hpp"
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <iostream>
#include <array>
#include <algorithm>

namespace sctui::audio {

namespace {

std::filesystem::path resolve_satellite_dir(const std::filesystem::path& user_specified) {
    if (!user_specified.empty() && std::filesystem::exists(user_specified / "main.js")) {
        std::error_code ec;
        auto canon = std::filesystem::canonical(user_specified, ec);
        return ec ? user_specified : canon;
    }

    std::vector<std::filesystem::path> candidates;

    // Check relative to current working directory
    candidates.push_back(std::filesystem::current_path() / "satellite");

    // Check relative to executable (/proc/self/exe on Linux)
    std::error_code ec;
    auto exe_path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        auto dir = exe_path.parent_path();
        candidates.push_back(dir / "satellite");
        candidates.push_back(dir / ".." / "satellite");
        candidates.push_back(dir / ".." / ".." / "satellite");
        candidates.push_back(dir / ".." / ".." / ".." / "satellite");
        candidates.push_back(dir / ".." / ".." / ".." / ".." / "satellite");
    }

    // Check user config directory: ~/.config/soundcloud-tui/satellite
    const char* home = std::getenv("HOME");
    if (home && *home) {
        candidates.push_back(std::filesystem::path(home) / ".config" / "soundcloud-tui" / "satellite");
    }

    // Check well-known repo dir
    candidates.push_back("/home/p4nika/projects/cpp/sctui/satellite");

    for (const auto& candidate : candidates) {
        std::error_code err;
        if (std::filesystem::exists(candidate / "main.js", err)) {
            auto canon = std::filesystem::canonical(candidate, err);
            return err ? candidate : canon;
        }
    }

    return user_specified.empty() ? std::filesystem::path("satellite") : user_specified;
}

} // namespace

DrmSatelliteDaemon::DrmSatelliteDaemon(std::filesystem::path satellite_dir)
    : satellite_dir_(resolve_satellite_dir(satellite_dir)) {}

DrmSatelliteDaemon::~DrmSatelliteDaemon() {
    stop();
}

std::string DrmSatelliteDaemon::find_executable() const {
    // Check direct binary in node_modules/electron/dist/
    std::filesystem::path dist_dir = satellite_dir_ / "node_modules" / "electron" / "dist";
    std::filesystem::path electron_bin = dist_dir / "electron";

    if (std::filesystem::exists(dist_dir) && std::filesystem::exists(electron_bin)) {
        // Ensure sctui-drm-satellite and castlabs-electron symlinks exist so btop and ps clearly display it
        std::error_code ec;
        std::filesystem::path named_symlink = dist_dir / "castlabs-electron";
        if (!std::filesystem::exists(named_symlink, ec)) {
            std::filesystem::create_symlink("electron", named_symlink, ec);
        }
        if (!ec && std::filesystem::exists(named_symlink)) {
            return named_symlink.string();
        }
        return electron_bin.string();
    }

    // 2. Local electron wrapper (.bin/electron)
    std::filesystem::path local_electron = satellite_dir_ / "node_modules" / ".bin" / "electron";
    if (std::filesystem::exists(local_electron)) {
        return local_electron.string();
    }

    // 3. Local castlabs-electron
    std::filesystem::path local_castlabs = satellite_dir_ / "node_modules" / ".bin" / "castlabs-electron";
    if (std::filesystem::exists(local_castlabs)) {
        return local_castlabs.string();
    }

    // 4. System castlabs-electron in PATH
    if (::system("which castlabs-electron >/dev/null 2>&1") == 0) {
        return "castlabs-electron";
    }

    // 5. System electron in PATH
    if (::system("which electron >/dev/null 2>&1") == 0) {
        return "electron";
    }

    // 6. Fallback: npx
    if (::system("which npx >/dev/null 2>&1") == 0) {
        return "npx";
    }

    return "node";
}

bool DrmSatelliteDaemon::start() {
    if (is_running_.load()) {
        return true;
    }

    satellite_dir_ = resolve_satellite_dir(satellite_dir_);
    std::filesystem::path main_js = satellite_dir_ / "main.js";
    if (!std::filesystem::exists(main_js)) {
        LOG_ERROR("DrmDaemon", "satellite/main.js not found at " + main_js.string());
        return false;
    }

    std::string exe = find_executable();
    LOG_INFO("DrmDaemon", "Spawning DRM satellite daemon using executable: " + exe);

    int stdin_pipe[2];
    int stdout_pipe[2];

    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        LOG_ERROR("DrmDaemon", "Failed to create IPC pipes for satellite daemon");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("DrmDaemon", "fork() failed for satellite daemon");
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // Child process (Satellite Daemon)
        // Ensure child terminates cleanly if parent dies
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        // Remove environment variables that break Electron zygote or force node mode
        ::unsetenv("CHROME_DESKTOP");
        ::unsetenv("ELECTRON_RUN_AS_NODE");

        // Redirect stdin and stdout to pipes
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        // Close unused pipe ends in child
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        std::string dir_str = satellite_dir_.string();
        std::string main_str = main_js.string();

        // Switch child working directory to satellite_dir_ so all relative assets resolve
        (void)::chdir(dir_str.c_str());

        if (exe == "npx") {
            execlp("npx", "npx", "@castlabs/electron", main_str.c_str(), nullptr);
        } else {
            execlp(exe.c_str(), exe.c_str(), main_str.c_str(), nullptr);
        }

        // If exec fails:
        std::cerr << "[DrmDaemon:CHILD] Failed to exec: " << exe << std::endl;
        _exit(127);
    }

    // Parent process
    close(stdin_pipe[0]);  // close read end of stdin pipe
    close(stdout_pipe[1]); // close write end of stdout pipe

    daemon_pid_ = pid;
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
    is_running_.store(true);

    reader_thread_ = std::jthread([this](std::stop_token stop_tok) {
        read_loop(stop_tok);
    });

    LOG_INFO("DrmDaemon", "Started DRM satellite daemon (PID " + std::to_string(pid) + ")");
    return true;
}

void DrmSatelliteDaemon::stop() {
    if (!is_running_.exchange(false)) {
        return;
    }

    is_ready_.store(false);

    // 1. Send clean exit command over stdin
    send_command("{\"cmd\":\"exit\"}");

    // 2. Close stdin pipe to trigger EOF in daemon
    if (stdin_fd_ >= 0) {
        close(stdin_fd_);
        stdin_fd_ = -1;
    }

    // 3. Stop background reader thread
    if (reader_thread_.joinable()) {
        reader_thread_.request_stop();
    }

    if (stdout_fd_ >= 0) {
        close(stdout_fd_);
        stdout_fd_ = -1;
    }

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }

    // 4. Wait for child exit with non-blocking polling, escalate to SIGTERM/SIGKILL if stuck
    if (daemon_pid_ > 0) {
        int status = 0;
        bool exited = false;
        for (int i = 0; i < 10; ++i) {
            pid_t res = waitpid(daemon_pid_, &status, WNOHANG);
            if (res == daemon_pid_) {
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        if (!exited) {
            LOG_WARN("DrmDaemon", "Daemon PID " + std::to_string(daemon_pid_) + " did not terminate, sending SIGTERM");
            kill(daemon_pid_, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (waitpid(daemon_pid_, &status, WNOHANG) != daemon_pid_) {
                kill(daemon_pid_, SIGKILL);
                waitpid(daemon_pid_, &status, 0);
            }
        }
        daemon_pid_ = -1;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.playback_state = PlaybackState::Stopped;
    }

    LOG_INFO("DrmDaemon", "DRM satellite daemon stopped.");
}

bool DrmSatelliteDaemon::is_running() const noexcept {
    return is_running_.load();
}

bool DrmSatelliteDaemon::is_ready() const noexcept {
    return is_ready_.load();
}

DrmSatelliteState DrmSatelliteDaemon::get_state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

void DrmSatelliteDaemon::send_command(const std::string& json_line) {
    if (stdin_fd_ < 0) return;
    std::string line = json_line + "\n";
    ssize_t ret = ::write(stdin_fd_, line.data(), line.size());
    (void)ret;
}

void DrmSatelliteDaemon::play(uint64_t track_id, const std::string& stream_url) {
    if (!is_running_.load()) {
        if (!start()) return;
    }

    // Wait briefly for daemon readiness (up to 1.5s) if it was just launched
    for (int i = 0; i < 30 && !is_ready_.load() && is_running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    nlohmann::json cmd;
    cmd["cmd"] = "play";
    cmd["track_id"] = track_id;
    cmd["stream_url"] = stream_url;
    cmd["volume"] = current_volume_.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.current_track_id = track_id;
        state_.playback_state = PlaybackState::Loading;
    }

    send_command(cmd.dump());
}

void DrmSatelliteDaemon::pause() {
    send_command("{\"cmd\":\"pause\"}");
}

void DrmSatelliteDaemon::resume() {
    send_command("{\"cmd\":\"resume\"}");
}

void DrmSatelliteDaemon::seek(uint64_t position_ms) {
    nlohmann::json cmd;
    cmd["cmd"] = "seek";
    cmd["position_ms"] = position_ms;
    send_command(cmd.dump());
}

void DrmSatelliteDaemon::set_volume(float volume_percent_0_to_150) {
    float norm_vol = std::clamp(volume_percent_0_to_150 / 100.0f, 0.0f, 1.5f);
    // Apply identical perceptual volume logarithmic curve as miniaudio (gain = norm_vol^2.2)
    float gain = std::pow(norm_vol, 2.2f);
    float clamped_gain = std::clamp(gain, 0.0f, 1.0f);
    current_volume_.store(clamped_gain, std::memory_order_relaxed);
    nlohmann::json cmd;
    cmd["cmd"] = "set_volume";
    cmd["volume"] = clamped_gain;
    send_command(cmd.dump());
}

void DrmSatelliteDaemon::stop_playback() {
    send_command("{\"cmd\":\"stop\"}");
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.playback_state = PlaybackState::Stopped;
}

void DrmSatelliteDaemon::set_on_state(StateCallback cb) {
    on_state_ = std::move(cb);
}

void DrmSatelliteDaemon::set_on_time_update(TimeUpdateCallback cb) {
    on_time_update_ = std::move(cb);
}

void DrmSatelliteDaemon::set_on_finished(FinishedCallback cb) {
    on_finished_ = std::move(cb);
}

void DrmSatelliteDaemon::set_on_error(ErrorCallback cb) {
    on_error_ = std::move(cb);
}

void DrmSatelliteDaemon::set_on_spectrum(SpectrumCallback cb) {
    on_spectrum_ = std::move(cb);
}

void DrmSatelliteDaemon::read_loop(std::stop_token stop_tok) {
    std::string buffer;
    std::array<char, 4096> chunk{};

    while (!stop_tok.stop_requested() && stdout_fd_ >= 0) {
        ssize_t bytes_read = ::read(stdout_fd_, chunk.data(), chunk.size());
        if (bytes_read <= 0) {
            break;
        }

        buffer.append(chunk.data(), static_cast<size_t>(bytes_read));

        size_t newline_pos = 0;
        while ((newline_pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline_pos);
            buffer.erase(0, newline_pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty()) {
                handle_ipc_message(line);
            }
        }
    }
}

void DrmSatelliteDaemon::handle_ipc_message(const std::string& line) {
    try {
        nlohmann::json j = nlohmann::json::parse(line);
        std::string type = j.value("type", "");

        if (type == "ready") {
            is_ready_.store(true);
            LOG_INFO("DrmDaemon", "DRM satellite reported ready: CDM=" + j.value("cdm", "none"));
        } else if (type == "state") {
            std::string state_str = j.value("playback_state", "stopped");
            uint64_t pos = j.value("position_ms", 0ULL);
            uint64_t dur = j.value("duration_ms", 0ULL);

            PlaybackState pb_state = PlaybackState::Stopped;
            if (state_str == "playing") pb_state = PlaybackState::Playing;
            else if (state_str == "paused") pb_state = PlaybackState::Paused;
            else if (state_str == "buffering") pb_state = PlaybackState::Buffering;
            else if (state_str == "error") pb_state = PlaybackState::Error;

            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.playback_state = pb_state;
                if (pos > 0) state_.position_ms = pos;
                if (dur > 0) state_.duration_ms = dur;
            }

            if (on_state_) on_state_(pb_state, pos, dur);
        } else if (type == "timeupdate") {
            uint64_t pos = j.value("position_ms", 0ULL);
            uint64_t dur = j.value("duration_ms", 0ULL);

            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.position_ms = pos;
                if (dur > 0) state_.duration_ms = dur;
            }

            if (on_time_update_) on_time_update_(pos, dur);
        } else if (type == "spectrum") {
            if (j.contains("bands") && j["bands"].is_array()) {
                const auto& arr = j["bands"];
                std::array<float, VisualizerBandsCount> spectrum{};
                size_t count = std::min(arr.size(), VisualizerBandsCount);
                for (size_t i = 0; i < count; ++i) {
                    if (arr[i].is_number()) {
                        spectrum[i] = arr[i].get<float>();
                    }
                }
                if (on_spectrum_) {
                    on_spectrum_(spectrum);
                }
            }
        } else if (type == "finished") {
            uint64_t track_id = j.value("track_id", 0ULL);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.playback_state = PlaybackState::Finished;
            }
            if (on_finished_) on_finished_(track_id);
        } else if (type == "error") {
            std::string msg = j.value("message", "DRM daemon error");
            LOG_ERROR("DrmDaemon", "DRM satellite error: " + msg);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.playback_state = PlaybackState::Error;
            }
            if (on_error_) on_error_(msg);
        }
    } catch (const std::exception& e) {
        LOG_WARN("DrmDaemon", "Failed to parse NDJSON from daemon: " + std::string(e.what()));
    }
}

} // namespace sctui::audio
