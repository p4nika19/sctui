#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <optional>
#include <array>
#include <chrono>
#include <memory>
#include <random>
#include <numeric>
#include <algorithm>
#include <ftxui/screen/color.hpp>
#include "sctui/models/track.hpp"
#include "sctui/models/user.hpp"
#include "sctui/core/config.hpp"
#include "sctui/core/entropy.hpp"
#include "sctui/audio/audio_engine.hpp"

namespace sctui::models {

enum class ViewMode {
    Search = 0,
    Stream,
    Likes,
    Playlists,
    Queue,
    Account,
    Settings
};

enum class FocusPanel {
    SearchInput = 0,
    Sidebar,
    TrackList,
    QueueList,
    Player,
    HelpModal,
    SettingsPanel,
    AccountPanel,
    Onboarding
};

struct Notification {
    std::string message;
    bool is_error{false};
    std::chrono::steady_clock::time_point expires_at;
};

struct QueueState {
    std::vector<Track> tracks;
    int current_index{-1};
    core::RepeatMode repeat_mode{core::RepeatMode::Off};
    bool shuffle{false};
    std::vector<size_t> shuffle_indices;
    size_t shuffle_cursor{0};

    [[nodiscard]] std::optional<Track> current_track() const {
        if (current_index >= 0 && static_cast<size_t>(current_index) < tracks.size()) {
            return tracks[static_cast<size_t>(current_index)];
        }
        return std::nullopt;
    }

    void rebuild_shuffle() {
        if (tracks.empty()) {
            shuffle_indices.clear();
            shuffle_cursor = 0;
            return;
        }

        shuffle_indices.resize(tracks.size());
        std::iota(shuffle_indices.begin(), shuffle_indices.end(), 0);

        if (tracks.size() > 1) {
            // Cryptographic-grade entropy engine combining /dev/urandom, CPU RDRAND, and high-res clock jitter
            auto rng = core::AdvancedRng::create_engine();
            std::shuffle(shuffle_indices.begin(), shuffle_indices.end(), rng);

            // Anchor the currently playing track at position 0 so playback continues without interruption
            if (current_index >= 0 && static_cast<size_t>(current_index) < tracks.size()) {
                auto it = std::find(shuffle_indices.begin(), shuffle_indices.end(), static_cast<size_t>(current_index));
                if (it != shuffle_indices.end() && it != shuffle_indices.begin()) {
                    std::iter_swap(shuffle_indices.begin(), it);
                }
            }
        }
        shuffle_cursor = 0;
    }

    void append_tracks_and_update_shuffle(const std::vector<Track>& new_tracks) {
        if (new_tracks.empty()) return;
        size_t old_size = tracks.size();
        tracks.insert(tracks.end(), new_tracks.begin(), new_tracks.end());
        if (shuffle) {
            std::vector<size_t> new_indices;
            new_indices.reserve(new_tracks.size());
            for (size_t i = old_size; i < tracks.size(); ++i) {
                new_indices.push_back(i);
            }
            if (new_indices.size() > 1) {
                auto rng = core::AdvancedRng::create_engine();
                std::shuffle(new_indices.begin(), new_indices.end(), rng);
            }
            shuffle_indices.insert(shuffle_indices.end(), new_indices.begin(), new_indices.end());
        }
    }

    void queue_track(const Track& track, bool play_next) {
        if (tracks.empty()) {
            tracks.push_back(track);
            current_index = 0;
            if (shuffle) {
                shuffle_indices = {0};
                shuffle_cursor = 0;
            }
            return;
        }

        if (play_next && current_index >= 0 && current_index < static_cast<int>(tracks.size())) {
            size_t insert_pos = static_cast<size_t>(current_index + 1);
            tracks.insert(tracks.begin() + insert_pos, track);

            if (shuffle) {
                if (shuffle_indices.size() != tracks.size() - 1) {
                    rebuild_shuffle();
                } else {
                    for (auto& idx : shuffle_indices) {
                        if (idx >= insert_pos) {
                            idx++;
                        }
                    }
                    size_t next_shuf_pos = std::min(shuffle_cursor + 1, shuffle_indices.size());
                    shuffle_indices.insert(shuffle_indices.begin() + next_shuf_pos, insert_pos);
                }
            }
        } else {
            size_t append_pos = tracks.size();
            tracks.push_back(track);
            if (shuffle) {
                if (shuffle_indices.size() != tracks.size() - 1) {
                    rebuild_shuffle();
                } else {
                    shuffle_indices.push_back(append_pos);
                }
            }
        }
    }

