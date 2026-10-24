#pragma once

#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <memory>
#include <functional>
#include "sctui/core/types.hpp"
#include "sctui/core/spsc_queue.hpp"

namespace sctui::core {
class TrackCacheManager;
}

namespace sctui::audio {

enum class PlaybackState {
    Stopped = 0,
    Loading,
    Buffering,
    Playing,
    Paused,
    Finished,
    Error
};

enum class AudioCommandType {
    Play,
    Pause,
    Resume,
    TogglePause,
    Stop,
    SeekRelative,
    SeekAbsolute,
    SetVolume,
    Shutdown
};

struct AudioCommand {
    AudioCommandType type{AudioCommandType::Stop};
    std::string stream_url;
    uint64_t track_id{0};
    uint64_t duration_ms{0};
    int64_t seek_delta_ms{0};
    uint64_t seek_target_ms{0};
    float volume{1.0f};
    bool is_drm{false};
};

constexpr size_t VisualizerBandsCount = 16;

class AudioEngine {
public:
    using TrackFinishedCallback = std::function<void(uint64_t track_id)>;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    core::Result<void> init();
    void shutdown();

    // Command dispatch (non-blocking, lock-free SPSC communication)
    void play(const std::string& stream_url, uint64_t track_id, uint64_t duration_ms, bool is_drm = false);
    void pause();
    void resume();
    void toggle_playback();
    void stop();
    void seek_relative(int64_t delta_ms);
    void seek_absolute(uint64_t position_ms);
    void set_volume(float volume_percent_0_to_150);

    // Atomic telemetry access (lock-free from UI thread)
    [[nodiscard]] PlaybackState get_state() const noexcept;
    [[nodiscard]] bool is_playing() const noexcept;
    [[nodiscard]] uint64_t get_current_track_id() const noexcept;
    [[nodiscard]] uint64_t get_position_ms() const noexcept;
    [[nodiscard]] uint64_t get_duration_ms() const noexcept;
    [[nodiscard]] float get_volume_linear() const noexcept;
    [[nodiscard]] float get_volume_percent() const noexcept;
    [[nodiscard]] float get_buffer_percent() const noexcept;
    [[nodiscard]] std::array<float, VisualizerBandsCount> get_visualizer_bands() const noexcept;
    [[nodiscard]] std::pair<float, float> get_rms_levels() const noexcept;

    void set_track_finished_callback(TrackFinishedCallback callback);
    void set_cache_manager(std::shared_ptr<core::TrackCacheManager> cache_mgr);
    [[nodiscard]] std::shared_ptr<core::TrackCacheManager> get_cache_manager() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sctui::audio
