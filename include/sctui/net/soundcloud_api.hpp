#pragma once

#include <string>
#include <vector>
#include <memory>
#include "sctui/core/types.hpp"
#include "sctui/models/track.hpp"
#include "sctui/models/user.hpp"
#include "sctui/net/http_client.hpp"

namespace sctui::net {

class SoundCloudApi {
public:
    explicit SoundCloudApi(std::string client_id = "", std::string auth_token = "");
    ~SoundCloudApi();

    void set_client_id(std::string client_id);
    [[nodiscard]] const std::string& get_client_id() const noexcept;

    void set_auth_token(std::string token);
    [[nodiscard]] const std::string& get_auth_token() const noexcept;

    void configure_network(
        const std::string& doh_url = "",
        const std::string& dns_servers = "",
        const std::string& proxy = ""
    );

    // Automatic Client-ID resolution via scraping soundcloud.com web bundle
    core::Result<std::string> resolve_client_id();

    // Search endpoints
    core::Result<std::vector<models::Track>> search_tracks(
        const std::string& query,
        size_t limit = 20,
        size_t offset = 0
    );

    core::Result<std::vector<models::Playlist>> search_playlists(
        const std::string& query,
        size_t limit = 10,
        size_t offset = 0
    );

    struct LikesPage {
        std::vector<models::Track> tracks;
        std::string next_href;
    };

    // User Library / Stream endpoints
    core::Result<LikesPage> get_user_likes_page(
        size_t limit = 100,
        const std::string& next_url = ""
    );

    core::Result<std::vector<models::Track>> get_user_likes(
        size_t limit = 100,
        size_t offset = 0
    );

    core::Result<std::vector<models::Track>> get_user_stream(
        size_t limit = 25,
        size_t offset = 0
    );

    core::Result<std::vector<models::Playlist>> get_user_playlists(
        size_t limit = 20,
        size_t offset = 0
    );

    core::Result<std::vector<models::Track>> get_playlist_tracks(uint64_t playlist_id);
    core::Result<models::Track> get_track_details(uint64_t track_id);

    core::Result<uint64_t> resolve_user_id();
    core::Result<models::UserProfile> get_me();
    core::Result<std::vector<models::Track>> get_user_uploaded_tracks(uint64_t user_id, size_t limit = 50);

    // Audio stream & metadata resolution
    core::Result<std::string> resolve_stream_url(const models::Track& track, bool allow_drm = false);
    core::Result<std::vector<float>> fetch_waveform_samples(const std::string& waveform_url);

    // Toggle track like
    core::Result<bool> toggle_like_track(uint64_t track_id, bool current_liked_state);

private:
    std::string client_id_;
    std::string auth_token_;
    uint64_t user_id_{0};
    HttpClient http_;

    core::Result<std::vector<models::Track>> parse_tracks_json(const std::string& json_str);
    core::Result<std::vector<models::Playlist>> parse_playlists_json(const std::string& json_str);
};

} // namespace sctui::net
