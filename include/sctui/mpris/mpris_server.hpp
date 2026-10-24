#pragma once

#include <string>
#include <memory>
#include <functional>
#include <optional>
#include <cstdint>
#include "sctui/models/track.hpp"
#include "sctui/core/config.hpp"
#include "sctui/audio/audio_engine.hpp"

namespace sctui::mpris {

struct MprisCallbacks {
    std::function<void()> on_play;
    std::function<void()> on_pause;
    std::function<void()> on_play_pause;
    std::function<void()> on_stop;
    std::function<void()> on_next;
    std::function<void()> on_prev;
    std::function<void(int64_t offset_ms)> on_seek_relative;
    std::function<void(uint64_t target_ms)> on_seek_absolute;
    std::function<void(float volume_percent)> on_set_volume;
    std::function<void(bool shuffle)> on_set_shuffle;
    std::function<void(core::RepeatMode mode)> on_set_loop_status;
    std::function<void()> on_quit;
};

class MprisServer {
public:
    explicit MprisServer(MprisCallbacks callbacks);
    ~MprisServer();

    MprisServer(const MprisServer&) = delete;
    MprisServer& operator=(const MprisServer&) = delete;

    bool start(const std::string& custom_service_name = "");
    void stop();

    [[nodiscard]] const std::string& get_service_name() const noexcept;

    // Telemetry and state synchronization (thread-safe)
    void update_playback_state(audio::PlaybackState state);
    void update_track(const std::optional<models::Track>& track);
    void update_position(uint64_t position_ms);
    void update_volume(float volume_percent);
    void update_shuffle(bool shuffle);
    void update_repeat_mode(core::RepeatMode mode);
    void notify_seeked(uint64_t position_ms);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace sctui::mpris
