#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <functional>
#include <string>
#include <memory>
#include <vector>
#include <optional>
#include "sctui/core/config.hpp"
#include "sctui/core/cover_art.hpp"
#include "sctui/models/state.hpp"

namespace sctui::ui::views {

enum class OnboardingStep {
    Authentication = 0,
    Appearance = 1,
    EngineConnectivity = 2
};

class OnboardingView {
public:
    using FinishCallback = std::function<void()>;
    using ExtractCookiesCallback = std::function<void()>;

    OnboardingView(
        models::AppState& state,
        core::Config& config,
        FinishCallback on_finish,
        ExtractCookiesCallback on_extract_cookies = nullptr
    );

    ftxui::Component get_component();
    ftxui::Element render();

    [[nodiscard]] OnboardingStep get_current_step() const noexcept { return current_step_; }
    void set_step(OnboardingStep step);

    bool handle_mouse_click(int y, int x);

    // Testing and programmatic inspection accessors
    [[nodiscard]] int get_selected_field() const noexcept { return selected_field_; }
    void select_field(int idx) { selected_field_ = idx; }
    [[nodiscard]] bool has_preview_image() const noexcept { return preview_image_.has_value(); }

private:
    models::AppState& state_;
    core::Config& config_;
    FinishCallback on_finish_;
    ExtractCookiesCallback on_extract_cookies_;

    OnboardingStep current_step_{OnboardingStep::Authentication};
    int selected_field_{0};

    // Step 1: Authentication state
    std::string auth_feedback_msg_;
    bool auth_feedback_is_error_{false};

    // Step 2: Appearance & Protocol state
    int icon_style_idx_{0};
    int image_protocol_idx_{0};
    std::optional<models::CoverArtImage> preview_image_;

    // Step 3: Engine & Connectivity state
    int dns_mode_idx_{0};
    int doh_provider_idx_{0};
    std::string doh_url_input_;
    std::string custom_dns_input_;
    std::string proxy_url_input_;
    int doh_cursor_pos_{0};
    int dns_cursor_pos_{0};
    int proxy_cursor_pos_{0};
    bool drm_engine_enabled_{true};
    bool download_drm_tracks_in_likes_{true};
    bool discord_rpc_enabled_{true};
    bool fullscreen_visualizer_mirrored_{false};
    bool mouse_control_{false};

    ftxui::Component doh_input_comp_;
    ftxui::Component dns_input_comp_;
    ftxui::Component proxy_input_comp_;
    ftxui::Component component_;

    // Layout row mappings for mouse clicks
    struct ClickableZone {
        int y_start;
        int y_end;
        int x_start;
        int x_end;
        int field_idx;
        std::function<void()> on_click;
    };
    std::vector<ClickableZone> clickable_zones_;

    void load_static_fixture();
    void sync_to_config();
    bool is_step3_field_visible(int idx) const;
    bool is_step3_editing_text() const;

    ftxui::Element render_persistent_header();
    ftxui::Element render_step1_auth();
    ftxui::Element render_step2_appearance();
    ftxui::Element render_step3_engine();
};

} // namespace sctui::ui::views
