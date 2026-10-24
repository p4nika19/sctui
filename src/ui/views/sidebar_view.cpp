#include "sctui/ui/views/sidebar_view.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include "sctui/core/cover_art.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>
#include <string>

namespace sctui::ui::views {

using namespace ftxui;

SidebarView::SidebarView(
    models::AppState& state,
    const core::Config& config,
    std::function<void(models::ViewMode)> on_view_selected
) : state_(state), config_(config), on_view_selected_(std::move(on_view_selected)) {
    selected_row_ = static_cast<int>(state_.current_view);

    component_ = CatchEvent(Make<ComponentBase>(), [this](Event event) {
        if (state_.focused_panel != models::FocusPanel::Sidebar) {
            return false;
        }

        if (event == Event::Character('j') || event == Event::ArrowDown) {
            selected_row_ = (selected_row_ + 1) % 7;
            return true;
        }
        if (event == Event::Character('k') || event == Event::ArrowUp) {
            selected_row_ = (selected_row_ + 6) % 7;
            return true;
        }
        if (event == Event::Character('l') || event == Event::ArrowRight || event == Event::Return) {
            auto target = static_cast<models::ViewMode>(selected_row_);
            state_.current_view = target;
            if (on_view_selected_) {
                on_view_selected_(target);
            }
            if (target == models::ViewMode::Settings) {
                state_.focused_panel = models::FocusPanel::SettingsPanel;
            } else if (target == models::ViewMode::Account) {
                state_.focused_panel = models::FocusPanel::AccountPanel;
            } else if (target == models::ViewMode::Queue) {
                state_.focused_panel = models::FocusPanel::QueueList;
            } else {
                state_.focused_panel = models::FocusPanel::TrackList;
            }
            return true;
        }

        return false;
    });
}

void SidebarView::set_selected_row(int row) {
    selected_row_ = std::clamp(row, 0, 6);
}

Component SidebarView::get_component() {
    return component_;
}

Element SidebarView::render(bool is_focused) {
    auto accent = state_.get_accent_color();
    const auto& sym = ThemeSymbols::get(state_.icon_style);
    std::vector<Element> items;
    struct EntryInfo {
        models::ViewMode mode;
        std::string icon;
        std::string label;
        std::string count_str;
    };

    std::string likes_count = state_.likes_tracks.empty() ? "" : std::to_string(state_.likes_tracks.size());
    std::string queue_count = state_.queue.tracks.empty() ? "" : std::to_string(state_.queue.tracks.size());
    std::string account_badge = state_.user_tracks.empty() ? "" : std::to_string(state_.user_tracks.size());

    const EntryInfo entries[] = {
        { models::ViewMode::Search, sym.search, "Search", "" },
        { models::ViewMode::Stream, sym.stream, "Stream", "" },
        { models::ViewMode::Likes, sym.likes, "Likes", likes_count },
        { models::ViewMode::Playlists, sym.playlists, "Playlists", "" },
        { models::ViewMode::Queue, sym.queue, "Queue", queue_count },
        { models::ViewMode::Account, sym.account, "Account", account_badge },
        { models::ViewMode::Settings, sym.settings, "Settings", "" }
    };

    for (const auto& entry : entries) {
        bool is_active = (state_.current_view == entry.mode);
        bool is_selected = (selected_row_ == static_cast<int>(entry.mode) && is_focused);
        Element item_element;

        if (state_.is_compact()) {
            // In compact mode, show only icons
            item_element = hbox({
                text(entry.icon) | center
            });
        } else {
            // Full sidebar
            Elements row;
            if (is_selected) {
                row.push_back(text("► ") | color(accent) | bold);
                row.push_back(text(entry.icon) | bold);
                row.push_back(text(" ") | bold);
                row.push_back(text(entry.label) | color(Color::White) | bold);
            } else if (is_active) {
                row.push_back(text(sym.play + " ") | color(accent) | bold);
                row.push_back(text(entry.icon) | bold);
                row.push_back(text(" ") | bold);
                row.push_back(text(entry.label) | color(Color::White) | bold);
            } else {
                row.push_back(text("  "));
                row.push_back(text(entry.icon) | color(Color::GrayLight));
                row.push_back(text(" "));
                row.push_back(text(entry.label) | color(Color::GrayLight));
            }

            if (!entry.count_str.empty()) {
                row.push_back(filler());
                row.push_back(text("(") | color(Color::GrayDark));
                row.push_back(text(entry.count_str) | color(accent));
                row.push_back(text(")") | color(Color::GrayDark));
            }

            item_element = hbox(std::move(row));
        }

        if (is_selected || is_active) {
            item_element = item_element | bgcolor(Color::RGB(
                state_.accent_color.r / (is_selected ? 6 : 9),
                state_.accent_color.g / (is_selected ? 6 : 9),
                state_.accent_color.b / (is_selected ? 6 : 9)
            ));
        }

        items.push_back(item_element);
    }

    auto sidebar_box = vbox(std::move(items));
    auto border_color = is_focused ? accent : Color::GrayDark;

    Elements sidebar_elements;
    sidebar_elements.push_back(text(state_.is_compact() ? "NAV" : " NAVIGATION ") | color(border_color) | bold | center);
    sidebar_elements.push_back(separator());
    sidebar_elements.push_back(sidebar_box);

    if (!state_.is_compact() && state_.terminal_height >= 29) {
        sidebar_elements.push_back(filler());
        sidebar_elements.push_back(separatorLight());

        Element cover_elem;
        if (state_.current_cover_art.has_value()) {
            cover_elem = core::CoverArtProcessor::render_cover_element(*state_.current_cover_art, config_.image_protocol);
        } else {
            cover_elem = core::CoverArtProcessor::render_placeholder(18, 9, state_.accent_color);
        }

        sidebar_elements.push_back(text(" NOW PLAYING ") | color(accent) | bold | center);
        sidebar_elements.push_back(cover_elem | center);

        if (state_.terminal_height >= 32) {
            std::string title_sub = state_.active_track
                ? core::truncate_display_width(state_.active_track->title, 18)
                : "No track playing";
            std::string artist_sub = (state_.active_track && !state_.active_track->artist.empty())
                ? core::truncate_display_width(state_.active_track->artist, 18)
                : "SoundCloud";

            sidebar_elements.push_back(text(title_sub) | color(Color::White) | bold | center);
            sidebar_elements.push_back(text(artist_sub) | color(Color::GrayLight) | center);
        }
    } else {
        sidebar_elements.push_back(filler());
    }

    return vbox(std::move(sidebar_elements))
        | borderRounded
        | color(border_color)
        | size(WIDTH, EQUAL, state_.is_compact() ? 8 : 22);
}

} // namespace sctui::ui::views
