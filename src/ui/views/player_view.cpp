#include "sctui/ui/views/player_view.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>
#include <cmath>

namespace sctui::ui::views {

using namespace ftxui;

PlayerView::PlayerView(models::AppState& state) : state_(state) {
    component_ = CatchEvent(Make<ComponentBase>(), [this](Event event) {
        if (state_.focused_panel != models::FocusPanel::Player) {
            return false;
        }

        if (event == Event::Character('h') || event == Event::ArrowLeft) {
            // Seek backward 5s
            return true;
        }
        if (event == Event::Character('l') || event == Event::ArrowRight) {
            // Seek forward 5s
            return true;
        }
        if (event == Event::Character('H')) {
            // Seek backward 30s
            return true;
        }
        if (event == Event::Character('L')) {
            // Seek forward 30s
            return true;
        }
        return false;
    });
}

Component PlayerView::get_component() {
    return component_;
}

Element PlayerView::render_progress_bar() {
    uint64_t pos = state_.position_ms;
    uint64_t dur = state_.duration_ms;

    float ratio = 0.0f;
    if (dur > 0) {
        ratio = std::clamp(static_cast<float>(pos) / static_cast<float>(dur), 0.0f, 1.0f);
    }

    std::string pos_str = core::format_duration_ms(pos);
    std::string dur_str = core::format_duration_ms(dur);

    // Interactive custom styled progress bar
    auto pbar = gauge(ratio) | color(state_.get_accent_color()) | bgcolor(Color::RGB(40, 40, 40));

    return hbox({
        text(pos_str) | color(Color::White) | bold,
        text(" "),
        pbar | xflex,
        text(" "),
        text(dur_str) | color(Color::GrayLight)
    });
}

Element PlayerView::render_waveform_or_meter() {
    // 8-level Unicode vertical block characters
    static const char* block_levels[] = {" ", " ", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

    Elements bars;
    const auto& bands = state_.visualizer_bars;

    for (size_t i = 0; i < audio::VisualizerBandsCount; ++i) {
        float val = std::clamp(bands[i], 0.0f, 1.0f);
        int level = static_cast<int>(std::round(val * 8.0f));
        level = std::clamp(level, 0, 8);

        Color bar_color;
        if (level >= 7) {
            bar_color = Color::RGB(255, 60, 0); // Peak Red-Orange
        } else if (level >= 4) {
            bar_color = state_.get_accent_color(); // Dynamic Accent Color
        } else {
            bar_color = Color::RGB(
                state_.accent_color.r * 2 / 3,
                state_.accent_color.g * 2 / 3,
                state_.accent_color.b * 2 / 3
            );
        }

        bars.push_back(text(block_levels[level]) | color(bar_color) | bold);
    }

    return hbox(std::move(bars));
}

Element PlayerView::render_controls() {
    Elements ctrls;
    const auto& sym = ThemeSymbols::get(state_.icon_style);

    // Playback state button
    std::string state_icon;
    Color state_col = Color::White;
    auto accent = state_.get_accent_color();

    switch (state_.playback_state) {
        case audio::PlaybackState::Playing:
            state_icon = sym.play + " PLAYING";
            state_col = accent;
            break;
        case audio::PlaybackState::Paused:
            state_icon = sym.pause + " PAUSED";
            state_col = Color::Yellow;
            break;
        case audio::PlaybackState::Buffering:
            state_icon = sym.buffering + " BUFFERING";
            state_col = accent;
            break;
        case audio::PlaybackState::Loading:
            state_icon = sym.loading + " LOADING";
            state_col = accent;
            break;
        case audio::PlaybackState::Finished:
            state_icon = sym.finished + " FINISHED";
            state_col = Color::GreenLight;
            break;
        case audio::PlaybackState::Error:
            state_icon = sym.error + " ERROR";
            state_col = Color::RedLight;
            break;
        case audio::PlaybackState::Stopped:
            state_icon = sym.stopped + " STOPPED";
            state_col = Color::GrayLight;
            break;
    }

    ctrls.push_back(text(" " + state_icon + " ") | color(state_col) | bold);
    ctrls.push_back(separator());

    // Shuffle Badge
    if (state_.queue.shuffle) {
        ctrls.push_back(text(" " + sym.shuffle + " SHUF ") | color(accent) | bold);
    } else {
        ctrls.push_back(text(" " + sym.shuffle + " SHUF ") | color(Color::GrayDark));
    }

    // Repeat Mode Badge
    switch (state_.queue.repeat_mode) {
        case core::RepeatMode::Off:
            ctrls.push_back(text(" " + sym.repeat_off + " ") | color(Color::GrayDark));
            break;
        case core::RepeatMode::One:
            ctrls.push_back(text(" " + sym.repeat_one + " ") | color(accent) | bold);
            break;
        case core::RepeatMode::All:
            ctrls.push_back(text(" " + sym.repeat_all + " ") | color(accent) | bold);
            break;
    }

    // Fullscreen Badge
    ctrls.push_back(separator());
    ctrls.push_back(text(state_.is_fullscreen ? " [⛶ FULL] " : " [⛶ Full] ") | color(state_.is_fullscreen ? accent : Color::GrayLight));

    // Buffer percentage if streaming
    if (state_.buffer_percent < 99.0f && state_.buffer_percent > 0.0f) {
        ctrls.push_back(separator());
        int buf = static_cast<int>(std::round(state_.buffer_percent));
        ctrls.push_back(text(" Buf: " + std::to_string(buf) + "% ") | color(Color::GrayLight));
    }

    return hbox(std::move(ctrls));
}

Element PlayerView::render_volume() {
    const auto& sym = ThemeSymbols::get(state_.icon_style);
    if (state_.is_muted) {
        return hbox({
            text(" " + sym.muted + " ") | color(Color::RedLight) | bold
        });
    }

    int vol = static_cast<int>(std::round(state_.volume_percent));
    float vol_ratio = std::clamp(state_.volume_percent / 150.0f, 0.0f, 1.0f);
    Color vol_col = (vol > 100) ? Color::RGB(255, 140, 0) : state_.get_accent_color();

    auto vol_gauge = gauge(vol_ratio) | color(vol_col) | size(WIDTH, EQUAL, 10);

    return hbox({
        text(" " + sym.volume + " ") | color(Color::GrayLight),
        vol_gauge,
        text(" " + std::to_string(vol) + "% ") | color(vol > 100 ? vol_col : Color::White) | bold
    });
}

Element PlayerView::render(bool is_focused) {
    auto accent = state_.get_accent_color();
    auto border_col = is_focused ? accent : Color::GrayDark;

    std::string track_title = "No track playing";
    std::string artist_name = "SoundCloud";

    int avail_info = std::max(20, state_.terminal_width - 48);
    int max_artist = std::clamp(avail_info / 3, 10, 24);
    int max_title = std::max(12, avail_info - max_artist - 5);

    if (state_.active_track) {
        track_title = core::truncate_display_width(state_.active_track->title, max_title);
        if (!state_.active_track->artist.empty()) {
            artist_name = core::truncate_display_width(state_.active_track->artist, max_artist);
        }
    }

    auto title_elem = text(track_title) | color(Color::White) | bold;
    auto artist_elem = text(" • " + artist_name) | color(accent);
    const auto& sym = ThemeSymbols::get(state_.icon_style);

    auto track_info = hbox({
        text(" " + sym.note + "  ") | color(accent) | bold,
        title_elem,
        artist_elem,
        filler()
    }) | xflex;

    if (state_.is_compact()) {
        return vbox({
            track_info,
            render_progress_bar(),
            hbox({
                render_controls(),
                filler(),
                render_volume()
            })
        }) | borderRounded | color(border_col);
    }

    std::string controls_hint = (state_.terminal_width < 110)
        ? " [f] Full  [n] Next  [p] Prev  [Space] Play  [s] Shuf "
        : " [f] Fullscreen  [n] Next  [p] Prev  [Space] Play/Pause  [s] Shuf  [r] Rep  [h/l] ±5s  [+/-] Vol  [m] Mute ";

    return vbox({
        hbox({
            track_info,
            render_waveform_or_meter(),
            text("   "),
            render_volume()
        }),
        separatorLight(),
        render_progress_bar(),
        hbox({
            render_controls(),
            filler(),
            text(controls_hint) | color(Color::GrayDark)
        })
    }) | borderRounded | color(border_col);
}

} // namespace sctui::ui::views
