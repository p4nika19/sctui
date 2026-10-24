#include "sctui/ui/views/header_view.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace sctui::ui::views {

using namespace ftxui;

HeaderView::HeaderView(models::AppState& state, std::function<void(const std::string&)> on_search_submit)
    : state_(state), on_search_submit_(std::move(on_search_submit)) {
    InputOption opt;
    opt.on_enter = [this]() {
        if (!state_.search_query.empty() && on_search_submit_) {
            on_search_submit_(state_.search_query);
        } else {
            state_.focused_panel = models::FocusPanel::TrackList;
        }
    };
    input_box_ = Input(&state_.search_query, " Search tracks, artists, sets... (Press '/' to focus)", opt);
}

Component HeaderView::get_component() {
    return input_box_;
}

void HeaderView::focus_input() {
    input_box_->TakeFocus();
}

Element HeaderView::render() {
    bool is_input_focused = (state_.focused_panel == models::FocusPanel::SearchInput);
    auto accent = state_.get_accent_color();
    const auto& sym = ThemeSymbols::get(state_.icon_style);

    auto logo = hbox({
        text(" " + sym.logo + " ") | color(accent) | bold,
        text("SOUNDCLOUD") | color(accent) | bold,
        text(" ") | color(accent),
        text("TUI") | color(Color::White) | bold,
        text(" ")
    });

    auto input_rendered = input_box_->Render() | xflex;
    auto search_bar = hbox({
        text(" " + sym.search + " ") | (is_input_focused ? color(accent) : color(Color::GrayDark)),
        input_rendered
    });

    Element search_container;
    if (is_input_focused) {
        search_container = search_bar | borderRounded | color(accent);
    } else {
        search_container = search_bar | borderRounded | color(Color::GrayDark);
    }

    // Status or Notification badge
    Element status_badge;
    if (state_.notification && std::chrono::steady_clock::now() < state_.notification->expires_at) {
        int max_notif_w = std::max(16, state_.terminal_width / 3);
        std::string notif_msg = core::truncate_display_width(state_.notification->message, max_notif_w);
        if (state_.notification->is_error) {
            status_badge = hbox({
                text(" " + sym.error + " ") | bold,
                text(notif_msg) | bold
            }) | color(Color::RedLight) | borderRounded;
        } else {
            status_badge = hbox({
                text(" " + sym.finished + " ") | bold,
                text(notif_msg)
            }) | color(Color::GreenLight) | borderRounded;
        }
    } else if (state_.is_loading) {
        status_badge = hbox({
            text(" " + sym.buffering + " Loading... ") | color(accent) | bold
        }) | borderRounded;
    } else {
        status_badge = hbox({
            text(" [?] Help  ") | color(Color::GrayLight),
            text("[q] Exit ") | color(Color::GrayLight)
        }) | borderRounded | color(Color::GrayDark);
    }

    if (state_.is_compact()) {
        return vbox({
            hbox({ logo, filler(), status_badge }),
            search_container
        });
    }

    return hbox({
        logo | center,
        separator(),
        search_container | xflex,
        separator(),
        status_badge | center
    });
}

} // namespace sctui::ui::views
