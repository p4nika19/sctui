#include "sctui/ui/views/track_list_view.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include <ftxui/component/event.hpp>
#include <algorithm>

namespace sctui::ui::views {

using namespace ftxui;

namespace {

Element render_like_badge(bool is_liked, core::IconStyle icon_style) {
    if (!is_liked) {
        return text("    ") | size(WIDTH, EQUAL, 4);
    }
    const auto& sym = ThemeSymbols::get(icon_style);
    std::string badge_str;
    switch (icon_style) {
        case core::IconStyle::Emoji:
            badge_str = sym.likes + "  ";
            break;
        case core::IconStyle::Unicode:
            badge_str = " " + sym.likes + "  ";
            break;
        case core::IconStyle::DefaultAscii:
            badge_str = sym.likes + "  ";
            break;
    }
    return text(badge_str) | color(Color::RGB(255, 65, 105)) | bold | size(WIDTH, EQUAL, 4);
}

} // namespace

TrackListView::TrackListView(
    models::AppState& state,
    PlayCallback on_play,
    QueueCallback on_queue,
    LikeCallback on_like,
    PlaylistCallback on_select_playlist
) : state_(state),
    on_play_(std::move(on_play)),
    on_queue_(std::move(on_queue)),
    on_like_(std::move(on_like)),
    on_select_playlist_(std::move(on_select_playlist)) {

    InputOption search_opt;
    search_opt.multiline = false;
    search_opt.cursor_position = &search_cursor_pos_;
    search_opt.on_change = [this]() {
        search_query_.erase(
            std::remove(search_query_.begin(), search_query_.end(), '\n'),
            search_query_.end()
        );
        search_query_.erase(
            std::remove(search_query_.begin(), search_query_.end(), '\r'),
            search_query_.end()
        );
        set_current_selected_index(0);
        scroll_offset_ = 0;
    };
    search_input_comp_ = Input(&search_query_, "Type to search...", search_opt);

    component_ = CatchEvent(Make<ComponentBase>(), [this](Event event) {
        if (state_.focused_panel != models::FocusPanel::TrackList) {
            return false;
        }

        bool is_alt_slash = (event == Event::Special("\x1b/") || event.input() == "\x1b/");
        if (is_alt_slash) {
            if (state_.current_view == models::ViewMode::Likes || state_.current_view == models::ViewMode::Playlists) {
                toggle_search();
                return true;
            }
        }

        if (search_active_ && search_input_focused_) {
            if (event == Event::Escape) {
                close_search();
                return true;
            }
            if (event == Event::Return) {
                play_selected();
                return true;
            }
            if (event == Event::ArrowDown) {
                scroll_down();
                return true;
            }
            if (event == Event::ArrowUp) {
                scroll_up();
                return true;
            }
            if (event == Event::PageDown) {
                set_current_selected_index(get_current_selected_index() + 10);
                return true;
            }
            if (event == Event::PageUp) {
                set_current_selected_index(get_current_selected_index() - 10);
                return true;
            }
            return search_input_comp_->OnEvent(event);
        }

        if (event == Event::Character('j') || event == Event::ArrowDown) {
            scroll_down();
            return true;
        }
        if (event == Event::Character('k') || event == Event::ArrowUp) {
            scroll_up();
            return true;
        }
        if (event == Event::Return) {
            play_selected();
            return true;
        }
        if (event == Event::Escape || event == Event::Character('h') || event == Event::ArrowLeft) {
            if (search_active_) {
                close_search();
                return true;
            }
            if (state_.current_view == models::ViewMode::Playlists && state_.selected_playlist.has_value()) {
                state_.selected_playlist.reset();
                state_.playlist_tracks.clear();
                return true;
            }
            state_.focused_panel = models::FocusPanel::Sidebar;
            return true;
        }
        if (event == Event::Character('l') && state_.current_view == models::ViewMode::Playlists && !state_.selected_playlist.has_value()) {
            play_selected();
            return true;
        }
        if (event == Event::Character('a')) {
            queue_selected(false);
            return true;
        }
        if (event == Event::Character('A') || event == Event::CtrlN) {
            queue_selected(true);
            return true;
        }
        if (event == Event::Character('l')) {
            toggle_like_selected();
            return true;
        }
        if (event == Event::Character('g') || event == Event::Home) {
            set_current_selected_index(0);
            return true;
        }
        if (event == Event::Character('G') || event == Event::End) {
            if (state_.current_view == models::ViewMode::Playlists && !state_.selected_playlist.has_value()) {
                const auto& pls = get_current_playlist_list();
                if (!pls.empty()) {
                    set_current_selected_index(static_cast<int>(pls.size() - 1));
                }
            } else {
                const auto& tracks = get_current_track_list();
                if (!tracks.empty()) {
                    set_current_selected_index(static_cast<int>(tracks.size() - 1));
                }
            }
            return true;
        }

        return false;
    });
}

Component TrackListView::get_component() {
    return component_;
}

void TrackListView::toggle_search() {
    if (state_.current_view != models::ViewMode::Likes && state_.current_view != models::ViewMode::Playlists) {
        return;
    }
    search_active_ = !search_active_;
    search_input_focused_ = search_active_;
    if (!search_active_) {
        search_query_.clear();
        search_cursor_pos_ = 0;
    } else {
        search_input_comp_->TakeFocus();
    }
    set_current_selected_index(0);
    scroll_offset_ = 0;
}

void TrackListView::close_search() {
    search_active_ = false;
    search_input_focused_ = false;
    search_query_.clear();
    search_cursor_pos_ = 0;
    set_current_selected_index(0);
    scroll_offset_ = 0;
}

void TrackListView::finish_search_typing() {
    search_input_focused_ = false;
}

const std::vector<models::Playlist>& TrackListView::get_current_playlist_list() const {
    if (state_.current_view == models::ViewMode::Playlists && !state_.selected_playlist.has_value()) {
        if (!search_active_ || search_query_.empty()) {
            return state_.playlists;
        }
        if (last_cached_query_ == search_query_ && last_cached_view_ == state_.current_view) {
            return filtered_playlists_;
        }
        filtered_playlists_.clear();
        std::string q = search_query_;
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
        for (const auto& pl : state_.playlists) {
            std::string t = pl.title;
            std::transform(t.begin(), t.end(), t.begin(), ::tolower);
            if (t.find(q) != std::string::npos) {
                filtered_playlists_.push_back(pl);
            }
        }
        last_cached_query_ = search_query_;
        last_cached_view_ = state_.current_view;
        return filtered_playlists_;
    }
    static const std::vector<models::Playlist> empty_playlists;
    return empty_playlists;
}

const std::vector<models::Track>& TrackListView::get_current_track_list() const {
    const std::vector<models::Track>* raw_list = nullptr;
    switch (state_.current_view) {
        case models::ViewMode::Search:
            raw_list = &state_.search_results;
            break;
        case models::ViewMode::Stream:
            raw_list = &state_.stream_tracks;
            break;
        case models::ViewMode::Likes:
            raw_list = &state_.likes_tracks;
            break;
        case models::ViewMode::Playlists:
            raw_list = &state_.playlist_tracks;
            break;
        case models::ViewMode::Queue:
            raw_list = &state_.queue.tracks;
            break;
        case models::ViewMode::Account:
        case models::ViewMode::Settings:
            break;
    }

    if (!raw_list) {
        static const std::vector<models::Track> empty_tracks;
        return empty_tracks;
    }

    if ((state_.current_view == models::ViewMode::Likes ||
         (state_.current_view == models::ViewMode::Playlists && state_.selected_playlist.has_value())) &&
        search_active_ && !search_query_.empty()) {

        uint64_t cur_pl_id = state_.selected_playlist ? state_.selected_playlist->id : 0;
        if (last_cached_query_ == search_query_ &&
            last_cached_view_ == state_.current_view &&
            last_cached_playlist_id_ == cur_pl_id) {
            return filtered_tracks_;
        }

        filtered_tracks_.clear();
        filtered_original_indices_.clear();
        std::string q = search_query_;
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);

        for (size_t i = 0; i < raw_list->size(); ++i) {
            const auto& t = (*raw_list)[i];
            std::string title_l = t.title;
            std::transform(title_l.begin(), title_l.end(), title_l.begin(), ::tolower);
            std::string artist_l = t.artist;
            std::transform(artist_l.begin(), artist_l.end(), artist_l.begin(), ::tolower);
            std::string genre_l = t.genre;
            std::transform(genre_l.begin(), genre_l.end(), genre_l.begin(), ::tolower);

            if (title_l.find(q) != std::string::npos ||
                artist_l.find(q) != std::string::npos ||
                genre_l.find(q) != std::string::npos) {
                filtered_tracks_.push_back(t);
                filtered_original_indices_.push_back(i);
            }
        }
        last_cached_query_ = search_query_;
        last_cached_view_ = state_.current_view;
        last_cached_playlist_id_ = cur_pl_id;
        return filtered_tracks_;
    }

