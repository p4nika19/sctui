#include "sctui/ui/views/account_view.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include "sctui/core/cover_art.hpp"
#include "sctui/core/cookie_extractor.hpp"
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>

namespace sctui::ui::views {

using namespace ftxui;

AccountView::AccountView(
    models::AppState& state,
    const core::Config& config,
    PlayCallback on_play,
    QueueCallback on_queue,
    LikeCallback on_like,
    ExtractCookiesCallback on_extract_cookies
) : state_(state),
    config_(config),
    on_play_(std::move(on_play)),
    on_queue_(std::move(on_queue)),
    on_like_(std::move(on_like)),
    on_extract_cookies_(std::move(on_extract_cookies)) {

    component_ = CatchEvent(Make<ComponentBase>(), [this](Event event) {
        if (state_.focused_panel != models::FocusPanel::AccountPanel) {
            return false;
        }

        // Return focus to sidebar on Escape or Left
        if (event == Event::Escape || event == Event::ArrowLeft || event == Event::Character('h')) {
            state_.focused_panel = models::FocusPanel::Sidebar;
            return true;
        }

        if (event == Event::Character('j') || event == Event::ArrowDown) {
            scroll_down();
            return true;
        }
        if (event == Event::Character('k') || event == Event::ArrowUp) {
            scroll_up();
            return true;
        }
        if (event == Event::Character('g')) {
            state_.user_tracks_selected_index = 0;
            scroll_offset_ = 0;
            return true;
        }
        if (event == Event::Character('G')) {
            if (!state_.user_tracks.empty()) {
                state_.user_tracks_selected_index = static_cast<int>(state_.user_tracks.size() - 1);
            }
            return true;
        }

        // Play selected track
        if (event == Event::Return) {
            if (config_.auth_token.empty() && on_extract_cookies_) {
                on_extract_cookies_();
                return true;
            }
            if (on_play_ && state_.user_tracks_selected_index >= 0 &&
                static_cast<size_t>(state_.user_tracks_selected_index) < state_.user_tracks.size()) {
                on_play_(state_.user_tracks[static_cast<size_t>(state_.user_tracks_selected_index)],
                         static_cast<size_t>(state_.user_tracks_selected_index));
            }
            return true;
        }

        // Queue selected track
        if (event == Event::Character('a')) {
            queue_selected(false);
            return true;
        }

        // Queue next
        if (event == Event::Character('A') || event == Event::CtrlN) {
            queue_selected(true);
            return true;
        }

        // Like / unlike
        if (event == Event::Character('l')) {
            if (on_like_ && state_.user_tracks_selected_index >= 0 &&
                static_cast<size_t>(state_.user_tracks_selected_index) < state_.user_tracks.size()) {
                on_like_(state_.user_tracks[static_cast<size_t>(state_.user_tracks_selected_index)]);
            }
            return true;
        }

        return false;
    });
}

Component AccountView::get_component() {
    return component_;
}

void AccountView::scroll_down() {
    if (state_.user_tracks.empty()) return;
    state_.user_tracks_selected_index = std::min(
        static_cast<int>(state_.user_tracks.size() - 1),
        state_.user_tracks_selected_index + 1
    );
}

void AccountView::scroll_up() {
    state_.user_tracks_selected_index = std::max(0, state_.user_tracks_selected_index - 1);
}

void AccountView::select_row(int visible_row) {
    if (state_.user_tracks.empty()) return;
    int target = scroll_offset_ + visible_row;
    if (target >= 0 && target < static_cast<int>(state_.user_tracks.size())) {
        state_.user_tracks_selected_index = target;
        if (on_play_) {
            on_play_(state_.user_tracks[static_cast<size_t>(target)], static_cast<size_t>(target));
        }
    }
}

void AccountView::queue_selected(bool play_next) {
    if (on_queue_ && state_.user_tracks_selected_index >= 0 &&
        static_cast<size_t>(state_.user_tracks_selected_index) < state_.user_tracks.size()) {
        on_queue_(state_.user_tracks[static_cast<size_t>(state_.user_tracks_selected_index)], play_next);
    }
}

Element AccountView::render(bool is_focused) {
    auto accent = state_.get_accent_color();
    auto border_col = is_focused ? accent : Color::GrayDark;
    const auto& sym = ThemeSymbols::get(state_.icon_style);

    // Check if not logged in
    if (config_.auth_token.empty()) {
        auto detected_browsers = core::CookieExtractor::detect_installed_browsers();
        std::string browsers_str;
        for (size_t i = 0; i < detected_browsers.size(); ++i) {
            if (i > 0) browsers_str += ", ";
            browsers_str += detected_browsers[i];
        }
        if (browsers_str.empty()) browsers_str = "No browsers detected";

        return vbox({
            hbox({
                text(" " + sym.account + " USER ACCOUNT ") | color(border_col) | bold,
                filler()
            }),
            separator(),
            vbox({
                filler(),
                text("○ No SoundCloud Account Connected") | color(Color::Yellow) | bold | center,
                text(""),
                text("To view your avatar, profile information, and uploaded tracks,") | color(Color::GrayLight) | center,
                text("please log in to SoundCloud or extract existing cookies from your browser.") | color(Color::GrayLight) | center,
                text(""),
                hbox({
                    text("Detected Browsers: ") | color(Color::GrayDark),
                    text(browsers_str) | color(Color::White)
                }) | center,
                text(""),
                hbox({
                    text("Sign In URL: ") | color(Color::GrayDark),
                    text("https://soundcloud.com/signin") | color(Color::CyanLight) | bold
                }) | center,
                text(""),
                hbox({
                    text(" [ " + sym.key + " Press Enter to Auto-Extract Browser Cookies ] ")
                        | bgcolor(accent) | color(Color::Black) | bold
                }) | center,
                filler()
            }) | yflex
        }) | borderRounded | color(border_col) | yflex;
    }

    // Authenticated profile display
    Element avatar_elem;
    if (state_.user_profile.has_value() && state_.user_profile->avatar_image.has_value()) {
        avatar_elem = core::CoverArtProcessor::render_cover_element(*state_.user_profile->avatar_image, config_.image_protocol, 2);
    } else {
        avatar_elem = core::CoverArtProcessor::render_placeholder(18, 9, state_.accent_color);
    }

    const auto& prof = state_.user_profile;
    std::string username = prof ? prof->username : "SoundCloud User";
    std::string permalink = prof ? prof->permalink : "";
    std::string location;
    if (prof) {
        if (!prof->city.empty() && !prof->country.empty()) {
            location = prof->city + ", " + prof->country;
        } else if (!prof->city.empty()) {
            location = prof->city;
        } else if (!prof->country.empty()) {
            location = prof->country;
        }
    }

    std::string member_since;
    if (prof && !prof->created_at.empty()) {
        if (prof->created_at.size() >= 7) {
            std::string year = prof->created_at.substr(0, 4);
            std::string month = prof->created_at.substr(5, 2);
            static const char* kMonths[] = {
                "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
            };
            int m_idx = std::atoi(month.c_str()) - 1;
            if (m_idx >= 0 && m_idx < 12) {
                member_since = std::string(kMonths[m_idx]) + " " + year;
            } else {
                member_since = year;
            }
        }
    }

    // First line: Username, Handle, Badges
    Elements name_line_elems;
    name_line_elems.push_back(text(username) | color(accent) | bold);
    if (!permalink.empty()) {
        name_line_elems.push_back(text(" @" + permalink) | color(Color::GrayLight));
    }
    if (prof && prof->is_verified) {
        name_line_elems.push_back(text(" [✓ Verified]") | color(Color::CyanLight) | bold);
    }
    if (prof && prof->is_pro) {
        name_line_elems.push_back(text(" [PRO]") | color(Color::RGB(255, 140, 0)) | bold);
    }
    std::string plan_str = (prof && !prof->plan.empty()) ? prof->plan : "Free";
    name_line_elems.push_back(text(" [" + plan_str + "]") | color(Color::GrayDark));

    // Second line: Meta info (Location, Member since, Clean profile link)
    Elements meta_line_elems;
    bool has_meta = false;
    if (!location.empty()) {
        meta_line_elems.push_back(text(location) | color(Color::White));
        has_meta = true;
    }
    if (!member_since.empty()) {
        if (has_meta) meta_line_elems.push_back(text(" • ") | color(Color::GrayDark));
        meta_line_elems.push_back(text("Joined " + member_since) | color(Color::GrayLight));
        has_meta = true;
    }
    if (!permalink.empty()) {
        if (has_meta) meta_line_elems.push_back(text(" • ") | color(Color::GrayDark));
        meta_line_elems.push_back(text("soundcloud.com/" + permalink) | color(Color::CyanLight));
    }

    // Third line: Badges
    uint64_t followers = prof ? prof->followers_count : 0;
    uint64_t followings = prof ? prof->followings_count : 0;
    uint64_t track_count = prof ? prof->track_count : state_.user_tracks.size();
    uint64_t likes_count = prof ? prof->likes_count : state_.likes_tracks.size();
    uint64_t playlists_count = prof ? prof->playlist_count : 0;
    uint64_t reposts_count = prof ? prof->reposts_count : 0;

    auto make_badge = [&](const std::string& icon, const std::string& count_str, const std::string& label) {
        return hbox({
            text(" " + icon + " ") | color(accent) | bold,
            text(count_str) | color(Color::White) | bold,
            text(" " + label + " ") | color(Color::GrayLight)
        }) | border | color(Color::RGB(40, 40, 45));
    };

    Elements badges;
    badges.push_back(make_badge(sym.account, std::to_string(followers), "Followers"));
    badges.push_back(text(" "));
    badges.push_back(make_badge(sym.group, std::to_string(followings), "Following"));
    badges.push_back(text(" "));
    badges.push_back(make_badge(sym.note, std::to_string(track_count), "Tracks"));
    badges.push_back(text(" "));
    badges.push_back(make_badge(sym.likes, std::to_string(likes_count), "Likes"));
    if (playlists_count > 0) {
        badges.push_back(text(" "));
        badges.push_back(make_badge(sym.playlists, std::to_string(playlists_count), "Playlists"));
    }
    if (reposts_count > 0) {
        badges.push_back(text(" "));
        badges.push_back(make_badge("↻", std::to_string(reposts_count), "Reposts"));
    }

    // Fourth line: Bio / Description
    std::string desc = (prof && !prof->description.empty()) ? prof->description : "SoundCloud Artist Profile";

    auto profile_card = hbox({
        avatar_elem | size(WIDTH, EQUAL, 18) | size(HEIGHT, EQUAL, 9),
        text("  "),
        vbox({
            hbox(std::move(name_line_elems)),
            text(""),
            hbox(std::move(meta_line_elems)),
            text(""),
            hbox(std::move(badges)),
            text(""),
            text(core::truncate_string(desc, 90)) | color(Color::GrayLight)
        }) | xflex
    });

    // Tracks section
    Elements track_rows;
    if (state_.user_tracks.empty()) {
        if (state_.is_loading) {
            track_rows.push_back(text("  Loading tracks...") | color(Color::GrayLight));
        } else {
            track_rows.push_back(text("  No uploaded tracks found on this account.") | color(Color::GrayLight));
        }
    } else {
        int viewport_h = std::max(3, state_.terminal_height - 23);
        int selected_idx = std::clamp(state_.user_tracks_selected_index, 0, static_cast<int>(state_.user_tracks.size() - 1));

        if (selected_idx < scroll_offset_) {
            scroll_offset_ = selected_idx;
        } else if (selected_idx >= scroll_offset_ + viewport_h) {
            scroll_offset_ = selected_idx - viewport_h + 1;
        }

        int start_idx = scroll_offset_;
        int end_idx = std::min(static_cast<int>(state_.user_tracks.size()), start_idx + viewport_h);

        for (int i = start_idx; i < end_idx; ++i) {
            const auto& track = state_.user_tracks[static_cast<size_t>(i)];
            bool is_selected = (i == state_.user_tracks_selected_index && is_focused);
            bool is_active = (state_.active_track.has_value() && state_.active_track->id == track.id);

            Elements cols;
            if (is_active) {
                cols.push_back(text(" " + sym.play + " ") | color(accent) | bold);
            } else {
                cols.push_back(text("   "));
            }

            cols.push_back(text(std::to_string(i + 1) + ". ") | color(Color::GrayDark));

            std::string title = core::truncate_string(track.title, 38);
            Elements title_elem;
            title_elem.push_back(text(title) | color(is_active ? accent : (is_selected ? Color::White : Color::GrayLight)) | bold | xflex);

            if (track.is_drm_protected()) {
                Color drm_col = state_.drm_engine_enabled ? Color::GrayLight : Color::RedLight;
                title_elem.push_back(text(" [DRM]") | color(drm_col) | (state_.drm_engine_enabled ? dim : bold));
            } else if (track.is_go_plus()) {
                title_elem.push_back(text(" [GO+]") | color(Color::RGB(255, 140, 0)) | bold);
            }
            cols.push_back(hbox(std::move(title_elem)) | xflex);

            cols.push_back(text(" " + sym.note + " " + std::to_string(track.playback_count) + " ") | color(Color::GrayDark));
            cols.push_back(text(" " + sym.likes + " " + std::to_string(track.likes_count) + " ") | color(Color::GrayDark));
            cols.push_back(text(" " + core::format_duration_ms(track.duration_ms) + " ") | color(Color::GrayDark));

            auto row = hbox(std::move(cols));
            if (is_selected) {
                row = row | bgcolor(Color::RGB(
                    state_.accent_color.r / 6,
                    state_.accent_color.g / 6,
                    state_.accent_color.b / 6
                ));
            }
            track_rows.push_back(row);
        }
    }

    return vbox({
        hbox({
            text(" " + sym.account + " USER ACCOUNT & PROFILE ") | color(border_col) | bold,
            filler(),
            text(state_.user_tracks.empty() ? "" : std::to_string(state_.user_tracks.size()) + " tracks") | color(Color::GrayDark)
        }),
        separator(),
        profile_card,
        separatorLight(),
        hbox({
            text(" " + sym.note + " UPLOADED TRACKS ") | color(Color::White) | bold,
            filler(),
            text(" [Enter] Play  [a] Queue  [A] Next  [l] Like  [h/Esc] Back ") | color(Color::GrayDark)
        }),
        vbox(std::move(track_rows)) | yflex
    }) | borderRounded | color(border_col) | yflex;
}

} // namespace sctui::ui::views
