#include "sctui/discord/discord_rpc.hpp"
#include "sctui/core/cover_art.hpp"
#include "sctui/core/logger.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <nlohmann/json.hpp>

namespace sctui::discord {

namespace {

uint64_t current_epoch_seconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

std::string truncate_utf8(std::string value, size_t max_bytes) {
    if (value.size() <= max_bytes) return value;
    size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) {
        end--;
    }
    value.resize(end);
    return value;
}

std::vector<std::string> get_candidate_socket_paths() {
    std::vector<std::string> dirs;
    const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime && *xdg_runtime) {
        dirs.emplace_back(xdg_runtime);
        dirs.push_back(std::string(xdg_runtime) + "/app/com.discordapp.Discord");
        dirs.push_back(std::string(xdg_runtime) + "/app/com.discordapp.DiscordCanary");
        dirs.push_back(std::string(xdg_runtime) + "/app/dev.vencord.Vesktop");
    }
    const char* tmpdir = std::getenv("TMPDIR");
    if (tmpdir && *tmpdir) {
        dirs.emplace_back(tmpdir);
    }
    dirs.emplace_back("/tmp");

    std::vector<std::string> candidates;
    for (const auto& dir : dirs) {
        for (int i = 0; i < 10; ++i) {
            candidates.push_back(dir + "/discord-ipc-" + std::to_string(i));
        }
    }
    return candidates;
}

} // namespace

DiscordRpc::DiscordRpc(std::string client_id)
    : client_id_(std::move(client_id)) {}

DiscordRpc::~DiscordRpc() {
    stop();
}

void DiscordRpc::start() {
    if (is_running_) return;
    is_running_ = true;

    worker_thread_ = std::jthread([this](std::stop_token stop_tok) {
        while (!stop_tok.stop_requested() && is_running_) {
            if (enabled_ && !connected_) {
                if (connect_socket()) {
                    LOG_INFO("DiscordRpc", "Connected to Discord IPC socket");
                }
            } else if (!enabled_ && connected_) {
                clear_presence();
                disconnect_socket();
            }

            if (connected_) {
                read_incoming();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    });
}

void DiscordRpc::stop() {
    is_running_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.request_stop();
        worker_thread_.join();
    }
    clear_presence();
    disconnect_socket();
}

void DiscordRpc::set_enabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled && connected_) {
        clear_presence();
        disconnect_socket();
    }
}

bool DiscordRpc::is_enabled() const noexcept {
    return enabled_;
}

bool DiscordRpc::is_connected() const noexcept {
    return connected_;
}

bool DiscordRpc::connect_socket() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_) return true;

    auto paths = get_candidate_socket_paths();
    for (const auto& path : paths) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode)) {
            continue;
        }

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) continue;

        // Set non-blocking or reasonable timeout
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            socket_fd_ = fd;
            connected_ = true;
            if (send_handshake()) {
                // Read and verify the READY response from Discord IPC
                uint32_t resp_hdr[2];
                ssize_t n = recv(socket_fd_, resp_hdr, sizeof(resp_hdr), 0);
                if (n == sizeof(resp_hdr)) {
                    uint32_t r_len = resp_hdr[1];
                    if (r_len > 0 && r_len < 65536) {
                        std::vector<char> r_buf(r_len);
                        size_t total_r = 0;
                        while (total_r < r_len) {
                            ssize_t r = recv(socket_fd_, r_buf.data() + total_r, r_len - total_r, 0);
                            if (r <= 0) break;
                            total_r += r;
                        }
                    }
                    LOG_INFO("DiscordRpc", "Connected and handshaked with Discord IPC: " + path);
                    last_track_id_ = 0;
                    return true;
                }
            }
            close(socket_fd_);
            socket_fd_ = -1;
            connected_ = false;
        } else {
            close(fd);
        }
    }

    return false;
}

void DiscordRpc::disconnect_socket() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    connected_ = false;
}

bool DiscordRpc::send_frame(uint32_t opcode, const std::string& json_payload) {
    if (socket_fd_ < 0) return false;

    uint32_t header[2];
    header[0] = opcode;
    header[1] = static_cast<uint32_t>(json_payload.size());

    // Send header (8 bytes)
    ssize_t sent = send(socket_fd_, header, sizeof(header), MSG_NOSIGNAL);
    if (sent != sizeof(header)) {
        connected_ = false;
        return false;
    }

    // Send payload
    if (!json_payload.empty()) {
        sent = send(socket_fd_, json_payload.data(), json_payload.size(), MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(json_payload.size())) {
            connected_ = false;
            return false;
        }
    }

    return true;
}

std::string DiscordRpc::build_handshake_payload(const std::string& client_id) {
    nlohmann::json j;
    j["v"] = 1;
    j["client_id"] = client_id;
    return j.dump();
}

bool DiscordRpc::send_handshake() {
    return send_frame(0, build_handshake_payload(client_id_));
}