    return *raw_list;
}

int TrackListView::get_current_selected_index() const {
    switch (state_.current_view) {
        case models::ViewMode::Search:
            return state_.search_selected_index;
        case models::ViewMode::Stream:
            return state_.stream_selected_index;
        case models::ViewMode::Likes:
            return state_.likes_selected_index;
        case models::ViewMode::Playlists:
            if (state_.selected_playlist.has_value()) {
                return state_.playlist_tracks_selected_index;
            }
            return state_.playlist_selected_index;
        case models::ViewMode::Queue:
            return state_.queue.current_index >= 0 ? state_.queue.current_index : 0;
        case models::ViewMode::Account:
        case models::ViewMode::Settings:
            return 0;
    }
    return 0;
}

void TrackListView::set_current_selected_index(int index) {
    if (state_.current_view == models::ViewMode::Playlists && !state_.selected_playlist.has_value()) {
        const auto& pls = get_current_playlist_list();
        if (pls.empty()) return;
        state_.playlist_selected_index = std::clamp(index, 0, static_cast<int>(pls.size() - 1));
        return;
    }

    const auto& tracks = get_current_track_list();
    if (tracks.empty()) return;
    int clamped = std::clamp(index, 0, static_cast<int>(tracks.size() - 1));

    switch (state_.current_view) {
        case models::ViewMode::Search:
            state_.search_selected_index = clamped;
            break;
        case models::ViewMode::Stream:
            state_.stream_selected_index = clamped;
            break;
        case models::ViewMode::Likes:
            state_.likes_selected_index = clamped;
            break;
        case models::ViewMode::Playlists:
            state_.playlist_tracks_selected_index = clamped;
            break;
        case models::ViewMode::Queue:
        case models::ViewMode::Account:
        case models::ViewMode::Settings:
            break;
    }
}

