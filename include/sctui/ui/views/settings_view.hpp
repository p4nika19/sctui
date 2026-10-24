#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <functional>
#include <string>
#include <memory>
#include <unordered_map>
#include "sctui/core/config.hpp"
#include "sctui/core/track_cache.hpp"
#include "sctui/models/state.hpp"

namespace sctui::ui::views {

class SettingsView {
public:
    using SaveCallback = std::function<void(const core::Config& new_config)>;
    using ExtractCookiesCallback = std::function<void()>;
    using RefreshClientIdCallback = std::function<void()>;
    using ClearCacheCallback = std::function<void(bool keep_liked)>;
    using DownloadAllLikesCallback = std::function<void()>;

    SettingsView(
        models::AppState& state,
        core::Config& config,
        std::shared_ptr<core::TrackCacheManager> cache_mgr,
        SaveCallback on_save,
        ExtractCookiesCallback on_extract_cookies,
        RefreshClientIdCallback on_refresh_client_id,
        ClearCacheCallback on_clear_cache,
        DownloadAllLikesCallback on_download_all_likes = nullptr
    );

    ftxui::Component get_component();
    ftxui::Element render(bool is_focused);

    void reload_from_config();
    void select_field(int field_idx);
    bool handle_mouse_click(int y, int x);
    bool is_editing_text() const;

private:
    models::AppState& state_;
    core::Config& config_;
    std::shared_ptr<core::TrackCacheManager> cache_mgr_;
    SaveCallback on_save_;
    ExtractCookiesCallback on_extract_cookies_;
    RefreshClientIdCallback on_refresh_client_id_;
    ClearCacheCallback on_clear_cache_;
    DownloadAllLikesCallback on_download_all_likes_;

    ftxui::Component component_;

    // Editable draft configuration state
    int selected_field_{0};
    int image_protocol_idx_{0};
    int icon_style_idx_{0};
    int dns_mode_idx_{0};
    int doh_provider_idx_{0};
    std::string doh_url_input_;
    std::string custom_dns_input_;
    std::string proxy_url_input_;
    int doh_cursor_pos_{0};
    int dns_cursor_pos_{0};
    int proxy_cursor_pos_{0};
    size_t max_cache_size_mb_{1024};
    bool lock_liked_tracks_in_cache_{true};
    bool dont_count_liked_in_general_cache_{false};
    int volume_input_{75};
    bool discord_rpc_enabled_{true};
    bool mouse_control_{false};
    bool drm_engine_enabled_{true};
    bool download_drm_tracks_in_likes_{true};

    ftxui::Component doh_input_comp_;
    ftxui::Component dns_input_comp_;
    ftxui::Component proxy_input_comp_;

    std::string feedback_message_;
    bool feedback_is_error_{false};

    std::unordered_map<int, int> row_to_field_;
    int login_url_row_{-1};
};

} // namespace sctui::ui::views
