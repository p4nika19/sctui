#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <functional>
#include <vector>
#include "sctui/models/track.hpp"
#include "sctui/models/state.hpp"

namespace sctui::ui::views {

class TrackListView {
public:
    using PlayCallback = std::function<void(const models::Track& track, size_t index)>;
    using QueueCallback = std::function<void(const models::Track& track, bool play_next)>;
    using LikeCallback = std::function<void(const models::Track& track)>;
    using PlaylistCallback = std::function<void(const models::Playlist& playlist)>;

    TrackListView(
        models::AppState& state,
        PlayCallback on_play,
        QueueCallback on_queue,
        LikeCallback on_like,
        PlaylistCallback on_select_playlist = nullptr
    );

    ftxui::Component get_component();
    ftxui::Element render(bool is_focused);

    void scroll_up();
    void scroll_down();
    void select_row(int visible_row);
    void play_selected();
    void queue_selected(bool play_next);
    void toggle_like_selected();

    [[nodiscard]] const std::vector<models::Track>& get_current_track_list() const;
    [[nodiscard]] const std::vector<models::Playlist>& get_current_playlist_list() const;
    [[nodiscard]] int get_current_selected_index() const;
    void set_current_selected_index(int index);

    [[nodiscard]] bool is_searching() const noexcept { return search_active_; }
    void toggle_search();
    void close_search();
    void finish_search_typing();

private:
    models::AppState& state_;
    PlayCallback on_play_;
    QueueCallback on_queue_;
    LikeCallback on_like_;
    PlaylistCallback on_select_playlist_;
    ftxui::Component component_;
    ftxui::Component search_input_comp_;
    int scroll_offset_{0};

    bool search_active_{false};
    bool search_input_focused_{false};
    std::string search_query_;
    int search_cursor_pos_{0};

    mutable std::vector<models::Track> filtered_tracks_;
    mutable std::vector<models::Playlist> filtered_playlists_;
    mutable std::vector<size_t> filtered_original_indices_;
    mutable std::string last_cached_query_;
    mutable models::ViewMode last_cached_view_{models::ViewMode::Search};
    mutable uint64_t last_cached_playlist_id_{0};
};

} // namespace sctui::ui::views
