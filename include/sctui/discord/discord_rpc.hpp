#pragma once

#include <string>
#include <optional>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include "sctui/models/track.hpp"
#include "sctui/audio/audio_engine.hpp"

namespace sctui::discord {

class DiscordRpc {
public:
    explicit DiscordRpc(std::string client_id = "1343314565749211247");
    ~DiscordRpc();

    void start();
    void stop();
    void set_enabled(bool enabled);
    [[nodiscard]] bool is_enabled() const noexcept;
    [[nodiscard]] bool is_connected() const noexcept;

    // Updates presence based on active track and playback telemetry
    void update_presence(
        const std::optional<models::Track>& track,
        audio::PlaybackState playback_state,
        uint64_t position_ms,
        uint64_t duration_ms
    );

    // Clears presence activity
    void clear_presence();

    // Builds the JSON payload for SET_ACTIVITY (exposed for unit testing)
    [[nodiscard]] static std::string build_activity_payload(
        const models::Track& track,
        audio::PlaybackState playback_state,
        uint64_t position_ms,
        uint64_t duration_ms
    );

    // Builds the JSON payload for Handshake (exposed for unit testing)
    [[nodiscard]] static std::string build_handshake_payload(const std::string& client_id);

private:
    std::string client_id_;
    std::atomic<bool> enabled_{true};
    std::atomic<bool> is_running_{false};
    std::atomic<bool> connected_{false};
    int socket_fd_{-1};
    mutable std::mutex mutex_;

    // Telemetry caching to prevent redundant frame emissions
    uint64_t last_track_id_{0};
    audio::PlaybackState last_state_{audio::PlaybackState::Stopped};
    uint64_t last_update_sec_{0};

    std::jthread worker_thread_;

    bool connect_socket();
    void disconnect_socket();
    bool send_frame(uint32_t opcode, const std::string& json_payload);
    bool send_handshake();
    void read_incoming();
};

} // namespace sctui::discord