    bool move_to_next(int from_index) {
        if (tracks.empty() || from_index < 0 || static_cast<size_t>(from_index) >= tracks.size()) {
            return false;
        }

        if (shuffle && shuffle_indices.size() == tracks.size()) {
            size_t old_pos = static_cast<size_t>(from_index);
            if (old_pos == shuffle_cursor) return false;
            size_t target_pos = shuffle_cursor + 1;
            if (target_pos >= shuffle_indices.size()) target_pos = shuffle_indices.size() - 1;
            if (old_pos == target_pos) return false;

            size_t val = shuffle_indices[old_pos];
            shuffle_indices.erase(shuffle_indices.begin() + old_pos);
            if (old_pos < target_pos) {
                target_pos--;
            }
            shuffle_indices.insert(shuffle_indices.begin() + target_pos, val);
            if (shuffle_cursor > old_pos && shuffle_cursor <= target_pos) {
                shuffle_cursor--;
            }
            return true;
        } else {
            int cur_idx = current_index;
            int target_idx = (cur_idx < 0) ? 0 : (cur_idx + 1);
            if (target_idx >= static_cast<int>(tracks.size())) {
                target_idx = static_cast<int>(tracks.size() - 1);
            }
            if (from_index == cur_idx || from_index == target_idx) {
                return false;
            }

            Track t = std::move(tracks[static_cast<size_t>(from_index)]);
            tracks.erase(tracks.begin() + from_index);
            if (from_index < target_idx) {
                target_idx--;
            }
            tracks.insert(tracks.begin() + target_idx, std::move(t));

            if (from_index < cur_idx && target_idx >= cur_idx) {
                current_index--;
            } else if (from_index > cur_idx && target_idx <= cur_idx) {
                current_index++;
            }
            return true;
        }
    }

    void set_current_index(int index) {
        current_index = index;
        if (shuffle) {
            if (shuffle_indices.size() != tracks.size()) {
                rebuild_shuffle();
            }
            if (index >= 0 && static_cast<size_t>(index) < tracks.size()) {
                if (shuffle_cursor < shuffle_indices.size() && shuffle_indices[shuffle_cursor] == static_cast<size_t>(index)) {
                    return;
                }
                auto it = std::find(shuffle_indices.begin(), shuffle_indices.end(), static_cast<size_t>(index));
                if (it != shuffle_indices.end()) {
                    shuffle_cursor = static_cast<size_t>(std::distance(shuffle_indices.begin(), it));
                }
            }
        }
    }

    void toggle_shuffle() {
        shuffle = !shuffle;
        if (shuffle) {
            rebuild_shuffle();
        }
    }

    void advance_next() {
        if (tracks.empty()) return;
        if (repeat_mode == core::RepeatMode::One) {
            // Keep current_index
            return;
        }
        if (shuffle) {
            if (shuffle_indices.size() != tracks.size()) {
                rebuild_shuffle();
            }
            if (!shuffle_indices.empty()) {
                if (shuffle_cursor + 1 < shuffle_indices.size()) {
                    shuffle_cursor++;
                    current_index = static_cast<int>(shuffle_indices[shuffle_cursor]);
                } else if (repeat_mode == core::RepeatMode::All) {
                    rebuild_shuffle();
                    current_index = static_cast<int>(shuffle_indices[0]);
                } else {
                    current_index = -1; // Finished queue
                }
                return;
            }
        }
        if (current_index + 1 < static_cast<int>(tracks.size())) {
            current_index++;
        } else if (repeat_mode == core::RepeatMode::All) {
            current_index = 0;
        } else {
            current_index = -1; // Finished
        }
    }

