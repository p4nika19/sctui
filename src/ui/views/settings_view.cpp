#include "sctui/ui/views/settings_view.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include "sctui/core/cover_art.hpp"
#include "sctui/core/cookie_extractor.hpp"
#include "sctui/audio/drm_daemon.hpp"
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>
#include <cstdlib>

namespace sctui::ui::views {

using namespace ftxui;

namespace {
constexpr size_t kCacheSizes[] = { 256, 512, 1024, 2048, 4096, 8192, 0 }; // 0 = unlimited
constexpr int kNumCacheSizes = sizeof(kCacheSizes) / sizeof(kCacheSizes[0]);
constexpr int kTotalFields = 19;
}

SettingsView::SettingsView(
    models::AppState& state,
    core::Config& config,
    std::shared_ptr<core::TrackCacheManager> cache_mgr,
    SaveCallback on_save,
    ExtractCookiesCallback on_extract_cookies,
    RefreshClientIdCallback on_refresh_client_id,
    ClearCacheCallback on_clear_cache,
    DownloadAllLikesCallback on_download_all_likes
) : state_(state),
    config_(config),
    cache_mgr_(std::move(cache_mgr)),
    on_save_(std::move(on_save)),
    on_extract_cookies_(std::move(on_extract_cookies)),
    on_refresh_client_id_(std::move(on_refresh_client_id)),
    on_clear_cache_(std::move(on_clear_cache)),
    on_download_all_likes_(std::move(on_download_all_likes)) {

    reload_from_config();

    doh_input_comp_ = Input(&doh_url_input_, "https://cloudflare-dns.com/dns-query");
    dns_input_comp_ = Input(&custom_dns_input_, "1.1.1.1");
    proxy_input_comp_ = Input(&proxy_url_input_, "socks5://127.0.0.1:1080 or http://127.0.0.1:7890");

    auto inner_container = Container::Vertical({
        doh_input_comp_,
        dns_input_comp_,
        proxy_input_comp_
    });

    component_ = CatchEvent(inner_container, [this](Event event) {
        if (state_.focused_panel != models::FocusPanel::SettingsPanel) {
            return false;
        }

        // Return focus to sidebar on Escape
        if (event == Event::Escape) {
            state_.focused_panel = models::FocusPanel::Sidebar;
            return true;
        }

        // Dedicated text editing mode for URL / IP text input fields
        if (is_editing_text()) {
            if (event == Event::Escape) {
                state_.focused_panel = models::FocusPanel::Sidebar;
                return true;
            }
            if (event == Event::Tab || event == Event::ArrowDown || event == Event::Return) {
                do {
                    selected_field_ = (selected_field_ + 1) % kTotalFields;
                } while (!is_field_visible(selected_field_));
                if (selected_field_ == 7) {
                    if (dns_mode_idx_ == 1) doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
                    else if (dns_mode_idx_ == 2) dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
                }
                if (selected_field_ == 8) proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
                return true;
            }
            if (event == Event::TabReverse || event == Event::ArrowUp) {
                do {
                    selected_field_ = (selected_field_ + kTotalFields - 1) % kTotalFields;
                } while (!is_field_visible(selected_field_));
                if (selected_field_ == 7) {
                    if (dns_mode_idx_ == 1) doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
                    else if (dns_mode_idx_ == 2) dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
                }
                if (selected_field_ == 8) proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
                return true;
            }

            std::string* active_text = nullptr;
            int* active_cursor = nullptr;
            if (selected_field_ == 7) {
                if (dns_mode_idx_ == 1 && doh_provider_idx_ == 3) {
                    active_text = &doh_url_input_;
                    active_cursor = &doh_cursor_pos_;
                } else if (dns_mode_idx_ == 2) {
                    active_text = &custom_dns_input_;
                    active_cursor = &dns_cursor_pos_;
                }
            } else if (selected_field_ == 8) {
                active_text = &proxy_url_input_;
                active_cursor = &proxy_cursor_pos_;
            }

            if (!active_text || !active_cursor) {
                return false;
            }

            if (event == Event::ArrowLeft) {
                if (*active_cursor > 0) {
                    (*active_cursor)--;
                }
                return true;
            }
            if (event == Event::ArrowRight) {
                if (*active_cursor < static_cast<int>(active_text->size())) {
                    (*active_cursor)++;
                }
                return true;
            }
            if (event == Event::Home || event == Event::CtrlA) {
                *active_cursor = 0;
                return true;
            }
            if (event == Event::End || event == Event::CtrlE) {
                *active_cursor = static_cast<int>(active_text->size());
                return true;
            }
            if (event == Event::Backspace) {
                if (*active_cursor > 0 && *active_cursor <= static_cast<int>(active_text->size())) {
                    active_text->erase(*active_cursor - 1, 1);
                    (*active_cursor)--;
                }
                return true;
            }
            if (event == Event::Delete) {
                if (*active_cursor < static_cast<int>(active_text->size())) {
                    active_text->erase(*active_cursor, 1);
                }
                return true;
            }
            if (event == Event::CtrlU) {
                if (*active_cursor > 0) {
                    active_text->erase(0, *active_cursor);
                    *active_cursor = 0;
                }
                return true;
            }
            if (event == Event::CtrlW) {
                if (*active_cursor > 0) {
                    int p = *active_cursor;
                    while (p > 0 && active_text->at(p - 1) == ' ') p--;
                    while (p > 0 && active_text->at(p - 1) != ' ' && active_text->at(p - 1) != '/' && active_text->at(p - 1) != ':') p--;
                    active_text->erase(p, *active_cursor - p);
                    *active_cursor = p;
                }
                return true;
            }

            if (event.is_character()) {
                const auto& ch_str = event.character();
                active_text->insert(*active_cursor, ch_str);
                *active_cursor += static_cast<int>(ch_str.size());
                return true;
            }

            return true;
        }

        if (event == Event::ArrowDown || event == Event::Character('j') || event == Event::Tab) {
            do {
                selected_field_ = (selected_field_ + 1) % kTotalFields;
            } while (!is_field_visible(selected_field_));
            if (selected_field_ == 7) {
                if (dns_mode_idx_ == 1) doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
                else if (dns_mode_idx_ == 2) dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
            }
            if (selected_field_ == 8) proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
            return true;
        }

        if (event == Event::ArrowUp || event == Event::Character('k') || event == Event::TabReverse) {
            do {
                selected_field_ = (selected_field_ + kTotalFields - 1) % kTotalFields;
            } while (!is_field_visible(selected_field_));
            if (selected_field_ == 7) {
                if (dns_mode_idx_ == 1) doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
                else if (dns_mode_idx_ == 2) dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
            }
            if (selected_field_ == 8) proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
            return true;
        }

        // Left/Right adjustments
        if (event == Event::ArrowLeft || event == Event::Character('h')) {
            if (selected_field_ == 2) { // Image protocol
                image_protocol_idx_ = (image_protocol_idx_ + 4) % 5;
                return true;
            }
            if (selected_field_ == 3) { // Icon Style
                icon_style_idx_ = (icon_style_idx_ + 2) % 3;
                state_.icon_style = static_cast<core::IconStyle>(icon_style_idx_);
                return true;
            }
            if (selected_field_ == 4) { // Fullscreen Visualizer Style
                fullscreen_visualizer_style_ = static_cast<core::FullscreenVisualizerStyle>((static_cast<int>(fullscreen_visualizer_style_) + 2) % 3);
                return true;
            }
            if (selected_field_ == 5) { // DNS Mode
                dns_mode_idx_ = (dns_mode_idx_ + 2) % 3;
                return true;
            }
            if (selected_field_ == 6) { // DoH Provider
                doh_provider_idx_ = (doh_provider_idx_ + 3) % 4;
                return true;
            }
            if (selected_field_ == 9) { // Max cache size
                int cur_idx = 2;
                for (int i = 0; i < kNumCacheSizes; ++i) {
                    if (kCacheSizes[i] == max_cache_size_mb_) { cur_idx = i; break; }
                }
                cur_idx = (cur_idx + kNumCacheSizes - 1) % kNumCacheSizes;
                max_cache_size_mb_ = kCacheSizes[cur_idx];
                return true;
            }
            if (selected_field_ == 10) { // Lock liked in cache
                lock_liked_tracks_in_cache_ = !lock_liked_tracks_in_cache_;
                return true;
            }
            if (selected_field_ == 11) { // Don't count liked in general cache
                dont_count_liked_in_general_cache_ = !dont_count_liked_in_general_cache_;
                return true;
            }
            if (selected_field_ == 14) { // Discord Rich Presence
                discord_rpc_enabled_ = !discord_rpc_enabled_;
                return true;
            }
            if (selected_field_ == 15) { // Mouse Controls
                mouse_control_ = !mouse_control_;
                return true;
            }
            if (selected_field_ == 16) { // DRM Playback Engine
                drm_engine_enabled_ = !drm_engine_enabled_;
                return true;
            }
            if (selected_field_ == 17) { // Download DRM in Likes
                download_drm_tracks_in_likes_ = !download_drm_tracks_in_likes_;
                return true;
            }
            if (selected_field_ != 7 && selected_field_ != 8) {
                state_.focused_panel = models::FocusPanel::Sidebar;
                return true;
            }
        }

        if (event == Event::ArrowRight || event == Event::Character('l')) {
            if (selected_field_ == 2) {
                image_protocol_idx_ = (image_protocol_idx_ + 1) % 5;
                return true;
            }
            if (selected_field_ == 3) {
                icon_style_idx_ = (icon_style_idx_ + 1) % 3;
                state_.icon_style = static_cast<core::IconStyle>(icon_style_idx_);
                return true;
            }
            if (selected_field_ == 4) {
                fullscreen_visualizer_style_ = static_cast<core::FullscreenVisualizerStyle>((static_cast<int>(fullscreen_visualizer_style_) + 1) % 3);
                return true;
            }
            if (selected_field_ == 5) {
                dns_mode_idx_ = (dns_mode_idx_ + 1) % 3;
                return true;
            }
            if (selected_field_ == 6) {
                doh_provider_idx_ = (doh_provider_idx_ + 1) % 4;
                return true;
            }
            if (selected_field_ == 9) {
                int cur_idx = 2;
                for (int i = 0; i < kNumCacheSizes; ++i) {
                    if (kCacheSizes[i] == max_cache_size_mb_) { cur_idx = i; break; }
                }
                cur_idx = (cur_idx + 1) % kNumCacheSizes;
                max_cache_size_mb_ = kCacheSizes[cur_idx];
                return true;
            }
            if (selected_field_ == 10) {
                lock_liked_tracks_in_cache_ = !lock_liked_tracks_in_cache_;
                return true;
            }
            if (selected_field_ == 11) {
                dont_count_liked_in_general_cache_ = !dont_count_liked_in_general_cache_;
                return true;
            }
            if (selected_field_ == 14) {
                discord_rpc_enabled_ = !discord_rpc_enabled_;
                return true;
            }
            if (selected_field_ == 15) {
                mouse_control_ = !mouse_control_;
                return true;
            }
            if (selected_field_ == 16) {
                drm_engine_enabled_ = !drm_engine_enabled_;
                return true;
            }
            if (selected_field_ == 17) {
                download_drm_tracks_in_likes_ = !download_drm_tracks_in_likes_;
                return true;
            }
        }

        // Return / Space to trigger actions or cycle
        if (event == Event::Return || event == Event::Character(' ')) {
            if (selected_field_ == 0) {
                if (on_extract_cookies_) {
                    on_extract_cookies_();
                    reload_from_config();
                }
                return true;
            }
            if (selected_field_ == 1) {
                if (on_refresh_client_id_) {
                    on_refresh_client_id_();
                }
                return true;
            }
            if (selected_field_ == 2) {
                image_protocol_idx_ = (image_protocol_idx_ + 1) % 5;
                return true;
            }
            if (selected_field_ == 3) {
                icon_style_idx_ = (icon_style_idx_ + 1) % 3;
                state_.icon_style = static_cast<core::IconStyle>(icon_style_idx_);
                return true;
            }
            if (selected_field_ == 4) {
                fullscreen_visualizer_style_ = static_cast<core::FullscreenVisualizerStyle>((static_cast<int>(fullscreen_visualizer_style_) + 1) % 3);
                return true;
            }
            if (selected_field_ == 5) {
                dns_mode_idx_ = (dns_mode_idx_ + 1) % 3;
                return true;
            }
            if (selected_field_ == 6) {
                doh_provider_idx_ = (doh_provider_idx_ + 1) % 4;
                return true;
            }
            if (selected_field_ == 9) {
                int cur_idx = 2;
                for (int i = 0; i < kNumCacheSizes; ++i) {
                    if (kCacheSizes[i] == max_cache_size_mb_) { cur_idx = i; break; }
                }
                cur_idx = (cur_idx + 1) % kNumCacheSizes;
                max_cache_size_mb_ = kCacheSizes[cur_idx];
                return true;
            }
            if (selected_field_ == 10) {
                lock_liked_tracks_in_cache_ = !lock_liked_tracks_in_cache_;
                return true;
            }
            if (selected_field_ == 11) {
                dont_count_liked_in_general_cache_ = !dont_count_liked_in_general_cache_;
                return true;
            }
            if (selected_field_ == 12) { // Download All Likes
                if (on_download_all_likes_) {
                    on_download_all_likes_();
                }
                return true;
            }
            if (selected_field_ == 13) { // Clear Cache
                if (on_clear_cache_) {
                    on_clear_cache_(lock_liked_tracks_in_cache_);
                    feedback_message_ = "Cache cleaned successfully!";
                    feedback_is_error_ = false;
                }
                return true;
            }
            if (selected_field_ == 14) { // Discord Rich Presence
                discord_rpc_enabled_ = !discord_rpc_enabled_;
                return true;
            }
            if (selected_field_ == 15) { // Mouse Controls
                mouse_control_ = !mouse_control_;
                return true;
            }
            if (selected_field_ == 16) { // DRM Playback Engine
                drm_engine_enabled_ = !drm_engine_enabled_;
                return true;
            }
            if (selected_field_ == 17) { // Download DRM in Likes
                download_drm_tracks_in_likes_ = !download_drm_tracks_in_likes_;
                return true;
            }
            if (selected_field_ == 18) { // Save & Apply
                config_.image_protocol = static_cast<core::ImageProtocol>(image_protocol_idx_);
                config_.icon_style = static_cast<core::IconStyle>(icon_style_idx_);
                state_.icon_style = config_.icon_style;
                config_.dns_mode = static_cast<core::DnsMode>(dns_mode_idx_);
                config_.doh_provider = static_cast<core::DohProvider>(doh_provider_idx_);
                config_.doh_url = doh_url_input_;
                config_.custom_dns_server = custom_dns_input_;
                config_.proxy_url = proxy_url_input_;
                config_.max_cache_size_mb = max_cache_size_mb_;
                config_.lock_liked_tracks_in_cache = lock_liked_tracks_in_cache_;
                config_.dont_count_liked_in_general_cache = dont_count_liked_in_general_cache_;
                config_.discord_rpc_enabled = discord_rpc_enabled_;
                config_.mouse_control = mouse_control_;
                config_.drm_engine_enabled = drm_engine_enabled_;
                config_.download_drm_tracks_in_likes = download_drm_tracks_in_likes_;
                config_.fullscreen_visualizer_style = fullscreen_visualizer_style_;
                config_.fullscreen_visualizer_mirrored = (fullscreen_visualizer_style_ != core::FullscreenVisualizerStyle::Stretched);
                state_.drm_engine_enabled = drm_engine_enabled_;
                state_.download_drm_tracks_in_likes = download_drm_tracks_in_likes_;

                if (on_save_) {
                    on_save_(config_);
                }
                feedback_message_ = "Settings successfully saved and applied!";
                feedback_is_error_ = false;
                return true;
            }
        }

        // Forward text events to active text inputs
        if (selected_field_ == 7) {
            if (dns_mode_idx_ == 1 && doh_provider_idx_ == 3) {
                return doh_input_comp_->OnEvent(event);
            }
            if (dns_mode_idx_ == 2) {
                return dns_input_comp_->OnEvent(event);
            }
        }
        if (selected_field_ == 8) {
            return proxy_input_comp_->OnEvent(event);
        }

        return false;
    });
}

void SettingsView::reload_from_config() {
    image_protocol_idx_ = static_cast<int>(config_.image_protocol);
    icon_style_idx_ = static_cast<int>(config_.icon_style);
    dns_mode_idx_ = static_cast<int>(config_.dns_mode);
    doh_provider_idx_ = static_cast<int>(config_.doh_provider);
    doh_url_input_ = config_.doh_url;
    custom_dns_input_ = config_.custom_dns_server;
    proxy_url_input_ = config_.proxy_url;
    doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
    dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
    proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
    max_cache_size_mb_ = config_.max_cache_size_mb;
    lock_liked_tracks_in_cache_ = config_.lock_liked_tracks_in_cache;
    dont_count_liked_in_general_cache_ = config_.dont_count_liked_in_general_cache;
    discord_rpc_enabled_ = config_.discord_rpc_enabled;
    mouse_control_ = config_.mouse_control;
    drm_engine_enabled_ = config_.drm_engine_enabled;
    download_drm_tracks_in_likes_ = config_.download_drm_tracks_in_likes;
    fullscreen_visualizer_style_ = config_.fullscreen_visualizer_style;
}

void SettingsView::reset_selection() {
    selected_field_ = 0;
}

bool SettingsView::is_field_visible(int field_idx) const {
    if (field_idx == 6) { // DoH Provider
        return dns_mode_idx_ == 1;
    }
    if (field_idx == 7) { // Custom DoH URL or Custom DNS IP
        return (dns_mode_idx_ == 1 && doh_provider_idx_ == 3) || (dns_mode_idx_ == 2);
    }
    return true;
}

bool SettingsView::is_editing_text() const {
    if (selected_field_ == 7) {
        return (dns_mode_idx_ == 1 && doh_provider_idx_ == 3) || (dns_mode_idx_ == 2);
    }
    if (selected_field_ == 8) {
        return true;
    }
    return false;
}

void SettingsView::select_field(int field_idx) {
    if (field_idx >= 0 && field_idx < kTotalFields && is_field_visible(field_idx)) {
        selected_field_ = field_idx;
        if (selected_field_ == 7) {
            if (dns_mode_idx_ == 1) {
                doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
            } else if (dns_mode_idx_ == 2) {
                dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
            }
        } else if (selected_field_ == 8) {
            proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
        }
    }
}

bool SettingsView::handle_mouse_click(int y, [[maybe_unused]] int x) {
    if (y == login_url_row_) {
        int ret = std::system("xdg-open https://soundcloud.com/signin >/dev/null 2>&1 &");
        (void)ret;
        state_.notify("Opened https://soundcloud.com/signin in default browser", false, 4);
        return true;
    }

    auto it = row_to_field_.find(y);
    if (it == row_to_field_.end()) {
        return false;
    }

    int field = it->second;
    select_field(field);

    if (field == 0) { // Auto-Extract Cookies
        if (on_extract_cookies_) {
            on_extract_cookies_();
            reload_from_config();
        }
        return true;
    }
    if (field == 1) { // Refresh Client ID
        if (on_refresh_client_id_) {
            on_refresh_client_id_();
        }
        return true;
    }
    if (field == 2) { // Cover Art Protocol
        image_protocol_idx_ = (image_protocol_idx_ + 1) % 5;
        return true;
    }
    if (field == 3) { // Icon Style
        icon_style_idx_ = (icon_style_idx_ + 1) % 3;
        state_.icon_style = static_cast<core::IconStyle>(icon_style_idx_);
        return true;
    }
    if (field == 4) { // Fullscreen Visualizer Style
        fullscreen_visualizer_style_ = static_cast<core::FullscreenVisualizerStyle>((static_cast<int>(fullscreen_visualizer_style_) + 1) % 3);
        return true;
    }
    if (field == 5) { // DNS Mode
        dns_mode_idx_ = (dns_mode_idx_ + 1) % 3;
        return true;
    }
    if (field == 6) { // DoH Provider
        doh_provider_idx_ = (doh_provider_idx_ + 1) % 4;
        return true;
    }
    if (field == 9) { // Max Cache Size
        int cur_idx = 2;
        for (int i = 0; i < kNumCacheSizes; ++i) {
            if (kCacheSizes[i] == max_cache_size_mb_) { cur_idx = i; break; }
        }
        cur_idx = (cur_idx + 1) % kNumCacheSizes;
        max_cache_size_mb_ = kCacheSizes[cur_idx];
        return true;
    }
    if (field == 10) { // Lock liked tracks in cache
        lock_liked_tracks_in_cache_ = !lock_liked_tracks_in_cache_;
        return true;
    }
    if (field == 11) { // Don't count liked in general cache
        dont_count_liked_in_general_cache_ = !dont_count_liked_in_general_cache_;
        return true;
    }
    if (field == 12) { // Download all likes
        if (on_download_all_likes_) {
            on_download_all_likes_();
        }
        return true;
    }
    if (field == 13) { // Clear cache
        if (on_clear_cache_) {
            on_clear_cache_(lock_liked_tracks_in_cache_);
            feedback_message_ = "Cache cleaned successfully!";
            feedback_is_error_ = false;
        }
        return true;
    }
    if (field == 14) { // Discord Rich Presence
        discord_rpc_enabled_ = !discord_rpc_enabled_;
        return true;
    }
    if (field == 15) { // Mouse Controls
        mouse_control_ = !mouse_control_;
        return true;
    }
    if (field == 16) { // DRM Playback Engine
        drm_engine_enabled_ = !drm_engine_enabled_;
        return true;
    }
    if (field == 17) { // Download DRM in Likes
        download_drm_tracks_in_likes_ = !download_drm_tracks_in_likes_;
        return true;
    }
    if (field == 18) { // Apply Changes
        config_.image_protocol = static_cast<core::ImageProtocol>(image_protocol_idx_);
        config_.icon_style = static_cast<core::IconStyle>(icon_style_idx_);
        state_.icon_style = config_.icon_style;
        config_.dns_mode = static_cast<core::DnsMode>(dns_mode_idx_);
        config_.doh_provider = static_cast<core::DohProvider>(doh_provider_idx_);
        config_.doh_url = doh_url_input_;
        config_.custom_dns_server = custom_dns_input_;
        config_.proxy_url = proxy_url_input_;
        config_.max_cache_size_mb = max_cache_size_mb_;
        config_.lock_liked_tracks_in_cache = lock_liked_tracks_in_cache_;
        config_.dont_count_liked_in_general_cache = dont_count_liked_in_general_cache_;
        config_.discord_rpc_enabled = discord_rpc_enabled_;
        config_.mouse_control = mouse_control_;
        config_.drm_engine_enabled = drm_engine_enabled_;
        config_.download_drm_tracks_in_likes = download_drm_tracks_in_likes_;
        config_.fullscreen_visualizer_style = fullscreen_visualizer_style_;
        config_.fullscreen_visualizer_mirrored = (fullscreen_visualizer_style_ != core::FullscreenVisualizerStyle::Stretched);
        state_.drm_engine_enabled = drm_engine_enabled_;
        state_.download_drm_tracks_in_likes = download_drm_tracks_in_likes_;

        if (on_save_) {
            on_save_(config_);
        }
        feedback_message_ = "Settings successfully applied and saved!";
        feedback_is_error_ = false;
        return true;
    }

    return true;
}

Component SettingsView::get_component() {
    return component_;
}

Element SettingsView::render(bool is_focused) {
    auto accent = state_.get_accent_color();
    auto border_col = is_focused ? accent : Color::GrayDark;
    const auto& sym = ThemeSymbols::get(static_cast<core::IconStyle>(icon_style_idx_));

    auto detected_proto = core::CoverArtProcessor::detect_terminal_graphics_protocol();
    std::string detected_proto_str = core::Config::image_protocol_to_string(detected_proto);

    Elements content;
    row_to_field_.clear();
    login_url_row_ = -1;

    auto render_row = [&](int field_index, const std::string& label, Element control, const std::string& hint = "") {
        int current_y = 4 + static_cast<int>(content.size());
        row_to_field_[current_y] = field_index;

        bool selected = (selected_field_ == field_index && is_focused);
        auto prefix = selected ? text(" ► ") | color(accent) | bold : text("   ");
        auto label_elem = text(label) | (selected ? color(Color::White) | bold : color(Color::GrayLight));

        Elements row_parts = {
            prefix,
            label_elem | size(WIDTH, EQUAL, 28),
            control
        };

        if (!hint.empty()) {
            row_parts.push_back(filler());
            row_parts.push_back(text(hint) | color(Color::GrayDark));
        }

        auto row = hbox(std::move(row_parts));
        if (selected) {
            row = row | bgcolor(Color::RGB(
                state_.accent_color.r / 7,
                state_.accent_color.g / 7,
                state_.accent_color.b / 7
            ));
        }
        return row;
    };

    // 1. Account info & auto-extract cookies
    std::string auth_status_text;
    Color auth_status_color = Color::RedLight;
    if (!config_.auth_token.empty()) {
        std::string masked = config_.auth_token.substr(0, std::min<size_t>(6, config_.auth_token.size())) + "••••••••";
        auth_status_text = "● Authenticated (" + masked + ")";
        if (!config_.extracted_browser.empty()) {
            auth_status_text += " [via " + config_.extracted_browser + "]";
        }
        auth_status_color = Color::GreenLight;
    } else {
        auth_status_text = "○ Not Authenticated (Guest Mode)";
    }

    // Auto-extract button with common color (White) when not selected
    auto btn_extract = text(" [ " + sym.key + " Auto-Extract Browser Cookies ] ")
        | (selected_field_ == 0 && is_focused ? bgcolor(accent) | color(Color::Black) | bold : color(Color::White));

    auto btn_refresh_cid = text(" [ " + sym.buffering + " Refresh Client ID ] ")
        | (selected_field_ == 1 && is_focused ? bgcolor(accent) | color(Color::Black) | bold : color(Color::White));

    auto detected_browsers = core::CookieExtractor::detect_installed_browsers();
    std::string detected_str;
    if (detected_browsers.empty()) {
        detected_str = "None found";
    } else {
        for (size_t i = 0; i < detected_browsers.size(); ++i) {
            if (i > 0) detected_str += ", ";
            detected_str += detected_browsers[i];
        }
    }

    // 2. Cover art protocol selector
    const char* protos[] = {
        "Auto (Auto-Detect)",
        "Kitty Graphics Protocol (High-Res)",
        "iTerm2 Inline Images Protocol (High-Res)",
        "Pixelated (Unicode Half-Block 24-bit)",
        "Disabled"
    };
    std::string proto_display = "< " + std::string(protos[std::clamp(image_protocol_idx_, 0, 4)]) + " >";
    auto proto_elem = text(proto_display)
        | (selected_field_ == 2 && is_focused ? color(accent) | bold : color(Color::White));

    // 3. UI Icon & Symbols Style selector
    const char* icon_styles[] = {
        "Emoji (Color Emojis: ☁, 🔍, 👤, 💖, 📁)",
        "Unicode (Clean Symbols: ☁, ⌕, ☺, ♥, ≡)",
        "Default (Plain ASCII: [SC], [/], [@], <3>)"
    };
    std::string icon_style_display = "< " + std::string(icon_styles[std::clamp(icon_style_idx_, 0, 2)]) + " >";
    auto icon_style_elem = text(icon_style_display)
        | (selected_field_ == 3 && is_focused ? color(accent) | bold : color(Color::White));

    // 5. DNS Mode selector
    const char* dns_modes[] = { "System Default", "DNS-over-HTTPS (DoH)", "Custom DNS Server" };
    std::string dns_mode_display = "< " + std::string(dns_modes[std::clamp(dns_mode_idx_, 0, 2)]) + " >";
    auto dns_mode_elem = text(dns_mode_display)
        | (selected_field_ == 5 && is_focused ? color(accent) | bold : color(Color::White));

    // 6. DoH Provider selector
    const char* doh_provs[] = { "Cloudflare (1.1.1.1)", "Google (8.8.8.8)", "Quad9 (9.9.9.9)", "Custom URL" };
    std::string doh_prov_display = "< " + std::string(doh_provs[std::clamp(doh_provider_idx_, 0, 3)]) + " >";
    auto doh_prov_elem = text(doh_prov_display)
        | (selected_field_ == 6 && is_focused ? color(accent) | bold : color(Color::White));

    auto render_input_box = [accent](const std::string& content, const std::string& placeholder, int cursor_pos, bool is_editing) -> Element {
        Element inner;
        if (content.empty()) {
            if (is_editing) {
                inner = hbox({
                    text(" ") | bgcolor(accent) | color(Color::Black),
                    text(" " + placeholder) | color(Color::GrayDark)
                });
            } else {
                inner = text(placeholder) | color(Color::GrayDark);
            }
        } else if (!is_editing) {
            inner = text(content) | color(Color::White);
        } else {
            int pos = std::clamp(cursor_pos, 0, static_cast<int>(content.size()));
            std::string before = content.substr(0, pos);
            Element cursor_elem;
            std::string after;
            if (pos < static_cast<int>(content.size())) {
                std::string ch = content.substr(pos, 1);
                cursor_elem = text(ch) | bgcolor(accent) | color(Color::Black) | bold;
                after = content.substr(pos + 1);
            } else {
                cursor_elem = text(" ") | bgcolor(accent) | color(Color::Black);
            }
            inner = hbox({
                text(before) | color(Color::White),
                cursor_elem,
                text(after) | color(Color::White)
            });
        }

        auto box = hbox({
            text(" "),
            inner | xflex,
            text(" ")
        }) | size(HEIGHT, EQUAL, 1);

        if (is_editing) {
            return box | borderRounded | color(accent) | bgcolor(Color::RGB(18, 18, 24)) | size(WIDTH, GREATER_THAN, 40);
        } else {
            return box | borderRounded | color(Color::GrayDark) | size(WIDTH, GREATER_THAN, 40);
        }
    };

    // 7. Custom DoH URL or Custom DNS IP
    Element custom_net_elem;
    std::string custom_net_label = "Custom Server Endpoint";
    bool field_7_editing = (selected_field_ == 7 && is_focused);
    if (dns_mode_idx_ == 1 && doh_provider_idx_ == 3) {
        custom_net_label = "Custom DoH URL";
        custom_net_elem = render_input_box(doh_url_input_, "https://cloudflare-dns.com/dns-query", doh_cursor_pos_, field_7_editing);
    } else if (dns_mode_idx_ == 2) {
        custom_net_label = "Custom DNS Server IP";
        custom_net_elem = render_input_box(custom_dns_input_, "1.1.1.1", dns_cursor_pos_, field_7_editing);
    } else {
        custom_net_elem = text("(Not required for current DNS mode)") | color(Color::GrayDark);
    }

    // 8. Proxy URL
    bool field_8_editing = (selected_field_ == 8 && is_focused);
    auto proxy_elem = render_input_box(proxy_url_input_, "socks5://127.0.0.1:1080 or http://127.0.0.1:7890", proxy_cursor_pos_, field_8_editing);

    // 9. Track Cache Settings & Liked Locking
    std::string max_cache_str = (max_cache_size_mb_ == 0) ? "Unlimited" : (std::to_string(max_cache_size_mb_) + " MB");
    auto cache_size_elem = text("< " + max_cache_str + " >")
        | (selected_field_ == 9 && is_focused ? color(accent) | bold : color(Color::White));

    auto liked_lock_elem = text(lock_liked_tracks_in_cache_ ? "< Locked (Never Evict) >" : "< Unlocked >")
        | (selected_field_ == 10 && is_focused ? color(accent) | bold : color(lock_liked_tracks_in_cache_ ? Color::GreenLight : Color::GrayLight));

    auto dont_count_elem = text(dont_count_liked_in_general_cache_ ? "< Yes (Excluded from limit) >" : "< No (Counts toward limit) >")
        | (selected_field_ == 11 && is_focused ? color(accent) | bold : color(Color::White));

    // 12. Download All Likes to Cache button
    std::string download_likes_label;
    if (state_.is_downloading_likes) {
        int pct = (state_.likes_download_total > 0)
            ? static_cast<int>((state_.likes_download_current * 100) / state_.likes_download_total)
            : 0;
        download_likes_label = " [ " + sym.buffering + " Downloading Likes: " +
            std::to_string(state_.likes_download_current) + "/" +
            std::to_string(state_.likes_download_total) + " (" + std::to_string(pct) + "%) ] ";
    } else {
        download_likes_label = " [ " + sym.likes + " Download All Likes to Cache ] ";
    }
    auto btn_download_likes = text(download_likes_label)
        | (selected_field_ == 12 && is_focused ? bgcolor(accent) | color(Color::Black) | bold : color(Color::White));

    // 13. Cache Maintenance
    auto btn_clean_cache = text(" [ " + sym.clean + " Clean Regular Cache (Keep Liked) ] ")
        | (selected_field_ == 13 && is_focused ? bgcolor(accent) | color(Color::Black) | bold : color(Color::YellowLight));

    // Cache Stats
    std::string stats_str;
    if (cache_mgr_) {
        auto stats = cache_mgr_->get_stats(max_cache_size_mb_, lock_liked_tracks_in_cache_, dont_count_liked_in_general_cache_);
        uint64_t total_mb = stats.total_size_bytes / (1024 * 1024);
        stats_str = std::to_string(total_mb) + " MB (" + std::to_string(stats.total_tracks) +
                    " tracks: " + std::to_string(stats.regular_tracks) + " regular, " +
                    std::to_string(stats.liked_tracks) + " liked)";
    } else {
        stats_str = "Cache manager active";
    }

    // 14. Discord Rich Presence
    auto discord_rpc_elem = text(discord_rpc_enabled_ ? "< Enabled >" : "< Disabled >")
        | (selected_field_ == 14 && is_focused ? color(accent) | bold : color(discord_rpc_enabled_ ? Color::GreenLight : Color::GrayLight));

    // 15. TUI Mouse Controls
    auto mouse_ctrl_elem = text(mouse_control_ ? "< Enabled >" : "< Disabled >")
        | (selected_field_ == 15 && is_focused ? color(accent) | bold : color(mouse_control_ ? Color::GreenLight : Color::GrayLight));

    // 16. DRM Playback Engine
    bool electron_installed = audio::DrmSatelliteDaemon::is_castlabs_electron_installed();
    std::string drm_status_str;
    ftxui::Color drm_status_color = Color::GrayLight;
    if (!drm_engine_enabled_) {
        drm_status_str = "< Disabled >";
        drm_status_color = Color::GrayLight;
    } else if (state_.is_downloading_electron) {
        drm_status_str = "< Downloading Electron... >";
        drm_status_color = Color::YellowLight;
    } else if (electron_installed) {
        drm_status_str = "< Enabled (CastLabs Daemon) >";
        drm_status_color = Color::GreenLight;
    } else {
        drm_status_str = "< Enabled (Download Required) >";
        drm_status_color = Color::YellowLight;
    }

    auto drm_elem = text(drm_status_str)
        | (selected_field_ == 16 && is_focused ? color(accent) | bold : color(drm_status_color));

    // 17. Download DRM in Likes
    auto drm_download_elem = text(download_drm_tracks_in_likes_ ? "< Enabled (Non-GO tracks) >" : "< Disabled >")
        | (selected_field_ == 17 && is_focused ? color(accent) | bold : color(download_drm_tracks_in_likes_ ? Color::GreenLight : Color::GrayLight));

    // 4. Fullscreen Visualizer Style
    std::string vis_str;
    switch (fullscreen_visualizer_style_) {
        case core::FullscreenVisualizerStyle::Stretched:
            vis_str = "< Stretched (Full Width) >";
            break;
        case core::FullscreenVisualizerStyle::LowFreqCentered:
            vis_str = "< Low Freq Centered & Mirrored >";
            break;
        case core::FullscreenVisualizerStyle::HighFreqCentered:
            vis_str = "< High Freq Centered & Mirrored >";
            break;
    }
    auto vis_style_elem = text(vis_str)
        | (selected_field_ == 4 && is_focused ? color(accent) | bold : color(fullscreen_visualizer_style_ != core::FullscreenVisualizerStyle::Stretched ? Color::CyanLight : Color::White));

    // 18. Save & Apply button
    auto btn_save = text(" [ " + sym.save + " Save & Apply Settings ] ")
        | (selected_field_ == 18 && is_focused ? bgcolor(accent) | color(Color::Black) | bold : bgcolor(Color::RGB(30, 30, 35)) | color(accent) | bold);

    // Header Title
    content.push_back(hbox({
        text(" " + sym.settings + "  PREFERENCES & CONFIGURATION ") | color(accent) | bold,
        filler(),
        text("[h/l, Space] Change  [j/k, Tab] Move  [Enter] Select/Save ") | color(Color::GrayDark)
    }));
    content.push_back(separator());

    // Section: Account
    content.push_back(text(" " + sym.account + " SOUNDCLOUD ACCOUNT & SESSIONS ") | color(Color::White) | bold);
    content.push_back(hbox({
        text("   Status:                 ") | color(Color::GrayLight),
        text(auth_status_text) | color(auth_status_color) | bold
    }));
    content.push_back(hbox({
        text("   Installed Browsers:     ") | color(Color::GrayLight),
        text(detected_str) | color(Color::GrayLight)
    }));
    login_url_row_ = 4 + static_cast<int>(content.size());
    content.push_back(hbox({
        text("   Login URL:              ") | color(Color::GrayLight),
        text("https://soundcloud.com/signin") | color(Color::CyanLight) | bold
    }));
    content.push_back(render_row(0, "Auto-Extract Cookies", btn_extract, "Checks Mozilla first, then Chromium"));
    content.push_back(render_row(1, "SoundCloud Web Client ID", btn_refresh_cid, "Scrapes fresh client_id if expired"));
    content.push_back(text(""));

    // Section: Graphics & UI Style
    content.push_back(text(" " + sym.graphics + "  TERMINAL COVER ART & ICONS ") | color(Color::White) | bold);
    content.push_back(render_row(2, "Cover Art Protocol", proto_elem, "(Terminal detects: " + detected_proto_str + ")"));
    content.push_back(render_row(3, "Icon & Symbols Style", icon_style_elem, "Replace emojis with Unicode or Default ASCII symbols"));
    content.push_back(render_row(4, "Fullscreen Visualizer", vis_style_elem, "Mirrored extends from center to edges, or stretched full width"));
    content.push_back(hbox({
        text("   Cover Art Option:       ") | color(Color::GrayDark),
        text("Choose 'Pixelated (Unicode Half-Block 24-bit)' to manually force pixelated cover even on Kitty/Ghostty/iTerm2.") | color(Color::GrayDark)
    }));
    content.push_back(text(""));

    // Section: Networking & Geo-Bypass
    content.push_back(text(" " + sym.network + " NETWORKING & GEO-UNBLOCK (DNS / DoH / PROXY) ") | color(Color::White) | bold);
    content.push_back(render_row(5, "DNS Resolution Mode", dns_mode_elem, "Bypasses ISP DNS blocking / regional tampering"));
    if (dns_mode_idx_ == 1) {
        content.push_back(render_row(6, "DoH Provider", doh_prov_elem, "Queries DNS via secure HTTPS connection"));
    }
    if ((dns_mode_idx_ == 1 && doh_provider_idx_ == 3) || dns_mode_idx_ == 2) {
        content.push_back(render_row(7, custom_net_label, custom_net_elem));
    }
    content.push_back(render_row(8, "HTTP/SOCKS5 Proxy URL", proxy_elem, "e.g. socks5h://127.0.0.1:1080 for full traffic tunneling"));
    content.push_back(text(""));

    // Section: Audio & Cache
    content.push_back(text(" " + sym.save + " TRACK CACHING & OFFLINE PLAYBACK ") | color(Color::White) | bold);
    content.push_back(hbox({
        text("   Current Usage:          ") | color(Color::GrayLight),
        text(stats_str) | color(accent) | bold
    }));
    content.push_back(render_row(9, "Max Cache Size", cache_size_elem, "Cached tracks play instantly without buffering"));
    content.push_back(render_row(10, "Lock Liked in Cache", liked_lock_elem, "Liked tracks are pinned and never evicted"));
    content.push_back(render_row(11, "Exclude Liked from Limit", dont_count_elem, "Liked tracks have own space and don't reduce cache quota"));
    content.push_back(render_row(12, "Download All Likes", btn_download_likes, "Preloads audio and full metadata for all liked tracks"));
    content.push_back(render_row(13, "Cache Maintenance", btn_clean_cache));
    content.push_back(text(""));

    // Section: Audio Output & Integrations
    content.push_back(text(" " + sym.audio + " AUDIO ENGINE & INTEGRATIONS ") | color(Color::White) | bold);
    content.push_back(render_row(14, "Discord Rich Presence", discord_rpc_elem, "Shows currently playing SoundCloud track in Discord status"));
    content.push_back(render_row(15, "TUI Mouse Controls", mouse_ctrl_elem, "Allows controlling playback, seeking, scrolling, and tab selection with mouse"));
    content.push_back(render_row(16, "DRM Playback Engine", drm_elem, "Uses background CastLabs Electron daemon for Widevine streams"));
    content.push_back(render_row(17, "Download DRM in Likes", drm_download_elem, "Downloads DRM-protected tracks (non-GO) during likes download"));
    content.push_back(hbox({
        text("   Cache Directory:        ") | color(Color::GrayDark),
        text(config_.cache_dir) | color(Color::GrayDark)
    }));
    content.push_back(text(""));

    // Section: Save & Apply
    content.push_back(render_row(18, "Apply Changes", btn_save));
    if (!feedback_message_.empty()) {
        content.push_back(text(""));
        content.push_back(hbox({
            text("   " + sym.finished + " ") | color(feedback_is_error_ ? Color::RedLight : Color::GreenLight) | bold,
            text(feedback_message_) | color(feedback_is_error_ ? Color::RedLight : Color::GreenLight) | bold
        }));
    }

    return vbox(std::move(content))
        | borderRounded
        | color(border_col)
        | yflex;
}

} // namespace sctui::ui::views
