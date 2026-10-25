#include "sctui/ui/views/queue_view.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include <ftxui/component/event.hpp>
#include <algorithm>

namespace sctui::ui::views {

using namespace ftxui;

QueueView::QueueView(models::AppState& state, PlayFromQueueCallback on_play_from_queue)
    : state_(state), on_play_from_queue_(std::move(on_play_from_queue)) {
    component_ = CatchEvent(Make<ComponentBase>(), [this](Event event) {
        if (state_.focused_panel != models::FocusPanel::QueueList) {
            return false;
        }

        if (event == Event::ArrowDown) {
            if (!state_.queue.tracks.empty()) {
                selected_index_ = std::min(static_cast<int>(state_.queue.tracks.size() - 1), selected_index_ + 1);
            }
            return true;
        }
        if (event == Event::ArrowUp) {
            selected_index_ = std::max(0, selected_index_ - 1);
            return true;
        }
        if (event == Event::Character('j') || event == Event::Character('J') || event == Event::CtrlJ || event == Event::AltJ) {
            move_selected_down();
            return true;
        }
        if (event == Event::Character('k') || event == Event::Character('K') || event == Event::CtrlK || event == Event::AltK) {
            move_selected_up();
            return true;
        }
        if (event == Event::Character('A') || event == Event::CtrlN) {
            move_selected_to_next();
            return true;
        }
        if (event == Event::Character('d') || event == Event::Character('x')) {
            remove_selected();
            return true;
        }
        if (event == Event::Character('C')) {
            clear_queue();
            return true;
        }
        if (event == Event::Return) {
            if (on_play_from_queue_ && selected_index_ >= 0 && static_cast<size_t>(selected_index_) < state_.queue.tracks.size()) {
                bool is_shuffled = state_.queue.shuffle && state_.queue.shuffle_indices.size() == state_.queue.tracks.size();
                if (is_shuffled) {
                    size_t real_index = state_.queue.shuffle_indices[static_cast<size_t>(selected_index_)];
                    state_.queue.shuffle_cursor = static_cast<size_t>(selected_index_);
                    on_play_from_queue_(real_index);
                } else {
                    on_play_from_queue_(static_cast<size_t>(selected_index_));
                }
            }
            return true;
        }
        if (event == Event::Character('h') || event == Event::ArrowLeft || event == Event::Escape) {
            state_.focused_panel = models::FocusPanel::Sidebar;
            return true;
        }

        return false;
    });
}

Component QueueView::get_component() {
    return component_;
}

void QueueView::move_selected_up() {
    if (selected_index_ > 0 && selected_index_ < static_cast<int>(state_.queue.tracks.size())) {
        bool is_shuffled = state_.queue.shuffle && state_.queue.shuffle_indices.size() == state_.queue.tracks.size();
        if (is_shuffled) {
            std::swap(state_.queue.shuffle_indices[static_cast<size_t>(selected_index_)],
                      state_.queue.shuffle_indices[static_cast<size_t>(selected_index_ - 1)]);
            if (state_.queue.shuffle_cursor == static_cast<size_t>(selected_index_)) {
                state_.queue.shuffle_cursor--;
            } else if (state_.queue.shuffle_cursor == static_cast<size_t>(selected_index_ - 1)) {
                state_.queue.shuffle_cursor++;
            }
        } else {
            std::swap(state_.queue.tracks[static_cast<size_t>(selected_index_)],
                      state_.queue.tracks[static_cast<size_t>(selected_index_ - 1)]);
            if (state_.queue.current_index == selected_index_) {
                state_.queue.current_index--;
            } else if (state_.queue.current_index == selected_index_ - 1) {
                state_.queue.current_index++;
            }
        }
        selected_index_--;
    }
}

void QueueView::move_selected_down() {
    if (selected_index_ >= 0 && selected_index_ + 1 < static_cast<int>(state_.queue.tracks.size())) {
        bool is_shuffled = state_.queue.shuffle && state_.queue.shuffle_indices.size() == state_.queue.tracks.size();
        if (is_shuffled) {
            std::swap(state_.queue.shuffle_indices[static_cast<size_t>(selected_index_)],
                      state_.queue.shuffle_indices[static_cast<size_t>(selected_index_ + 1)]);
            if (state_.queue.shuffle_cursor == static_cast<size_t>(selected_index_)) {
                state_.queue.shuffle_cursor++;
            } else if (state_.queue.shuffle_cursor == static_cast<size_t>(selected_index_ + 1)) {
                state_.queue.shuffle_cursor--;
            }
        } else {
            std::swap(state_.queue.tracks[static_cast<size_t>(selected_index_)],
                      state_.queue.tracks[static_cast<size_t>(selected_index_ + 1)]);
            if (state_.queue.current_index == selected_index_) {
                state_.queue.current_index++;
            } else if (state_.queue.current_index == selected_index_ + 1) {
                state_.queue.current_index--;
            }
        }
        selected_index_++;
    }
}

void QueueView::move_selected_to_next() {
    if (state_.queue.tracks.empty() || selected_index_ < 0 ||
        static_cast<size_t>(selected_index_) >= state_.queue.tracks.size()) {
        return;
    }

    bool is_shuffled = state_.queue.shuffle && state_.queue.shuffle_indices.size() == state_.queue.tracks.size();
    int cur_idx = is_shuffled ? static_cast<int>(state_.queue.shuffle_cursor) : state_.queue.current_index;
    int target = (cur_idx < 0) ? 0 : (cur_idx + 1);
    if (target >= static_cast<int>(state_.queue.tracks.size())) {
        target = static_cast<int>(state_.queue.tracks.size() - 1);
    }

    if (selected_index_ == cur_idx) {
        state_.notify("Track is currently playing");
        return;
    }
    if (selected_index_ == target) {
        state_.notify("Track is already next in queue");
        return;
    }

    size_t track_idx = is_shuffled ? state_.queue.shuffle_indices[static_cast<size_t>(selected_index_)] : static_cast<size_t>(selected_index_);
    std::string title = state_.queue.tracks[track_idx].title;

    if (state_.queue.move_to_next(selected_index_)) {
        selected_index_ = target;
        state_.notify("Moved to next in queue: " + core::truncate_string(title, 25));
    }
}

void QueueView::remove_selected() {
    if (selected_index_ >= 0 && static_cast<size_t>(selected_index_) < state_.queue.tracks.size()) {
        bool is_shuffled = state_.queue.shuffle && state_.queue.shuffle_indices.size() == state_.queue.tracks.size();
        if (is_shuffled) {
            size_t real_index = state_.queue.shuffle_indices[static_cast<size_t>(selected_index_)];
            state_.queue.shuffle_indices.erase(state_.queue.shuffle_indices.begin() + selected_index_);
            state_.queue.tracks.erase(state_.queue.tracks.begin() + real_index);
            for (auto& idx : state_.queue.shuffle_indices) {
                if (idx > real_index) idx--;
            }
            if (state_.queue.shuffle_cursor >= state_.queue.shuffle_indices.size() && !state_.queue.shuffle_indices.empty()) {
                state_.queue.shuffle_cursor = state_.queue.shuffle_indices.size() - 1;
            }
        } else {
            state_.queue.tracks.erase(state_.queue.tracks.begin() + selected_index_);
            if (state_.queue.current_index > selected_index_) {
                state_.queue.current_index--;
            } else if (state_.queue.current_index == selected_index_) {
                if (state_.queue.current_index >= static_cast<int>(state_.queue.tracks.size())) {
                    state_.queue.current_index = static_cast<int>(state_.queue.tracks.size() - 1);
                }
            }
        }
        if (selected_index_ >= static_cast<int>(state_.queue.tracks.size())) {
            selected_index_ = static_cast<int>(state_.queue.tracks.size() - 1);
        }
    }
}

void QueueView::clear_queue() {
    state_.queue.tracks.clear();
    state_.queue.shuffle_indices.clear();
    state_.queue.current_index = -1;
    state_.queue.shuffle_cursor = 0;
    selected_index_ = 0;
}

int QueueView::get_selected_index() const {
    return selected_index_;
}

void QueueView::scroll_down() {
    if (!state_.queue.tracks.empty()) {
        selected_index_ = std::min(static_cast<int>(state_.queue.tracks.size() - 1), selected_index_ + 1);
    }
}

void QueueView::scroll_up() {
    selected_index_ = std::max(0, selected_index_ - 1);
}

void QueueView::select_row(int visible_row) {
    int target = scroll_offset_ + visible_row;
    if (target >= 0 && target < static_cast<int>(state_.queue.tracks.size())) {
        selected_index_ = target;
        if (on_play_from_queue_) {
            bool is_shuffled = state_.queue.shuffle && state_.queue.shuffle_indices.size() == state_.queue.tracks.size();
            if (is_shuffled) {
                size_t real_index = state_.queue.shuffle_indices[static_cast<size_t>(target)];
                state_.queue.shuffle_cursor = static_cast<size_t>(target);
                on_play_from_queue_(real_index);
            } else {
                on_play_from_queue_(static_cast<size_t>(target));
            }
        }
    }
}

Element QueueView::render(bool is_focused) {
    auto accent = state_.get_accent_color();
    auto border_col = is_focused ? accent : Color::GrayDark;

    if (state_.queue.tracks.empty()) {
        return vbox({
            text(" PLAY QUEUE ") | color(border_col) | bold,
            separator(),
            vbox({
                filler(),
                text("Queue is empty.") | color(Color::GrayLight) | center,
                text("Press 'a' on any track to add to queue.") | color(Color::GrayDark) | center,
                filler()
            }) | yflex
        }) | borderRounded | color(border_col) | yflex;
    }

    // Dynamic shuffle check
    bool is_shuffled = state_.queue.shuffle;
    if (is_shuffled && state_.queue.shuffle_indices.size() != state_.queue.tracks.size()) {
        state_.queue.rebuild_shuffle();
    }

    // Automatically follow playing track in selection and viewport scrolling
    uint64_t active_track_id = state_.active_track ? state_.active_track->id : 0;
    if (active_track_id != 0 && active_track_id != last_active_track_id_) {
        last_active_track_id_ = active_track_id;
        int cur_idx = is_shuffled
            ? static_cast<int>(state_.queue.shuffle_cursor)
            : state_.queue.current_index;
        if (cur_idx >= 0 && cur_idx < static_cast<int>(state_.queue.tracks.size())) {
            selected_index_ = cur_idx;
        }
    }

    uint64_t total_queue_ms = 0;
    for (const auto& t : state_.queue.tracks) {
        total_queue_ms += t.duration_ms;
    }

    std::string count_str = std::to_string(state_.queue.tracks.size()) + " tracks • " + core::format_duration_ms(total_queue_ms);

    int viewport_height = std::max(3, state_.terminal_height - 18);
    int selected_idx = std::clamp(selected_index_, 0, static_cast<int>(state_.queue.tracks.size() - 1));
    if (selected_idx < scroll_offset_) {
        scroll_offset_ = selected_idx;
    } else if (selected_idx >= scroll_offset_ + viewport_height) {
        scroll_offset_ = selected_idx - viewport_height + 1;
    }

    int start_idx = scroll_offset_;
    int end_idx = std::min(static_cast<int>(state_.queue.tracks.size()), start_idx + viewport_height);

    // Calculate maximum digits relative to the last track in the queue for exact title alignment
    int total_queue_size = static_cast<int>(state_.queue.tracks.size());
    int max_digits = std::max(2, static_cast<int>(std::to_string(total_queue_size).length()));

    Elements rows;
    for (int i = start_idx; i < end_idx; ++i) {
        size_t track_idx = is_shuffled ? state_.queue.shuffle_indices[static_cast<size_t>(i)] : static_cast<size_t>(i);
        const auto& track = state_.queue.tracks[track_idx];

        bool is_selected = (i == selected_index_);
        bool is_current = is_shuffled
            ? (static_cast<size_t>(i) == state_.queue.shuffle_cursor)
            : (static_cast<int>(track_idx) == state_.queue.current_index);

        Elements cols;
        if (is_current) {
            cols.push_back(text(" ▶ ") | color(accent) | bold);
        } else {
            cols.push_back(text("   "));
        }

        // Right-align track sequence number based on max_digits of the last track
        std::string num_str = std::to_string(i + 1);
        std::string padded_num = std::string(static_cast<size_t>(std::max(0, max_digits - static_cast<int>(num_str.length()))), ' ') + num_str + ". ";
        cols.push_back(text(padded_num) | color(Color::GrayDark));

        std::string title = core::truncate_string(track.title, 26);
        Elements title_elements;
        title_elements.push_back(text(title) | color(is_current ? accent : (is_selected ? Color::White : Color::GrayLight)) | bold | xflex);

        // Do not show [DRM] badge in queue tab per user request
        if (track.is_go_plus()) {
            title_elements.push_back(text(" [GO+]") | color(Color::RGB(255, 140, 0)) | bold);
        }
        cols.push_back(hbox(std::move(title_elements)) | xflex);

        std::string dur = core::format_duration_ms(track.duration_ms);
        cols.push_back(text(" " + dur + " ") | color(Color::GrayDark));

        auto row = hbox(std::move(cols));
        if (is_selected) {
            row = row | bgcolor(Color::RGB(
                state_.accent_color.r / 6,
                state_.accent_color.g / 6,
                state_.accent_color.b / 6
            ));
        }

        rows.push_back(row);
    }

    const auto& sym = ThemeSymbols::get(state_.icon_style);
    std::string title_badge = is_shuffled
        ? " PLAY QUEUE " + sym.shuffle + " [SHUFFLED] "
        : " PLAY QUEUE " + (state_.icon_style == core::IconStyle::DefaultAscii ? "[SEQ]" : sym.repeat_all) + " [SEQUENTIAL] ";

    return vbox({
        hbox({
            text(title_badge) | color(is_shuffled ? accent : border_col) | bold,
            filler(),
            text(" " + count_str + " ") | color(Color::GrayDark)
        }),
        separator(),
        vbox(std::move(rows)) | yflex,
        separatorLight(),
        text(" [Enter] Play  [J/K] Reorder  [d] Delete  [C] Clear ") | color(Color::GrayDark) | center
    }) | borderRounded | color(border_col) | yflex;
}

} // namespace sctui::ui::views