void TrackListView::scroll_down() {
    set_current_selected_index(get_current_selected_index() + 1);
}

void TrackListView::scroll_up() {
    set_current_selected_index(get_current_selected_index() - 1);
}

void TrackListView::select_row(int visible_row) {
    int target_idx = scroll_offset_ + visible_row;
    if (state_.current_view == models::ViewMode::Playlists && !state_.selected_playlist.has_value()) {
        const auto& pls = get_current_playlist_list();
        if (target_idx >= 0 && target_idx < static_cast<int>(pls.size())) {
            set_current_selected_index(target_idx);
            play_selected();
        }
    } else {
        const auto& tracks = get_current_track_list();
        if (target_idx >= 0 && target_idx < static_cast<int>(tracks.size())) {
            set_current_selected_index(target_idx);
            play_selected();
        }
    }
}

void TrackListView::play_selected() {
    if (state_.current_view == models::ViewMode::Playlists && !state_.selected_playlist.has_value()) {
        const auto& pls = get_current_playlist_list();
        if (on_select_playlist_ && !pls.empty()) {
            int idx = std::clamp(state_.playlist_selected_index, 0, static_cast<int>(pls.size() - 1));
            on_select_playlist_(pls[static_cast<size_t>(idx)]);
            close_search();
        }
        return;
    }

    const auto& tracks = get_current_track_list();
    int idx = get_current_selected_index();
    if (idx >= 0 && static_cast<size_t>(idx) < tracks.size()) {
        if (on_play_) {
            on_play_(tracks[static_cast<size_t>(idx)], static_cast<size_t>(idx));
        }
    }
}