    void advance_prev() {
        if (tracks.empty()) return;
        if (repeat_mode == core::RepeatMode::One) {
            return;
        }
        if (shuffle) {
            if (shuffle_indices.size() != tracks.size()) {
                rebuild_shuffle();
            }
            if (!shuffle_indices.empty()) {
                if (shuffle_cursor > 0) {
                    shuffle_cursor--;
                } else if (repeat_mode == core::RepeatMode::All) {
                    shuffle_cursor = shuffle_indices.size() - 1;
                }
                current_index = static_cast<int>(shuffle_indices[shuffle_cursor]);
                return;
            }
        }
        if (current_index > 0) {
            current_index--;
        } else if (repeat_mode == core::RepeatMode::All) {
            current_index = static_cast<int>(tracks.size() - 1);
        }
    }
};

struct AppState {
    ViewMode current_view{ViewMode::Search};
    FocusPanel focused_panel{FocusPanel::SearchInput};

    // Navigation & Viewport Virtual Scrolling
    std::string search_query;
    bool is_loading{false};
    std::string status_text{"Ready"};
    std::optional<Notification> notification;

    // Data lists for views
    std::vector<Track> search_results;
    int search_selected_index{0};

    std::vector<Track> stream_tracks;
    int stream_selected_index{0};

    std::vector<Track> likes_tracks;
    int likes_selected_index{0};
    std::unordered_set<uint64_t> liked_track_ids;

    [[nodiscard]] bool is_track_liked(uint64_t track_id) const noexcept {
        return liked_track_ids.find(track_id) != liked_track_ids.end();
    }

    std::vector<Playlist> playlists;
    int playlist_selected_index{0};

    std::optional<Playlist> selected_playlist;
    std::vector<Track> playlist_tracks;
    int playlist_tracks_selected_index{0};

    // Current logged-in user profile & own tracks
    std::optional<UserProfile> user_profile;
    std::vector<Track> user_tracks;
    int user_tracks_selected_index{0};

    // Queue & Playback
    QueueState queue;
    std::optional<Track> active_track;
    audio::PlaybackState playback_state{audio::PlaybackState::Stopped};
    uint64_t position_ms{0};
    uint64_t duration_ms{0};
    float buffer_percent{0.0f};
    float volume_percent{75.0f};
    bool is_muted{false};
    float pre_mute_volume{75.0f};
    std::array<float, audio::VisualizerBandsCount> visualizer_bars{};

    // Terminal geometry & breakpoints
    int terminal_width{120};
    int terminal_height{35};

    // Dynamic theme & album cover art
    ColorRGB accent_color{255, 85, 0};
    std::optional<CoverArtImage> current_cover_art;
    core::IconStyle icon_style{core::IconStyle::Emoji};
    bool mouse_control{false};
    bool is_fullscreen{false};
    bool is_onboarding{false};
    bool drm_engine_enabled{true};
    bool download_drm_tracks_in_likes{true};
    bool is_downloading_electron{false};

    // Likes cache downloading progress
    bool is_downloading_likes{false};
    size_t likes_download_current{0};
    size_t likes_download_total{0};

    // Discord Rich Presence status
    bool discord_rpc_enabled{true};

    [[nodiscard]] ftxui::Color get_accent_color() const noexcept {
        return ftxui::Color::RGB(accent_color.r, accent_color.g, accent_color.b);
    }

    [[nodiscard]] bool is_compact() const noexcept {
        return terminal_width < 90 || terminal_height < 25;
    }

    [[nodiscard]] bool is_ultrawide() const noexcept {
        return terminal_width >= 160;
    }

    [[nodiscard]] bool user_has_go() const noexcept {
        return user_profile.has_value() && user_profile->has_go();
    }

    [[nodiscard]] bool user_has_go_plus() const noexcept {
        return user_profile.has_value() && user_profile->has_go_plus();
    }

    void notify(std::string msg, bool is_error = false, int timeout_sec = 4) {
        notification = Notification{
            .message = std::move(msg),
            .is_error = is_error,
            .expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec)
        };
    }
};

} // namespace sctui::models
