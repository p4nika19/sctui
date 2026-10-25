#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <filesystem>
#include "sctui/audio/audio_engine.hpp"

namespace sctui::audio {

struct DrmSatelliteState {
    PlaybackState playback_state{PlaybackState::Stopped};
    uint64_t position_ms{0};
    uint64_t duration_ms{0};
    uint64_t current_track_id{0};
};

class DrmSatelliteDaemon {
public:
    using StateCallback = std::function<void(PlaybackState state, uint64_t pos_ms, uint64_t dur_ms)>;
    using TimeUpdateCallback = std::function<void(uint64_t pos_ms, uint64_t dur_ms)>;
    using FinishedCallback = std::function<void(uint64_t track_id)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using SpectrumCallback = std::function<void(const std::array<float, VisualizerBandsCount>& bands)>;

    explicit DrmSatelliteDaemon(std::filesystem::path satellite_dir = "satellite");
    ~DrmSatelliteDaemon();

    DrmSatelliteDaemon(const DrmSatelliteDaemon&) = delete;
    DrmSatelliteDaemon& operator=(const DrmSatelliteDaemon&) = delete;

    bool start();
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_ready() const noexcept;
    [[nodiscard]] DrmSatelliteState get_state() const;

    void play(uint64_t track_id, const std::string& stream_url);
    void pause();
    void resume();
    void seek(uint64_t position_ms);
    void set_volume(float volume_percent_0_to_150);
    void stop_playback();

    void set_on_state(StateCallback cb);
    void set_on_time_update(TimeUpdateCallback cb);
    void set_on_finished(FinishedCallback cb);
    void set_on_error(ErrorCallback cb);
    void set_on_spectrum(SpectrumCallback cb);

private:
    std::filesystem::path satellite_dir_;
    pid_t daemon_pid_{-1};
    int stdin_fd_{-1};
    int stdout_fd_{-1};

    std::atomic<bool> is_running_{false};
    std::atomic<bool> is_ready_{false};
    std::atomic<float> current_volume_{0.2176f};

    mutable std::mutex state_mutex_;
    DrmSatelliteState state_;

    StateCallback on_state_;
    TimeUpdateCallback on_time_update_;
    FinishedCallback on_finished_;
    ErrorCallback on_error_;
    SpectrumCallback on_spectrum_;

    std::jthread reader_thread_;

    void send_command(const std::string& json_line);
    void read_loop(std::stop_token stop_tok);
    void handle_ipc_message(const std::string& line);
    std::string find_executable() const;
};

} // namespace sctui::audio