void TrackListView::queue_selected(bool play_next) {
    const auto& tracks = get_current_track_list();
    int idx = get_current_selected_index();
    if (idx >= 0 && static_cast<size_t>(idx) < tracks.size()) {
        if (on_queue_) {
            on_queue_(tracks[static_cast<size_t>(idx)], play_next);
        }
    }
}

void TrackListView::toggle_like_selected() {
    const auto& tracks = get_current_track_list();
    int idx = get_current_selected_index();
    if (idx >= 0 && static_cast<size_t>(idx) < tracks.size()) {
        if (on_like_) {
            on_like_(tracks[static_cast<size_t>(idx)]);
        }
    }
}

Element TrackListView::render(bool is_focused) {
    auto accent = state_.get_accent_color();
    auto border_col = is_focused ? accent : Color::GrayDark;

    if (state_.current_view == models::ViewMode::Playlists && !state_.selected_playlist.has_value()) {
        const auto& playlists = get_current_playlist_list();

        Element middle_elem = filler();
        if (search_active_) {
            middle_elem = hbox({
                text(" 🔍 ") | color(accent),
                search_input_comp_->Render() | xflex,
                text(" [Esc to close] ") | color(Color::GrayDark) | dim
            }) | borderRounded | color(accent) | size(WIDTH, GREATER_THAN, 28) | size(WIDTH, LESS_THAN, 48);
        } else {
            middle_elem = text(" [alt+/ to search in playlists] ") | color(Color::RGB(100, 100, 110)) | dim;
        }

        if (playlists.empty()) {
            std::string empty_msg = state_.is_loading ? "Loading playlists..." : "No playlists found for your account.";
            if (search_active_ && !search_query_.empty()) {
                empty_msg = "No playlists matching '" + search_query_ + "'. Press Esc to clear.";
            }
            auto empty_box = vbox({
                filler(),
                text(empty_msg) | color(Color::GrayLight) | center,
                filler()
            });
            return vbox({
                hbox({
                    text(" PLAYLISTS ") | color(border_col) | bold,
                    filler(),
                    middle_elem,
                    filler(),
                    text(" 0/0 ") | color(Color::GrayDark)
                }),
                separator(),
                empty_box | yflex
            }) | borderRounded | color(border_col) | yflex;
        }

        int viewport_height = std::max(3, state_.terminal_height - 16);
        int selected_idx = std::clamp(state_.playlist_selected_index, 0, static_cast<int>(playlists.size() - 1));
        if (selected_idx < scroll_offset_) {
            scroll_offset_ = selected_idx;
        } else if (selected_idx >= scroll_offset_ + viewport_height) {
            scroll_offset_ = selected_idx - viewport_height + 1;
        }

        int start_idx = scroll_offset_;
        int end_idx = std::min(static_cast<int>(playlists.size()), start_idx + viewport_height);

        Elements rows;
        Elements header_cols;
        header_cols.push_back(text("  #  ") | color(Color::GrayDark) | bold | size(WIDTH, EQUAL, 5));
        header_cols.push_back(text("PLAYLIST TITLE") | color(Color::GrayLight) | bold | xflex);
        if (!state_.is_compact()) {
            header_cols.push_back(text("TRACKS") | color(Color::GrayLight) | bold | size(WIDTH, EQUAL, 14));
            header_cols.push_back(text("TIME") | color(Color::GrayLight) | bold | size(WIDTH, EQUAL, 10));
            header_cols.push_back(text("LIKES") | color(Color::GrayLight) | bold | size(WIDTH, EQUAL, 12));
        }
        rows.push_back(hbox(std::move(header_cols)));
        rows.push_back(separatorLight());

        for (int i = start_idx; i < end_idx; ++i) {
            const auto& pl = playlists[static_cast<size_t>(i)];
            bool is_selected = (i == selected_idx);

            Elements cols;
            std::string num_str = " " + std::to_string(i + 1);
            while (num_str.length() < 4) num_str.push_back(' ');
            num_str.push_back(' ');
            cols.push_back(text(num_str) | color(is_selected ? accent : Color::GrayLight) | size(WIDTH, EQUAL, 5));

            Elements pl_title_elem;
            pl_title_elem.push_back(text(core::truncate_display_width(pl.title, 40)) | color(is_selected ? Color::White : Color::GrayLight) | bold);
            pl_title_elem.push_back(filler());
            cols.push_back(hbox(std::move(pl_title_elem)) | xflex);

            if (!state_.is_compact()) {
                std::string tracks_str = std::to_string(pl.track_count) + " tracks";
                int tw = ftxui::string_width(tracks_str);
                while (tw < 14) { tracks_str.push_back(' '); tw++; }
                cols.push_back(text(tracks_str) | color(accent) | size(WIDTH, EQUAL, 14));

                std::string dur_str = core::format_duration_ms(pl.duration_ms);
                int dw = ftxui::string_width(dur_str);
                while (dw < 10) { dur_str.push_back(' '); dw++; }
                cols.push_back(text(dur_str) | color(Color::GrayLight) | size(WIDTH, EQUAL, 10));

                const auto& sym = ThemeSymbols::get(state_.icon_style);
                std::string likes_str = sym.likes + " " + std::to_string(pl.likes_count);
                int lw = ftxui::string_width(likes_str);
                while (lw < 12) { likes_str.push_back(' '); lw++; }
                cols.push_back(text(likes_str) | color(Color::GrayDark) | size(WIDTH, EQUAL, 12));
            }

            auto row_elem = hbox(std::move(cols));
            if (is_selected) {
                row_elem = row_elem | bgcolor(Color::RGB(
                    state_.accent_color.r / 6,
                    state_.accent_color.g / 6,
                    state_.accent_color.b / 6
                ));
            }
            rows.push_back(row_elem);
        }

        std::string count_info = " " + std::to_string(selected_idx + 1) + "/" + std::to_string(playlists.size()) + " ";
        return vbox({
            hbox({
                text(" PLAYLISTS (Press Enter to open) ") | color(border_col) | bold,
                filler(),
                middle_elem,
                filler(),
                text(count_info) | color(Color::GrayDark)
            }),
            separator(),
            vbox(std::move(rows)) | yflex
        }) | borderRounded | color(border_col) | yflex;
    }

    const auto& tracks = get_current_track_list();
    int selected_idx = get_current_selected_index();

    std::string panel_title = " TRACKS ";
    if (state_.current_view == models::ViewMode::Playlists && state_.selected_playlist) {
        panel_title = " PLAYLIST: " + core::truncate_display_width(state_.selected_playlist->title, 36) + " [Esc or 'h' to back] ";
    } else if (state_.current_view == models::ViewMode::Likes) {
        panel_title = " LIKES ";
    } else if (state_.current_view == models::ViewMode::Stream) {
        panel_title = " STREAM ";
    } else if (state_.current_view == models::ViewMode::Search) {
        panel_title = " SEARCH RESULTS ";
    }

    Element middle_elem = filler();
    if (state_.current_view == models::ViewMode::Likes) {
        if (search_active_) {
            middle_elem = hbox({
                text(" 🔍 ") | color(accent),
                search_input_comp_->Render() | xflex,
                text(" [Esc to close] ") | color(Color::GrayDark) | dim
            }) | borderRounded | color(accent) | size(WIDTH, GREATER_THAN, 28) | size(WIDTH, LESS_THAN, 48);
        } else {
            middle_elem = text(" [alt+/ to search in likes] ") | color(Color::RGB(100, 100, 110)) | dim;
        }
    } else if (state_.current_view == models::ViewMode::Playlists && state_.selected_playlist) {
        if (search_active_) {
            middle_elem = hbox({
                text(" 🔍 ") | color(accent),
                search_input_comp_->Render() | xflex,
                text(" [Esc to close] ") | color(Color::GrayDark) | dim
            }) | borderRounded | color(accent) | size(WIDTH, GREATER_THAN, 28) | size(WIDTH, LESS_THAN, 48);
        } else {
            middle_elem = text(" [alt+/ to search in playlist] ") | color(Color::RGB(100, 100, 110)) | dim;
        }
    }

    if (tracks.empty()) {
        std::string empty_msg = "No tracks found.";
        if (search_active_ && !search_query_.empty()) {
            empty_msg = "No tracks matching '" + search_query_ + "'. Press Esc to clear.";
        } else if (state_.current_view == models::ViewMode::Playlists && state_.selected_playlist) {
            empty_msg = state_.is_loading
                ? "Loading tracks for playlist '" + state_.selected_playlist->title + "' (" + std::to_string(state_.selected_playlist->track_count) + " tracks)..."
                : "This playlist is empty.";
        } else if (state_.current_view == models::ViewMode::Search) {
            empty_msg = state_.search_query.empty()
                ? "Type a query in the search bar above to find tracks."
                : (state_.is_loading ? "Searching..." : "No results found for '" + state_.search_query + "'");
        } else if (state_.current_view == models::ViewMode::Likes) {
            empty_msg = state_.is_loading
                ? "Loading your likes..."
                : "No liked tracks yet. Press 'l' on any track to add it to Likes.";
        } else if (state_.current_view == models::ViewMode::Stream) {
            empty_msg = state_.is_loading ? "Loading stream..." : "No stream tracks found.";
        }

        auto empty_box = vbox({
            filler(),
            text(empty_msg) | color(Color::GrayLight) | center,
            filler()
        });

        return vbox({
            hbox({
                text(panel_title) | color(border_col) | bold,
                filler(),
                middle_elem,
                filler(),
                text(" 0/0 ") | color(Color::GrayDark)
            }),
            separator(),
            empty_box | yflex
        }) | borderRounded | color(border_col) | yflex;
    }

    // Virtual scrolling viewport calculation (terminal_height minus 16 lines of headers/player/borders)
    int viewport_height = std::max(3, state_.terminal_height - 16);

    if (selected_idx < scroll_offset_) {
        scroll_offset_ = selected_idx;
    } else if (selected_idx >= scroll_offset_ + viewport_height) {
        scroll_offset_ = selected_idx - viewport_height + 1;
    }

    int start_idx = scroll_offset_;
    int end_idx = std::min(static_cast<int>(tracks.size()), start_idx + viewport_height);

    Elements rows;

    bool show_like_col = (state_.current_view != models::ViewMode::Likes);

    // Header row
    Elements header_cols;
    header_cols.push_back(text("  #  ") | color(Color::GrayDark) | bold | size(WIDTH, EQUAL, 5));
    header_cols.push_back(text("TITLE") | color(Color::GrayLight) | bold | xflex);
    if (show_like_col) {
        header_cols.push_back(text("    ") | size(WIDTH, EQUAL, 4));
    }
    if (!state_.is_compact()) {
        header_cols.push_back(text("ARTIST") | color(Color::GrayLight) | bold | size(WIDTH, EQUAL, 24));
        header_cols.push_back(text("TIME") | color(Color::GrayLight) | bold | size(WIDTH, EQUAL, 8));
        if (state_.is_ultrawide()) {
            header_cols.push_back(text("GENRE") | color(Color::GrayLight) | bold | size(WIDTH, EQUAL, 16));
        }
    }
    rows.push_back(hbox(std::move(header_cols)));
    rows.push_back(separatorLight());

    int sidebar_w = state_.is_compact() ? 8 : 22;
    int fixed_cols = 5 + 24 + 8 + (state_.is_ultrawide() ? 52 : 0) + (show_like_col ? 4 : 0);
    int avail_row_w = std::max(20, state_.terminal_width - sidebar_w - 6);
    int max_title_w = state_.is_compact() ? std::max(12, avail_row_w - 7 - (show_like_col ? 4 : 0)) : std::max(12, avail_row_w - fixed_cols);

    for (int i = start_idx; i < end_idx; ++i) {
        const auto& track = tracks[static_cast<size_t>(i)];
        bool is_selected = (i == selected_idx);
        bool is_active_track = (state_.active_track && state_.active_track->id == track.id);

        Elements cols;

        // Play/Status indicator & Index
        std::string num_str = std::to_string(i + 1);
        Element indicator_elem;
        if (is_active_track) {
            const auto& sym = ThemeSymbols::get(state_.icon_style);
            std::string icon = (state_.playback_state == audio::PlaybackState::Playing) ? sym.play : sym.pause;
            indicator_elem = text(" " + icon + " ") | color(accent) | bold;
        } else {
            std::string padded = " " + num_str;
            while (padded.length() < 4) padded.push_back(' ');
            padded.push_back(' ');
            indicator_elem = text(padded) | color(Color::GrayDark);
        }
        cols.push_back(indicator_elem | size(WIDTH, EQUAL, 5));

        // Title with display-width aware truncation and Access badge
        Elements title_elements;
        std::string title_str = core::truncate_display_width(track.title, max_title_w);
        auto title_elem = text(title_str);
        if (is_active_track) {
            title_elem = title_elem | color(accent) | bold;
        } else if (is_selected) {
            title_elem = title_elem | color(Color::White) | bold;
        } else {
            title_elem = title_elem | color(Color::RGB(220, 220, 220));
        }
        title_elements.push_back(title_elem);

        if (track.is_drm_protected()) {
            Color drm_col = state_.drm_engine_enabled ? Color::GrayLight : Color::RedLight;
            title_elements.push_back(text(" [DRM] ") | color(drm_col) | (state_.drm_engine_enabled ? dim : bold));
        } else if (track.is_go_plus()) {
            title_elements.push_back(text(" [GO+] ") | color(Color::RGB(255, 140, 0)) | bold);
        }
        title_elements.push_back(filler());
        cols.push_back(hbox(std::move(title_elements)) | xflex);

        // Liked track badge (placed between title and artist, everywhere except Likes tab)
        if (show_like_col) {
            bool is_liked = state_.is_track_liked(track.id) || track.is_liked;
            cols.push_back(render_like_badge(is_liked, state_.icon_style));
        }

        // Artist & Duration in normal mode
        if (!state_.is_compact()) {
            std::string artist_str = core::truncate_display_width(track.artist, 22);
            int artist_w = ftxui::string_width(artist_str);
            while (artist_w < 24) { artist_str.push_back(' '); artist_w++; }
            cols.push_back(text(artist_str) | color(is_selected ? Color::White : Color::GrayLight) | size(WIDTH, EQUAL, 24));

            std::string dur_str = core::format_duration_ms(track.duration_ms);
            int dur_w = ftxui::string_width(dur_str);
            while (dur_w < 8) { dur_str.push_back(' '); dur_w++; }
            cols.push_back(text(dur_str) | color(Color::GrayLight) | size(WIDTH, EQUAL, 8));

            if (state_.is_ultrawide()) {
                std::string genre_str = core::truncate_display_width(track.genre.empty() ? "-" : track.genre, 14);
                int genre_w = ftxui::string_width(genre_str);
                while (genre_w < 16) { genre_str.push_back(' '); genre_w++; }
                cols.push_back(text(genre_str) | color(Color::GrayDark) | size(WIDTH, EQUAL, 16));
            }
        }

        auto row = hbox(std::move(cols));
        if (is_selected) {
            row = row | bgcolor(Color::RGB(
                state_.accent_color.r / 6,
                state_.accent_color.g / 6,
                state_.accent_color.b / 6
            ));
            if (is_focused) {
                row = row | bold;
            }
        }

        rows.push_back(row);
    }

    auto list_box = vbox(std::move(rows));

    // Scrollbar indicator
    std::string count_info = " " + std::to_string(selected_idx + 1) + "/" + std::to_string(tracks.size()) + " ";

    return vbox({
        hbox({
            text(panel_title) | color(border_col) | bold,
            filler(),
            middle_elem,
            filler(),
            text(count_info) | color(Color::GrayDark)
        }),
        separator(),
        list_box | yflex
    }) | borderRounded | color(border_col) | yflex;
}

} // namespace sctui::ui::views
