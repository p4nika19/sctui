#include "sctui/core/track_cache.hpp"
#include "sctui/core/cover_art.hpp"
#include "sctui/core/logger.hpp"
#include "sctui/net/http_client.hpp"
#include <fstream>
#include <chrono>
#include <algorithm>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace sctui::core {

namespace {

uint64_t current_epoch_seconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

} // namespace

models::Track CachedTrackInfo::to_track() const {
    models::Track t;
    t.id = track_id;
    t.title = title;
    t.artist = artist;
    t.artist_id = artist_id;
    t.permalink_url = permalink_url;
    t.artwork_url = artwork_url;
    t.duration_ms = duration_ms;
    t.genre = genre;
    t.description = description;
    t.playback_count = playback_count;
    t.likes_count = likes_count;
    t.reposts_count = reposts_count;
    t.created_at = created_at;
    t.stream_url = stream_url;
    t.waveform_url = waveform_url;
    t.waveform_samples = waveform_samples;
    t.is_liked = is_liked;
    t.access_type = access_type;
    t.policy = policy;
    t.monetization_model = monetization_model;
    t.is_snipped = is_snipped;
    return t;
}

CachedTrackInfo CachedTrackInfo::from_track(
    const models::Track& track,
    const std::filesystem::path& path,
    uint64_t size,
    uint64_t last_accessed
) {
    CachedTrackInfo info;
    info.track_id = track.id;
    info.title = track.title;
    info.artist = track.artist;
    info.artist_id = track.artist_id;
    info.permalink_url = track.permalink_url;
    info.artwork_url = track.artwork_url;
    info.duration_ms = track.duration_ms;
    info.genre = track.genre;
    info.description = track.description;
    info.playback_count = track.playback_count;
    info.likes_count = track.likes_count;
    info.reposts_count = track.reposts_count;
    info.created_at = track.created_at;
    info.stream_url = track.stream_url;
    info.waveform_url = track.waveform_url;
    info.waveform_samples = track.waveform_samples;
    info.is_liked = track.is_liked;
    info.access_type = track.access_type;
    info.policy = track.policy;
    info.monetization_model = track.monetization_model;
    info.is_snipped = track.is_snipped;
    info.file_path = path;
    info.size_bytes = size;
    info.last_accessed = last_accessed;
    return info;
}

TrackCacheManager::TrackCacheManager(const std::filesystem::path& cache_dir)
    : cache_dir_(cache_dir),
      tracks_dir_(cache_dir / "tracks"),
      covers_dir_(cache_dir / "covers"),
      metadata_dir_(cache_dir / "metadata"),
      metadata_file_(tracks_dir_ / "metadata.json") {

    std::error_code ec;
    std::filesystem::create_directories(tracks_dir_, ec);
    std::filesystem::create_directories(covers_dir_, ec);
    std::filesystem::create_directories(metadata_dir_, ec);
    load_metadata();
}

std::filesystem::path TrackCacheManager::get_track_cache_path(uint64_t track_id) const {
    return tracks_dir_ / ("track_" + std::to_string(track_id) + ".mp3");
}

std::filesystem::path TrackCacheManager::get_cover_cache_path(uint64_t track_id) const {
    return covers_dir_ / ("cover_" + std::to_string(track_id) + ".jpg");
}

std::optional<std::filesystem::path> TrackCacheManager::get_cached_cover(uint64_t track_id) const {
    std::filesystem::path p = get_cover_cache_path(track_id);
    std::error_code ec;
    if (std::filesystem::exists(p, ec) && std::filesystem::file_size(p, ec) > 500) {
        return p;
    }
    return std::nullopt;
}

std::filesystem::path TrackCacheManager::get_metadata_cache_path(uint64_t track_id) const {
    return metadata_dir_ / ("track_" + std::to_string(track_id) + ".json");
}

void TrackCacheManager::download_cover_art(const models::Track& track) {
    if (track.artwork_url.empty()) return;

    std::filesystem::path dest = get_cover_cache_path(track.id);
    std::error_code ec;
    if (std::filesystem::exists(dest, ec) && std::filesystem::file_size(dest, ec) > 500) {
        return; // Already cached
    }

    std::string hi_res_url = CoverArtProcessor::upgrade_artwork_url(track.artwork_url);
    net::HttpClient client;
    auto res = client.get(hi_res_url);
    if (!res || !res->is_2xx() || res->body.size() < 500) {
        res = client.get(track.artwork_url);
    }

    if (res && res->is_2xx() && res->body.size() >= 500) {
        std::ofstream out(dest, std::ios::binary);
        if (out.is_open()) {
            out.write(reinterpret_cast<const char*>(res->body.data()), res->body.size());
            LOG_INFO("TrackCache", "Cached high-res cover art for track " + std::to_string(track.id) +
                     " (" + std::to_string(res->body.size() / 1024) + " KB)");
        }
    }
}