std::string DiscordRpc::build_activity_payload(
    const models::Track& track,
    audio::PlaybackState playback_state,
    uint64_t position_ms,
    uint64_t duration_ms
) {
    try {
        nlohmann::json activity;

        std::string title = !track.title.empty() ? track.title : "SoundCloud Track";
        std::string artist = !track.artist.empty() ? track.artist : "SoundCloud Artist";
        if (title.length() < 2) title += " ";
        if (artist.length() < 2) artist += " ";
        title = truncate_utf8(std::move(title), 120);
        artist = truncate_utf8(std::move(artist), 120);

        activity["name"] = "SoundCloud";
        activity["type"] = 2;
        activity["details"] = title;
        activity["state"] = "by " + artist;

        const uint64_t now_sec = current_epoch_seconds();
        const uint64_t elapsed_sec = position_ms / 1000;
        const uint64_t dur_sec = duration_ms / 1000;

        if (playback_state == audio::PlaybackState::Playing) {
            const uint64_t start_sec = (now_sec >= elapsed_sec) ? (now_sec - elapsed_sec) : now_sec;
            activity["timestamps"]["start"] = start_sec;
            if (dur_sec > elapsed_sec) {
                activity["timestamps"]["end"] = start_sec + dur_sec;
            }
        }

        if (!track.artwork_url.empty() && track.artwork_url.starts_with("https://")) {
            activity["assets"]["large_image"] = core::CoverArtProcessor::upgrade_artwork_url(track.artwork_url);
        } else {
            activity["assets"]["large_image"] = "soundcloud";
        }
        activity["assets"]["large_text"] = truncate_utf8(title + " • " + artist, 128);
        activity["assets"]["small_image"] = "waves";
        activity["assets"]["small_text"] = (playback_state == audio::PlaybackState::Playing) ? "Playing" : "Paused";

        if (!track.permalink_url.empty() && track.permalink_url.starts_with("https://")) {
            nlohmann::json btn;
            btn["label"] = "Listen on SoundCloud";
            btn["url"] = track.permalink_url;
            activity["buttons"] = nlohmann::json::array({ std::move(btn) });
        }

        nlohmann::json root;
        root["cmd"] = "SET_ACTIVITY";
        root["args"]["pid"] = getpid();
        root["args"]["activity"] = std::move(activity);
        root["nonce"] = std::to_string(now_sec);
        return root.dump();
    } catch (const std::exception& e) {
        LOG_WARN("DiscordRpc", "Failed to build activity payload: " + std::string(e.what()));
        return {};
    }
}

void DiscordRpc::update_presence(
    const std::optional<models::Track>& track,
    audio::PlaybackState playback_state,
    uint64_t position_ms,
    uint64_t duration_ms
) {
    if (!enabled_) return;

    if (!track.has_value() || playback_state == audio::PlaybackState::Stopped) {
        clear_presence();
        return;
    }

    uint64_t now_sec = current_epoch_seconds();
    // Throttle updates: only send if track changed, state changed, or 5 seconds elapsed
    if (track->id == last_track_id_ &&
        playback_state == last_state_ &&
        (now_sec - last_update_sec_) < 5) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) return;

    std::string payload = build_activity_payload(*track, playback_state, position_ms, duration_ms);
    if (payload.empty()) return;
    if (send_frame(1, payload)) {
        last_track_id_ = track->id;
        last_state_ = playback_state;
        last_update_sec_ = now_sec;
    } else {
        connected_ = false;
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
        last_track_id_ = 0;
    }
}

void DiscordRpc::clear_presence() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || socket_fd_ < 0) return;

    nlohmann::json root;
    root["cmd"] = "SET_ACTIVITY";
    root["args"]["pid"] = getpid();
    root["args"]["activity"] = nullptr;
    root["nonce"] = std::to_string(current_epoch_seconds());

    send_frame(1, root.dump());
    last_track_id_ = 0;
    last_state_ = audio::PlaybackState::Stopped;
}

void DiscordRpc::read_incoming() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_fd_ < 0 || !connected_) return;

    uint32_t header[2];
    ssize_t n = recv(socket_fd_, header, sizeof(header), MSG_DONTWAIT);
    if (n == sizeof(header)) {
        uint32_t opcode = header[0];
        uint32_t len = header[1];
        if (len > 0 && len < 65536) {
            std::vector<char> buf(len);
            size_t total = 0;
            while (total < len) {
                ssize_t r = recv(socket_fd_, buf.data() + total, len - total, 0);
                if (r > 0) total += r;
                else break;
            }
        }
        if (opcode == 2) { // Ping
            send_frame(3, "{}"); // Pong
        }
    } else if (n == 0) {
        // Socket closed by Discord
        connected_ = false;
        close(socket_fd_);
        socket_fd_ = -1;
        last_track_id_ = 0;
    }
}

} // namespace sctui::discord
