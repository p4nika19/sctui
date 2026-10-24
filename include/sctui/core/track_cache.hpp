#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <filesystem>
#include <mutex>
#include <cstdint>
#include "sctui/models/track.hpp"

namespace sctui::core {

struct CachedTrackInfo {
    uint64_t track_id{0};
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
    std::string stream_url;
    std::string waveform_url;
    std::vector<float> waveform_samples;
    bool is_liked{false};
    models::TrackAccessType access_type{models::TrackAccessType::Playable};
    std::string policy;
    std::string monetization_model;
    bool is_snipped{false};

    std::filesystem::path file_path;
    std::filesystem::path cover_file_path;
    uint64_t size_bytes{0};
    uint64_t last_accessed{0}; // epoch seconds

    [[nodiscard]] models::Track to_track() const;
    static CachedTrackInfo from_track(
        const models::Track& track,
        const std::filesystem::path& path,
        uint64_t size,
        uint64_t last_accessed
    );
};

struct CacheStats {
    uint64_t total_size_bytes{0};
    uint64_t regular_size_bytes{0};
    uint64_t liked_size_bytes{0};
    size_t total_tracks{0};
    size_t regular_tracks{0};
    size_t liked_tracks{0};
    uint64_t max_size_bytes{0};
};

class TrackCacheManager {
public:
    explicit TrackCacheManager(const std::filesystem::path& cache_dir);

    // Checks if a track exists in cache and returns its path if valid
    [[nodiscard]] std::optional<std::filesystem::path> get_cached_track(uint64_t track_id);

    // Returns destination path for downloading/saving track audio
    [[nodiscard]] std::filesystem::path get_track_cache_path(uint64_t track_id) const;

    // Cover art caching paths & helpers
    [[nodiscard]] std::filesystem::path get_cover_cache_path(uint64_t track_id) const;
    [[nodiscard]] std::optional<std::filesystem::path> get_cached_cover(uint64_t track_id) const;
    void download_cover_art(const models::Track& track);

    // Individual JSON metadata path
    [[nodiscard]] std::filesystem::path get_metadata_cache_path(uint64_t track_id) const;

    // Registers or updates a completed track with full metadata in the cache index
    void register_track(const models::Track& track);

    // Overload for backward compatibility / tests
    void register_track(
        uint64_t track_id,
        const std::string& title = "",
        const std::string& artist = "",
        bool is_liked = false
    );

    // Retrieves complete metadata for a cached track if present
    [[nodiscard]] std::optional<models::Track> get_cached_track_metadata(uint64_t track_id) const;

    // Checks whether a track is present in the cache
    [[nodiscard]] bool is_track_cached(uint64_t track_id) const;

    // Updates the liked status of a cached track (locks/unlocks it)
    void update_track_liked(uint64_t track_id, bool is_liked);

    // Synchronizes all liked tracks from state/api (e.g. when likes are loaded)
    void sync_liked_tracks(const std::vector<uint64_t>& liked_ids);

    // Enforces max cache size with liked locking & exclude options
    void enforce_cache_limits(
        size_t max_cache_size_mb,
        bool lock_liked,
        bool dont_count_liked
    );

    // Deletes all unpinned/regular tracks, or all tracks if keep_liked is false
    void clear_cache(bool keep_liked = true);

    // Returns current statistics for display in Settings view
    [[nodiscard]] CacheStats get_stats(
        size_t max_cache_size_mb = 1024,
        bool lock_liked = true,
        bool dont_count_liked = false
    ) const;

private:
    std::filesystem::path cache_dir_;
    std::filesystem::path tracks_dir_;
    std::filesystem::path covers_dir_;
    std::filesystem::path metadata_dir_;
    std::filesystem::path metadata_file_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, CachedTrackInfo> tracks_;

    void load_metadata();
    void save_metadata();
};

} // namespace sctui::core
