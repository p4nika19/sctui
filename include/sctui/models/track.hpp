#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace sctui::models {

enum class TrackAccessType {
    Playable = 0,
    GoPlus,        // SoundCloud Go+ subscription required (SNIP / SUB_HIGH_TIER)
    DrmProtected   // DRM protected / regional block (only cbc/ctr-encrypted-hls or policy BLOCK)
};

struct Transcoding {
    std::string url;
    std::string preset;
    std::string duration;
    std::string protocol;   // e.g. "progressive" or "hls"
    std::string mime_type;  // e.g. "audio/mpeg" or "audio/ogg; codecs=\"opus\""
    bool snipped{false};
};

struct User {
    uint64_t id{0};
    std::string username;
    std::string permalink;
    std::string avatar_url;
    uint64_t followers_count{0};
};

struct Track {
    uint64_t id{0};
    std::string title;
    std::string artist;
    uint64_t artist_id{0};
    std::string permalink_url;
    std::string artwork_url;
    uint64_t duration_ms{0};
    std::string genre;
    std::string description;
    uint64_t playback_count{0};
    uint64_t likes_count{0};
    uint64_t reposts_count{0};
    std::string created_at;

    // Direct stream resolution
    std::string stream_url;
    std::vector<Transcoding> transcodings;

    // Visual waveform data
    std::string waveform_url;
    std::vector<float> waveform_samples; // 0.0f - 1.0f normalized heights

    bool is_liked{false};

    // DRM and Go+ Access Categorization
    TrackAccessType access_type{TrackAccessType::Playable};
    std::string policy;
    std::string monetization_model;
    bool is_snipped{false};

    [[nodiscard]] bool is_drm_protected() const noexcept {
        return access_type == TrackAccessType::DrmProtected;
    }

    [[nodiscard]] bool is_go_plus() const noexcept {
        return access_type == TrackAccessType::GoPlus;
    }

    [[nodiscard]] bool has_playable_stream() const noexcept {
        return !is_drm_protected() && (!stream_url.empty() || !transcodings.empty());
    }
};

struct Playlist {
    uint64_t id{0};
    std::string title;
    std::string description;
    std::string permalink_url;
    std::string artwork_url;
    uint64_t track_count{0};
    uint64_t duration_ms{0};
    uint64_t likes_count{0};
    User user;
    std::vector<Track> tracks;
};

} // namespace sctui::models