std::optional<std::filesystem::path> TrackCacheManager::get_cached_track(uint64_t track_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tracks_.find(track_id);
    std::filesystem::path path = get_track_cache_path(track_id);

    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        auto sz = std::filesystem::file_size(path, ec);
        if (sz > 1024) {
            uint64_t now_sec = current_epoch_seconds();
            if (it != tracks_.end()) {
                it->second.last_accessed = now_sec;
                it->second.size_bytes = sz;
            } else {
                CachedTrackInfo info;
                info.track_id = track_id;
                info.file_path = path;
                info.size_bytes = sz;
                info.last_accessed = now_sec;
                info.is_liked = false;
                tracks_[track_id] = std::move(info);
            }
            save_metadata();
            return path;
        }
    }

    if (it != tracks_.end()) {
        tracks_.erase(it);
        save_metadata();
    }
    return std::nullopt;
}

void TrackCacheManager::register_track(const models::Track& track) {
    std::filesystem::path path = get_track_cache_path(track.id);
    std::filesystem::path cover_dest = get_cover_cache_path(track.id);
    std::error_code ec;
    if (!std::filesystem::exists(cover_dest, ec) || std::filesystem::file_size(cover_dest, ec) < 500) {
        if (!track.artwork_url.empty()) {
            download_cover_art(track);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t sz = 0;
    if (std::filesystem::exists(path, ec)) {
        sz = std::filesystem::file_size(path, ec);
    }

    auto info = CachedTrackInfo::from_track(track, path, sz, current_epoch_seconds());
    if (std::filesystem::exists(cover_dest, ec)) {
        info.cover_file_path = cover_dest;
    }

    tracks_[track.id] = std::move(info);

    // Save individual metadata JSON file
    try {
        nlohmann::json item;
        item["track_id"] = track.id;
        item["title"] = track.title;
        item["artist"] = track.artist;
        item["artist_id"] = track.artist_id;
        item["permalink_url"] = track.permalink_url;
        item["artwork_url"] = track.artwork_url;
        item["duration_ms"] = track.duration_ms;
        item["genre"] = track.genre;
        item["description"] = track.description;
        item["playback_count"] = track.playback_count;
        item["likes_count"] = track.likes_count;
        item["reposts_count"] = track.reposts_count;
        item["created_at"] = track.created_at;
        item["stream_url"] = track.stream_url;
        item["waveform_url"] = track.waveform_url;
        item["is_liked"] = track.is_liked;
        item["access_type"] = static_cast<int>(track.access_type);
        item["policy"] = track.policy;
        item["monetization_model"] = track.monetization_model;
        item["is_snipped"] = track.is_snipped;
        item["file_path"] = path.string();
        item["cover_file_path"] = cover_dest.string();
        item["size_bytes"] = sz;

        std::ofstream mfile(get_metadata_cache_path(track.id));
        if (mfile.is_open()) {
            mfile << item.dump(2) << std::endl;
        }
    } catch (...) {}

    save_metadata();
    LOG_INFO("TrackCache", "Registered cached track with full metadata and cover: " + std::to_string(track.id) +
             " ('" + track.title + "' by " + track.artist + ")");
}

void TrackCacheManager::register_track(
    uint64_t track_id,
    const std::string& title,
    const std::string& artist,
    bool is_liked
) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::filesystem::path path = get_track_cache_path(track_id);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return;
    }

    uint64_t sz = std::filesystem::file_size(path, ec);
    if (sz <= 1024) {
        return;
    }

    auto& entry = tracks_[track_id];
    entry.track_id = track_id;
    entry.file_path = path;
    entry.size_bytes = sz;
    entry.last_accessed = current_epoch_seconds();
    if (!title.empty()) entry.title = title;
    if (!artist.empty()) entry.artist = artist;
    entry.is_liked = is_liked;

    save_metadata();
    LOG_INFO("TrackCache", "Registered cached track " + std::to_string(track_id) +
             " (" + std::to_string(sz / 1024) + " KB, liked=" + (is_liked ? "true" : "false") + ")");
}

std::optional<models::Track> TrackCacheManager::get_cached_track_metadata(uint64_t track_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tracks_.find(track_id);
    if (it != tracks_.end()) {
        return it->second.to_track();
    }
    return std::nullopt;
}

bool TrackCacheManager::is_track_cached(uint64_t track_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tracks_.contains(track_id)) return false;
    std::error_code ec;
    const auto path = get_track_cache_path(track_id);
    return std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) > 1024;
}

void TrackCacheManager::update_track_liked(uint64_t track_id, bool is_liked) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tracks_.find(track_id);
    if (it != tracks_.end()) {
        it->second.is_liked = is_liked;
        save_metadata();
    }
}

void TrackCacheManager::sync_liked_tracks(const std::vector<uint64_t>& liked_ids) {
    const std::unordered_set<uint64_t> liked_set(liked_ids.begin(), liked_ids.end());
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, info] : tracks_) {
        info.is_liked = liked_set.contains(id);
    }
    save_metadata();
}

std::vector<uint64_t> TrackCacheManager::get_liked_track_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint64_t> liked;
    for (const auto& [id, info] : tracks_) {
        if (info.is_liked) {
            liked.push_back(id);
        }
    }
    return liked;
}

void TrackCacheManager::enforce_cache_limits(
    size_t max_cache_size_mb,
    bool lock_liked,
    bool dont_count_liked
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (max_cache_size_mb == 0) {
        return; // Unlimited cache
    }

    uint64_t max_allowed_bytes = static_cast<uint64_t>(max_cache_size_mb) * 1024 * 1024;

    auto calculate_counted_bytes = [&]() -> uint64_t {
        uint64_t sum = 0;
        for (const auto& [id, t] : tracks_) {
            if (dont_count_liked && lock_liked && t.is_liked) {
                continue; // Do not count locked liked tracks
            }
            sum += t.size_bytes;
        }
        return sum;
    };

    uint64_t current_counted = calculate_counted_bytes();
    if (current_counted <= max_allowed_bytes) {
        return;
    }

    // Eviction loop: sort evictable tracks by last_accessed ascending (oldest first)
    while (current_counted > max_allowed_bytes) {
        uint64_t oldest_access = UINT64_MAX;
        uint64_t best_id = 0;
        bool found = false;

        for (const auto& [id, t] : tracks_) {
            // Never evict locked liked tracks
            if (lock_liked && t.is_liked) {
                continue;
            }
            if (t.last_accessed < oldest_access) {
                oldest_access = t.last_accessed;
                best_id = id;
                found = true;
            }
        }

        if (!found) {
            // No evictable regular tracks remain
            break;
        }

        auto it = tracks_.find(best_id);
        if (it != tracks_.end()) {
            std::error_code ec;
            std::filesystem::remove(get_track_cache_path(best_id), ec);
            std::filesystem::remove(get_cover_cache_path(best_id), ec);
            std::filesystem::remove(get_metadata_cache_path(best_id), ec);
            LOG_INFO("TrackCache", "Evicted old cached track " + std::to_string(best_id) +
                     " (" + std::to_string(it->second.size_bytes / 1024) + " KB)");
            tracks_.erase(it);
        }

        current_counted = calculate_counted_bytes();
    }

    save_metadata();
}

void TrackCacheManager::clear_cache(bool keep_liked) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint64_t> to_remove;

    for (const auto& [id, t] : tracks_) {
        if (keep_liked && t.is_liked) {
            continue;
        }
        std::error_code ec;
        std::filesystem::remove(get_track_cache_path(id), ec);
        std::filesystem::remove(get_cover_cache_path(id), ec);
        std::filesystem::remove(get_metadata_cache_path(id), ec);
        to_remove.push_back(id);
    }

    for (uint64_t id : to_remove) {
        tracks_.erase(id);
    }

    save_metadata();
    LOG_INFO("TrackCache", "Cleared cache (keep_liked=" + std::string(keep_liked ? "true" : "false") + ")");
}

CacheStats TrackCacheManager::get_stats(
    size_t max_cache_size_mb,
    bool lock_liked,
    bool dont_count_liked
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheStats stats;
    stats.max_size_bytes = static_cast<uint64_t>(max_cache_size_mb) * 1024 * 1024;
    stats.total_tracks = tracks_.size();

    for (const auto& [id, t] : tracks_) {
        stats.total_size_bytes += t.size_bytes;
        if (t.is_liked) {
            stats.liked_tracks++;
            stats.liked_size_bytes += t.size_bytes;
            if (!dont_count_liked || !lock_liked) {
                stats.regular_size_bytes += t.size_bytes;
            }
        } else {
            stats.regular_tracks++;
            stats.regular_size_bytes += t.size_bytes;
        }
    }

    return stats;
}

void TrackCacheManager::load_metadata() {
    std::error_code ec;
    if (!std::filesystem::exists(metadata_file_, ec)) {
        // If metadata.json does not exist, recover existing tracks from disk
        if (std::filesystem::exists(tracks_dir_, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(tracks_dir_, ec)) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".mp3") {
                    std::string stem = entry.path().stem().string();
                    if (stem.find("track_") == 0) {
                        try {
                            uint64_t tid = std::stoull(stem.substr(6));
                            CachedTrackInfo info;
                            info.track_id = tid;
                            info.file_path = entry.path();
                            info.size_bytes = entry.file_size(ec);
                            info.last_accessed = current_epoch_seconds();
                            tracks_[tid] = std::move(info);
                        } catch (...) {}
                    }
                }
            }
        }
        save_metadata();
        return;
    }

    try {
        std::ifstream file(metadata_file_);
        if (!file.is_open()) return;

        nlohmann::json j;
        file >> j;

        if (j.contains("tracks") && j["tracks"].is_object()) {
            for (auto& [key, item] : j["tracks"].items()) {
                CachedTrackInfo info;
                info.track_id = item.value("track_id", 0ULL);
                info.title = item.value("title", "");
                info.artist = item.value("artist", "");
                info.artist_id = item.value("artist_id", 0ULL);
                info.permalink_url = item.value("permalink_url", "");
                info.artwork_url = item.value("artwork_url", "");
                info.duration_ms = item.value("duration_ms", 0ULL);
                info.genre = item.value("genre", "");
                info.description = item.value("description", "");
                info.playback_count = item.value("playback_count", 0ULL);
                info.likes_count = item.value("likes_count", 0ULL);
                info.reposts_count = item.value("reposts_count", 0ULL);
                info.created_at = item.value("created_at", "");
                info.stream_url = item.value("stream_url", "");
                info.waveform_url = item.value("waveform_url", "");
                if (item.contains("waveform_samples") && item["waveform_samples"].is_array()) {
                    info.waveform_samples = item["waveform_samples"].get<std::vector<float>>();
                }
                info.is_liked = item.value("is_liked", false);
                info.access_type = static_cast<models::TrackAccessType>(item.value("access_type", 0));
                info.policy = item.value("policy", "");
                info.monetization_model = item.value("monetization_model", "");
                info.is_snipped = item.value("is_snipped", false);

                info.file_path = get_track_cache_path(info.track_id);
                auto cov = get_cover_cache_path(info.track_id);
                if (std::filesystem::exists(cov, ec)) {
                    info.cover_file_path = cov;
                }
                info.last_accessed = item.value("last_accessed", 0ULL);

                if (std::filesystem::is_regular_file(info.file_path, ec)) {
                    info.size_bytes = std::filesystem::file_size(info.file_path, ec);
                    if (!ec && info.size_bytes > 1024) {
                        tracks_[info.track_id] = std::move(info);
                    }
                }
            }
        }
    } catch (...) {}
}

void TrackCacheManager::save_metadata() {
    try {
        nlohmann::json j;
        j["tracks"] = nlohmann::json::object();

        for (const auto& [id, t] : tracks_) {
            nlohmann::json item;
            item["track_id"] = t.track_id;
            item["title"] = t.title;
            item["artist"] = t.artist;
            item["artist_id"] = t.artist_id;
            item["permalink_url"] = t.permalink_url;
            item["artwork_url"] = t.artwork_url;
            item["duration_ms"] = t.duration_ms;
            item["genre"] = t.genre;
            item["description"] = t.description;
            item["playback_count"] = t.playback_count;
            item["likes_count"] = t.likes_count;
            item["reposts_count"] = t.reposts_count;
            item["created_at"] = t.created_at;
            item["stream_url"] = t.stream_url;
            item["waveform_url"] = t.waveform_url;
            item["waveform_samples"] = t.waveform_samples;
            item["is_liked"] = t.is_liked;
            item["access_type"] = static_cast<int>(t.access_type);
            item["policy"] = t.policy;
            item["monetization_model"] = t.monetization_model;
            item["is_snipped"] = t.is_snipped;
            item["file_path"] = t.file_path.string();
            item["cover_file_path"] = t.cover_file_path.string();
            item["size_bytes"] = t.size_bytes;
            item["last_accessed"] = t.last_accessed;

            j["tracks"][std::to_string(id)] = std::move(item);
        }

        std::filesystem::path temp_file = metadata_file_;
        temp_file += ".tmp";
        {
            std::ofstream file(temp_file, std::ios::trunc);
            if (!file.is_open()) return;
            file << j.dump(2) << '\n';
            file.flush();
            if (!file) {
                file.close();
                std::error_code ec;
                std::filesystem::remove(temp_file, ec);
                return;
            }
        }

        std::error_code ec;
        std::filesystem::rename(temp_file, metadata_file_, ec);
        if (ec) std::filesystem::remove(temp_file, ec);
    } catch (...) {}
}

} // namespace sctui::core
