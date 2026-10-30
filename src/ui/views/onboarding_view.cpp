#include "sctui/ui/views/onboarding_view.hpp"
#include <ftxui/component/event.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <unistd.h>
#include "sctui/core/cookie_extractor.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include "sctui/audio/drm_daemon.hpp"

namespace sctui::ui::views {

using namespace ftxui;

namespace {
constexpr int kTotalStep3Fields = 11;

std::vector<uint8_t> read_binary_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}
} // namespace

OnboardingView::OnboardingView(
    models::AppState& state,
    core::Config& config,
    FinishCallback on_finish,
    ExtractCookiesCallback on_extract_cookies,
    InstallElectronCallback on_install_electron
) : state_(state),
    config_(config),
    on_finish_(std::move(on_finish)),
    on_extract_cookies_(std::move(on_extract_cookies)),
    on_install_electron_(std::move(on_install_electron)) {

    // Initialize draft values from config
    icon_style_idx_ = static_cast<int>(config_.icon_style);
    image_protocol_idx_ = static_cast<int>(config_.image_protocol);
    dns_mode_idx_ = static_cast<int>(config_.dns_mode);
    doh_provider_idx_ = static_cast<int>(config_.doh_provider);
    doh_url_input_ = config_.doh_url;
    custom_dns_input_ = config_.custom_dns_server;
    proxy_url_input_ = config_.proxy_url;
    doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
    dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
    proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
    drm_engine_enabled_ = config_.drm_engine_enabled;
    download_drm_tracks_in_likes_ = config_.download_drm_tracks_in_likes;
    discord_rpc_enabled_ = config_.discord_rpc_enabled;
    fullscreen_visualizer_style_ = config_.fullscreen_visualizer_style;
    mouse_control_ = config_.mouse_control;

    // Load static UI fixture cover art
    load_static_fixture();

    doh_input_comp_ = Input(&doh_url_input_, "https://cloudflare-dns.com/dns-query");
    dns_input_comp_ = Input(&custom_dns_input_, "1.1.1.1");
    proxy_input_comp_ = Input(&proxy_url_input_, "socks5://127.0.0.1:1080 or http://127.0.0.1:7890");

    auto inner_container = Container::Vertical({
        doh_input_comp_,
        dns_input_comp_,
        proxy_input_comp_
    });

    component_ = CatchEvent(inner_container, [this](Event event) {
        if (!state_.is_onboarding) {
            return false;
        }

        // --- Step 1: Authentication ---
        if (current_step_ == OnboardingStep::Authentication) {
            if (event == Event::ArrowDown || event == Event::Character('j') || event == Event::Tab) {
                selected_field_ = (selected_field_ + 1) % 2;
                return true;
            }
            if (event == Event::ArrowUp || event == Event::Character('k') || event == Event::TabReverse) {
                selected_field_ = (selected_field_ + 1) % 2;
                return true;
            }
            if (event == Event::Return || event == Event::Character(' ')) {
                if (selected_field_ == 0) {
                    // Action: Auto-extract Cookies
                    auto res = core::CookieExtractor::extract_soundcloud_token();
                    if (res.success && res.cookie) {
                        config_.auth_token = res.cookie->token;
                        config_.extracted_browser = res.cookie->browser_name;
                        auth_feedback_msg_ = "Successfully extracted session token from " + res.cookie->browser_name + "!";
                        auth_feedback_is_error_ = false;
                        if (on_extract_cookies_) {
                            on_extract_cookies_();
                        }
                    } else {
                        auth_feedback_msg_ = "No active SoundCloud cookies found in detected browsers.";
                        auth_feedback_is_error_ = true;
                    }
                    return true;
                }
                if (selected_field_ == 1) {
                    // Advance to Step 2
                    set_step(OnboardingStep::Appearance);
                    return true;
                }
            }
            return false;
        }

        // --- Step 2: Appearance & Terminal Protocols ---
        if (current_step_ == OnboardingStep::Appearance) {
            if (event == Event::ArrowDown || event == Event::Character('j') || event == Event::Tab) {
                if (selected_field_ < 2) {
                    selected_field_++;
                } else if (selected_field_ == 2 || selected_field_ == 3) {
                    selected_field_ = 0; // Wrap to top
                }
                return true;
            }
            if (event == Event::ArrowUp || event == Event::Character('k') || event == Event::TabReverse) {
                if (selected_field_ > 1) {
                    selected_field_ = 1;
                } else if (selected_field_ == 1) {
                    selected_field_ = 0;
                } else {
                    selected_field_ = 3; // Wrap to bottom Next button
                }
                return true;
            }

            // Left / Right adjustments
            if (event == Event::ArrowLeft || event == Event::Character('h')) {
                if (selected_field_ == 0) { // Icon Style
                    icon_style_idx_ = (icon_style_idx_ + 2) % 3;
                    state_.icon_style = static_cast<core::IconStyle>(icon_style_idx_);
                    return true;
                }
                if (selected_field_ == 1) { // Cover Art Protocol
                    image_protocol_idx_ = (image_protocol_idx_ + 4) % 5;
                    core::CoverArtProcessor::clear_terminal_image(888);
                    preview_drawn_in_terminal_ = false;
                    return true;
                }
                if (selected_field_ == 3) { // Switch between Previous and Next
                    selected_field_ = 2;
                    return true;
                }
            }
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                if (selected_field_ == 0) { // Icon Style
                    icon_style_idx_ = (icon_style_idx_ + 1) % 3;
                    state_.icon_style = static_cast<core::IconStyle>(icon_style_idx_);
                    return true;
                }
                if (selected_field_ == 1) { // Cover Art Protocol
                    image_protocol_idx_ = (image_protocol_idx_ + 1) % 5;
                    core::CoverArtProcessor::clear_terminal_image(888);
                    preview_drawn_in_terminal_ = false;
                    return true;
                }
                if (selected_field_ == 2) { // Switch between Previous and Next
                    selected_field_ = 3;
                    return true;
                }
            }

            if (event == Event::Return || event == Event::Character(' ')) {
                if (selected_field_ == 0) {
                    icon_style_idx_ = (icon_style_idx_ + 1) % 3;
                    state_.icon_style = static_cast<core::IconStyle>(icon_style_idx_);
                    return true;
                }
                if (selected_field_ == 1) {
                    image_protocol_idx_ = (image_protocol_idx_ + 1) % 5;
                    core::CoverArtProcessor::clear_terminal_image(888);
                    preview_drawn_in_terminal_ = false;
                    return true;
                }
                if (selected_field_ == 2) {
                    // Previous -> Step 1
                    set_step(OnboardingStep::Authentication);
                    return true;
                }
                if (selected_field_ == 3) {
                    // Next -> Step 3
                    set_step(OnboardingStep::EngineConnectivity);
                    return true;
                }
            }
            return false;
        }

        // --- Step 3: Engine & Connectivity ---
        if (current_step_ == OnboardingStep::EngineConnectivity) {
            if (is_step3_editing_text()) {
                if (event == Event::Tab || event == Event::ArrowDown || event == Event::Return) {
                    do {
                        selected_field_ = (selected_field_ + 1) % kTotalStep3Fields;
                    } while (!is_step3_field_visible(selected_field_));
                    if (selected_field_ == 2) {
                        if (dns_mode_idx_ == 1) doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
                        else if (dns_mode_idx_ == 2) dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
                    }
                    if (selected_field_ == 3) proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
                    return true;
                }
                if (event == Event::TabReverse || event == Event::ArrowUp) {
                    do {
                        selected_field_ = (selected_field_ + kTotalStep3Fields - 1) % kTotalStep3Fields;
                    } while (!is_step3_field_visible(selected_field_));
                    if (selected_field_ == 2) {
                        if (dns_mode_idx_ == 1) doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
                        else if (dns_mode_idx_ == 2) dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
                    }
                    if (selected_field_ == 3) proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
                    return true;
                }

                std::string* active_text = nullptr;
                int* active_cursor = nullptr;
                if (selected_field_ == 2) {
                    if (dns_mode_idx_ == 1 && doh_provider_idx_ == 3) {
                        active_text = &doh_url_input_;
                        active_cursor = &doh_cursor_pos_;
                    } else if (dns_mode_idx_ == 2) {
                        active_text = &custom_dns_input_;
                        active_cursor = &dns_cursor_pos_;
                    }
                } else if (selected_field_ == 3) {
                    active_text = &proxy_url_input_;
                    active_cursor = &proxy_cursor_pos_;
                }

                if (active_text && active_cursor) {
                    if (event == Event::ArrowLeft && *active_cursor > 0) {
                        (*active_cursor)--;
                        return true;
                    }
                    if (event == Event::ArrowRight && *active_cursor < static_cast<int>(active_text->size())) {
                        (*active_cursor)++;
                        return true;
                    }
                    if (event == Event::Home) {
                        *active_cursor = 0;
                        return true;
                    }
                    if (event == Event::End) {
                        *active_cursor = static_cast<int>(active_text->size());
                        return true;
                    }
                    if (event == Event::Backspace && *active_cursor > 0) {
                        active_text->erase(*active_cursor - 1, 1);
                        (*active_cursor)--;
                        return true;
                    }
                    if (event == Event::Delete && *active_cursor < static_cast<int>(active_text->size())) {
                        active_text->erase(*active_cursor, 1);
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
            }

            // Normal Step 3 navigation
            if (event == Event::ArrowDown || event == Event::Character('j') || event == Event::Tab) {
                do {
                    selected_field_ = (selected_field_ + 1) % kTotalStep3Fields;
                } while (!is_step3_field_visible(selected_field_));
                if (selected_field_ == 2) {
                    if (dns_mode_idx_ == 1) doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
                    else if (dns_mode_idx_ == 2) dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
                }
                if (selected_field_ == 3) proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
                return true;
            }
            if (event == Event::ArrowUp || event == Event::Character('k') || event == Event::TabReverse) {
                do {
                    selected_field_ = (selected_field_ + kTotalStep3Fields - 1) % kTotalStep3Fields;
                } while (!is_step3_field_visible(selected_field_));
                if (selected_field_ == 2) {
                    if (dns_mode_idx_ == 1) doh_cursor_pos_ = static_cast<int>(doh_url_input_.size());
                    else if (dns_mode_idx_ == 2) dns_cursor_pos_ = static_cast<int>(custom_dns_input_.size());
                }
                if (selected_field_ == 3) proxy_cursor_pos_ = static_cast<int>(proxy_url_input_.size());
                return true;
            }

            // Left / Right adjustments
            if (event == Event::ArrowLeft || event == Event::Character('h')) {
                if (selected_field_ == 0) { // DNS Resolution Mode
                    dns_mode_idx_ = (dns_mode_idx_ + 2) % 3;
                    return true;
                }
                if (selected_field_ == 1) { // DoH Provider
                    doh_provider_idx_ = (doh_provider_idx_ + 3) % 4;
                    return true;
                }
                if (selected_field_ == 4) { // DRM Playback Engine
                    drm_engine_enabled_ = !drm_engine_enabled_;
                    return true;
                }
                if (selected_field_ == 5) { // Download DRM in Likes
                    download_drm_tracks_in_likes_ = !download_drm_tracks_in_likes_;
                    return true;
                }
                if (selected_field_ == 6) { // Discord RPC
                    discord_rpc_enabled_ = !discord_rpc_enabled_;
                    return true;
                }
                if (selected_field_ == 7) { // Fullscreen Visualizer
                    fullscreen_visualizer_style_ = static_cast<core::FullscreenVisualizerStyle>((static_cast<int>(fullscreen_visualizer_style_) + 2) % 3);
                    return true;
                }
                if (selected_field_ == 8) { // Mouse Controls
                    mouse_control_ = !mouse_control_;
                    return true;
                }
                if (selected_field_ == 10) { // Switch from Confirm to Previous
                    selected_field_ = 9;
                    return true;
                }
            }
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                if (selected_field_ == 0) { // DNS Resolution Mode
                    dns_mode_idx_ = (dns_mode_idx_ + 1) % 3;
                    return true;
                }
                if (selected_field_ == 1) { // DoH Provider
                    doh_provider_idx_ = (doh_provider_idx_ + 1) % 4;
                    return true;
                }
                if (selected_field_ == 4) { // DRM Playback Engine
                    drm_engine_enabled_ = !drm_engine_enabled_;
                    return true;
                }
                if (selected_field_ == 5) { // Download DRM in Likes
                    download_drm_tracks_in_likes_ = !download_drm_tracks_in_likes_;
                    return true;
                }
                if (selected_field_ == 6) { // Discord RPC
                    discord_rpc_enabled_ = !discord_rpc_enabled_;
                    return true;
                }
                if (selected_field_ == 7) { // Fullscreen Visualizer
                    fullscreen_visualizer_style_ = static_cast<core::FullscreenVisualizerStyle>((static_cast<int>(fullscreen_visualizer_style_) + 2) % 3);
                    return true;
                }
                if (selected_field_ == 8) { // Mouse Controls
                    mouse_control_ = !mouse_control_;
                    return true;
                }
                if (selected_field_ == 9) { // Switch from Previous to Confirm
                    selected_field_ = 10;
                    return true;
                }
            }

            if (event == Event::Return || event == Event::Character(' ')) {
                if (selected_field_ == 0) {
                    dns_mode_idx_ = (dns_mode_idx_ + 1) % 3;
                    return true;
                }
                if (selected_field_ == 1) {
                    doh_provider_idx_ = (doh_provider_idx_ + 1) % 4;
                    return true;
                }
                if (selected_field_ == 4) {
                    drm_engine_enabled_ = !drm_engine_enabled_;
                    return true;
                }
                if (selected_field_ == 5) {
                    download_drm_tracks_in_likes_ = !download_drm_tracks_in_likes_;
                    return true;
                }
                if (selected_field_ == 6) {
                    discord_rpc_enabled_ = !discord_rpc_enabled_;
                    return true;
                }
                if (selected_field_ == 7) {
                    fullscreen_visualizer_style_ = static_cast<core::FullscreenVisualizerStyle>((static_cast<int>(fullscreen_visualizer_style_) + 1) % 3);
                    return true;
                }
                if (selected_field_ == 8) {
                    mouse_control_ = !mouse_control_;
                    return true;
                }
                if (selected_field_ == 9) {
                    // Previous -> Step 2
                    set_step(OnboardingStep::Appearance);
                    return true;
                }
                if (selected_field_ == 10) {
                    // Confirm & Start -> Apply and finish!
                    sync_to_config();
                    if (drm_engine_enabled_ && !audio::DrmSatelliteDaemon::is_castlabs_electron_installed() && on_install_electron_) {
                        set_step(OnboardingStep::InstallingElectron);
                        start_electron_installation();
                        return true;
                    }
                    config_.onboarding_completed = true;
                    (void)config_.save();
                    core::CoverArtProcessor::clear_terminal_image(888);
                    core::CoverArtProcessor::clear_terminal_image(0);
                    preview_drawn_in_terminal_ = false;
                    if (on_finish_) {
                        on_finish_();
                    }
                    return true;
                }
            }
            return false;
        }

        if (current_step_ == OnboardingStep::InstallingElectron) {
            if (install_electron_failed_) {
                if (event == Event::ArrowLeft || event == Event::Character('h')) {
                    selected_field_ = 0;
                    return true;
                }
                if (event == Event::ArrowRight || event == Event::Character('l') || event == Event::Tab) {
                    selected_field_ = (selected_field_ + 1) % 2;
                    return true;
                }
                if (event == Event::Return || event == Event::Character(' ')) {
                    if (selected_field_ == 0) {
                        start_electron_installation();
                        return true;
                    } else if (selected_field_ == 1) {
                        drm_engine_enabled_ = false;
                        config_.drm_engine_enabled = false;
                        state_.drm_engine_enabled = false;
                        config_.onboarding_completed = true;
                        (void)config_.save();
                        core::CoverArtProcessor::clear_terminal_image(888);
                        core::CoverArtProcessor::clear_terminal_image(0);
                        preview_drawn_in_terminal_ = false;
                        if (on_finish_) {
                            on_finish_();
                        }
                        return true;
                    }
                }
            }
            return true;
        }

        return false;
    });
}

OnboardingView::~OnboardingView() {
    core::CoverArtProcessor::clear_terminal_image(888);
}

void OnboardingView::load_static_fixture() {
    std::vector<std::filesystem::path> candidate_paths = {
        "assets/preview_cover.jpg",
        "../assets/preview_cover.jpg",
        "/usr/local/share/sctui/assets/preview_cover.jpg",
        "/usr/share/sctui/assets/preview_cover.jpg"
    };
    std::error_code ec;
    const auto executable_path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        const auto executable_dir = executable_path.parent_path();
        candidate_paths.push_back(executable_dir / "../share/sctui/assets/preview_cover.jpg");
        candidate_paths.push_back(executable_dir / "../../../../assets/preview_cover.jpg");
    }

    std::vector<uint8_t> raw_bytes;
    for (const auto& path : candidate_paths) {
        raw_bytes = read_binary_file(path.string());
        if (!raw_bytes.empty()) {
            break;
        }
    }

    if (!raw_bytes.empty()) {
        preview_image_ = core::CoverArtProcessor::decode_and_resize(
            raw_bytes.data(),
            raw_bytes.size(),
            20,
            20
        );
    }
}

void OnboardingView::sync_to_config() {
    config_.icon_style = static_cast<core::IconStyle>(icon_style_idx_);
    config_.image_protocol = static_cast<core::ImageProtocol>(image_protocol_idx_);
    config_.dns_mode = static_cast<core::DnsMode>(dns_mode_idx_);
    config_.doh_provider = static_cast<core::DohProvider>(doh_provider_idx_);
    config_.doh_url = doh_url_input_;
    config_.custom_dns_server = custom_dns_input_;
    config_.proxy_url = proxy_url_input_;
    config_.drm_engine_enabled = drm_engine_enabled_;
    config_.download_drm_tracks_in_likes = download_drm_tracks_in_likes_;
    config_.discord_rpc_enabled = discord_rpc_enabled_;
    config_.fullscreen_visualizer_style = fullscreen_visualizer_style_;
    config_.fullscreen_visualizer_mirrored = (fullscreen_visualizer_style_ != core::FullscreenVisualizerStyle::Stretched);
    config_.mouse_control = mouse_control_;

    state_.icon_style = config_.icon_style;
    state_.drm_engine_enabled = drm_engine_enabled_;
    state_.download_drm_tracks_in_likes = download_drm_tracks_in_likes_;
    state_.mouse_control = mouse_control_;
}

void OnboardingView::start_electron_installation() {
    install_electron_failed_ = false;
    install_electron_err_.clear();
    state_.is_downloading_electron = true;

    if (on_install_electron_) {
        on_install_electron_([this](bool ok, const std::string& err) {
            state_.is_downloading_electron = false;
            if (ok) {
                config_.onboarding_completed = true;
                (void)config_.save();
                core::CoverArtProcessor::clear_terminal_image(888);
                core::CoverArtProcessor::clear_terminal_image(0);
                preview_drawn_in_terminal_ = false;
                if (on_finish_) {
                    on_finish_();
                }
            } else {
                install_electron_failed_ = true;
                install_electron_err_ = err;
                selected_field_ = 0;
            }
        });
    }
}

bool OnboardingView::is_step3_field_visible(int idx) const {
    if (idx == 1) { // DoH Provider
        return dns_mode_idx_ == 1;
    }
    if (idx == 2) { // Custom endpoint
        return (dns_mode_idx_ == 1 && doh_provider_idx_ == 3) || (dns_mode_idx_ == 2);
    }
    return true;
}

bool OnboardingView::is_step3_editing_text() const {
    if (selected_field_ == 2) {
        return (dns_mode_idx_ == 1 && doh_provider_idx_ == 3) || (dns_mode_idx_ == 2);
    }
    if (selected_field_ == 3) {
        return true;
    }
    return false;
}

void OnboardingView::set_step(OnboardingStep step) {
    if (step != OnboardingStep::Appearance || current_step_ == OnboardingStep::Appearance) {
        core::CoverArtProcessor::clear_terminal_image(888);
        preview_drawn_in_terminal_ = false;
    }
    current_step_ = step;
    selected_field_ = 0;
}

bool OnboardingView::handle_mouse_click(int y, int x) {
    for (const auto& zone : clickable_zones_) {
        if (y >= zone.y_start && y <= zone.y_end && x >= zone.x_start && x <= zone.x_end) {
            selected_field_ = zone.field_idx;
            if (zone.on_click) {
                zone.on_click();
            }
            return true;
        }
    }
    return false;
}

Component OnboardingView::get_component() {
    return component_;
}

Element OnboardingView::render_persistent_header() {
    auto accent = state_.get_accent_color();
    Elements header_elems;

    // Persistent ASCII / Banner "SCTUI"
    const char* banner_lines[] = {
        "  ____   ____ _____ _   _ ___ ",
        " / ___| / ___|_   _| | | |_ _|",
        " \\___ \\| |     | | | | | || | ",
        "  ___) | |___  | | | |_| || | ",
        " |____/ \\____| |_|  \\___/|___|"
    };

    Elements banner_elements;
    for (const char* line : banner_lines) {
        banner_elements.push_back(text(line) | color(accent) | bold | center);
    }
    header_elems.push_back(vbox(std::move(banner_elements)));

    header_elems.push_back(text("SOUNDCLOUD TERMINAL USER INTERFACE") | color(Color::GrayLight) | bold | center);
    header_elems.push_back(text("First-Run Setup & Configuration Wizard") | color(Color::GrayDark) | center);
    header_elems.push_back(text(""));

    // Multi-step persistent step indicator bar
    Element step1_badge, step2_badge, step3_badge, step4_badge;
    if (current_step_ == OnboardingStep::Authentication) {
        step1_badge = text(" 1. AUTHENTICATION ") | bgcolor(accent) | color(Color::Black) | bold;
        step2_badge = text(" 2. APPEARANCE ") | color(Color::GrayLight);
        step3_badge = text(" 3. ENGINE & CONNECTIVITY ") | color(Color::GrayLight);
    } else if (current_step_ == OnboardingStep::Appearance) {
        step1_badge = text(" ✓ 1. AUTHENTICATION ") | color(Color::GreenLight) | bold;
        step2_badge = text(" 2. APPEARANCE ") | bgcolor(accent) | color(Color::Black) | bold;
        step3_badge = text(" 3. ENGINE & CONNECTIVITY ") | color(Color::GrayLight);
    } else if (current_step_ == OnboardingStep::EngineConnectivity) {
        step1_badge = text(" ✓ 1. AUTHENTICATION ") | color(Color::GreenLight) | bold;
        step2_badge = text(" ✓ 2. APPEARANCE ") | color(Color::GreenLight) | bold;
        step3_badge = text(" 3. ENGINE & CONNECTIVITY ") | bgcolor(accent) | color(Color::Black) | bold;
    } else {
        step1_badge = text(" ✓ 1. AUTHENTICATION ") | color(Color::GreenLight) | bold;
        step2_badge = text(" ✓ 2. APPEARANCE ") | color(Color::GreenLight) | bold;
        step3_badge = text(" ✓ 3. ENGINE & CONNECTIVITY ") | color(Color::GreenLight) | bold;
        step4_badge = text(" 4. INSTALLING ELECTRON ") | bgcolor(accent) | color(Color::Black) | bold;
    }

    Elements step_bar = {
        filler(),
        step1_badge,
        text(" ─── ") | color(Color::GrayDark),
        step2_badge,
        text(" ─── ") | color(Color::GrayDark),
        step3_badge
    };
    if (current_step_ == OnboardingStep::InstallingElectron) {
        step_bar.push_back(text(" ─── ") | color(Color::GrayDark));
        step_bar.push_back(step4_badge);
    }
    step_bar.push_back(filler());

    header_elems.push_back(hbox(std::move(step_bar)));
    header_elems.push_back(text(""));
    header_elems.push_back(separator() | color(Color::RGB(50, 50, 60)));

    return vbox(std::move(header_elems));
}

Element OnboardingView::render_step1_auth() {
    if (preview_drawn_in_terminal_) {
        core::CoverArtProcessor::clear_terminal_image(888);
        preview_drawn_in_terminal_ = false;
    }
    auto accent = state_.get_accent_color();
    clickable_zones_.clear();

    Elements body;
    body.push_back(text(""));
    body.push_back(text("Connect your SoundCloud account to sync personal likes, playlists, and stream.") | color(Color::White) | bold | center);
    body.push_back(text("SCTUI can automatically extract your session cookies from installed web browsers.") | color(Color::GrayLight) | center);
    body.push_back(text(""));

    // 2. Action: Auto-extract Cookies button
    bool is_logged_in = !config_.auth_token.empty();
    std::string extract_label = is_logged_in
        ? " [ ✓ Logged In via " + (!config_.extracted_browser.empty() ? config_.extracted_browser : "Browser") + " ] "
        : " [ 🔑 Auto-Extract Browser Cookies ] ";

    auto btn_extract = text(extract_label)
        | (selected_field_ == 0
            ? bgcolor(accent) | color(Color::Black) | bold
            : (is_logged_in ? bgcolor(Color::RGB(20, 50, 30)) | color(Color::GreenLight) | bold : bgcolor(Color::RGB(35, 35, 45)) | color(Color::White) | bold));

    body.push_back(hbox({ filler(), btn_extract, filler() }));

    // Status or feedback below button
    if (!auth_feedback_msg_.empty()) {
        body.push_back(text("  " + auth_feedback_msg_)
            | color(auth_feedback_is_error_ ? Color::YellowLight : Color::GreenLight) | center);
    } else {
        auto detected = core::CookieExtractor::detect_installed_browsers();
        std::string det_str = detected.empty() ? "No supported browsers found" : "";
        for (size_t i = 0; i < detected.size(); ++i) {
            if (i > 0) det_str += ", ";
            det_str += detected[i];
        }
        body.push_back(text("Detected browsers: " + det_str) | color(Color::GrayDark) | center);
    }
    body.push_back(text(""));

    // 3. Divider: A horizontal rule slightly wider than the auto-extract button (~52 cols)
    body.push_back(text("────────────────── or manually configure ──────────────────") | color(Color::GrayDark) | center);
    body.push_back(text(""));

    // 4. Fallback text: Detailed manual cookie extraction instructions
    auto manual_instructions = vbox({
        text(" Manual Session Setup: ") | color(accent) | bold,
        text("  1. Open https://soundcloud.com in any browser and log in to your account.") | color(Color::GrayLight),
        text("  2. Open Developer Tools (F12 or Ctrl+Shift+I) -> Application / Storage tab.") | color(Color::GrayLight),
        text("  3. Under Cookies, find and copy the 'oauth_token' cookie string.") | color(Color::GrayLight),
        text("  4. You can also paste it into ~/.config/sctui/config.json or in Settings later.") | color(Color::GrayLight),
        text("  * Note: You may continue without an account to browse public tracks and charts.") | color(Color::GrayDark)
    }) | borderRounded | color(Color::RGB(70, 70, 85)) | bgcolor(Color::RGB(16, 16, 22)) | size(WIDTH, LESS_THAN, 82);

    body.push_back(hbox({ filler(), manual_instructions, filler() }));
    body.push_back(text(""));

    // 5. Footer: [Confirm] button to advance to Step 2
    auto btn_confirm = text(" [ Confirm & Continue → ] ")
        | (selected_field_ == 1 ? bgcolor(accent) | color(Color::Black) | bold : bgcolor(Color::RGB(30, 30, 38)) | color(Color::White) | bold);

    body.push_back(hbox({ filler(), btn_confirm, filler() }));
    body.push_back(text(""));
    body.push_back(text("Use [j/k, Up/Down, Tab] to navigate  •  [Enter, Space] to select") | color(Color::GrayDark) | center);

    return vbox(std::move(body));
}

Element OnboardingView::render_step2_appearance() {
    auto accent = state_.get_accent_color();
    clickable_zones_.clear();

    Elements body;
    body.push_back(text(""));
    body.push_back(text("Customize terminal glyphs and high-resolution cover art rendering.") | color(Color::White) | bold | center);
    body.push_back(text(""));

    // 1. "Icon & Symbols Style" setting
    const char* icon_styles[] = {
        "Emoji (Color Emojis: ☁, 🔍, 👤, 💖, 📁)",
        "Unicode (Clean Symbols: ☁, ⌕, ☺, ♥, ≡)",
        "Default (Plain ASCII: [SC], [/], [@], <3>)"
    };
    std::string icon_display = "< " + std::string(icon_styles[std::clamp(icon_style_idx_, 0, 2)]) + " >";
    auto icon_elem = text(icon_display)
        | (selected_field_ == 0 ? color(accent) | bold : color(Color::White));

    std::string inline_examples;
    if (icon_style_idx_ == 0) {
        inline_examples = "☁ Cloud   🔍 Search   👤 Profile   💖 Like   📁 Playlist   ▶ Play";
    } else if (icon_style_idx_ == 1) {
        inline_examples = "☁ Cloud   ⌕ Search   ☺ Profile   ♥ Like   ≡ Playlist   ▶ Play";
    } else {
        inline_examples = "[SC] Cloud   [/] Search   [@] Profile   <3> Like   [=] Playlist   [>] Play";
    }

    auto style_card = vbox({
        hbox({
            text(" Icon & Symbols Style: ") | color(Color::White) | bold,
            icon_elem,
            filler()
        }),
        hbox({
            text("   Visual Examples:    ") | color(Color::GrayLight),
            text(inline_examples) | color(accent) | bold
        })
    }) | borderRounded | color(selected_field_ == 0 ? accent : Color::RGB(60, 60, 75)) | bgcolor(Color::RGB(16, 16, 22)) | size(WIDTH, LESS_THAN, 82);

    body.push_back(hbox({ filler(), style_card, filler() }));
    body.push_back(text(""));

    // 2. "Cover Art Protocol" selection with live preview comparison
    const char* protos[] = {
        "Auto (Auto-Detect Terminal Graphics)",
        "Kitty Graphics Protocol (High-Res 24-bit)",
        "iTerm2 Inline Images Protocol (High-Res)",
        "Pixelated (Unicode Half-Block 24-bit)",
        "Disabled (No artwork overlays)"
    };
    std::string proto_display = "< " + std::string(protos[std::clamp(image_protocol_idx_, 0, 4)]) + " >";
    auto proto_elem = text(proto_display)
        | (selected_field_ == 1 ? color(accent) | bold : color(Color::White));

    // Render fixture preview cover
    Element cover_preview_element;
    auto effective_proto = static_cast<core::ImageProtocol>(image_protocol_idx_);
    if (effective_proto == core::ImageProtocol::Auto) {
        effective_proto = core::CoverArtProcessor::detect_terminal_graphics_protocol();
    }

    if (image_protocol_idx_ == 4) { // Disabled mode
        core::CoverArtProcessor::clear_terminal_image(888);
        preview_drawn_in_terminal_ = false;
        cover_preview_element = vbox({
            filler(),
            text("[ Artwork ]") | color(Color::GrayDark) | center,
            text("[ Disabled ]") | color(Color::GrayDark) | center,
            filler()
        }) | size(WIDTH, EQUAL, 18) | size(HEIGHT, EQUAL, 8) | border | color(Color::GrayDark);
    } else if (image_protocol_idx_ == 3) { // Half-block pixelated
        if (preview_drawn_in_terminal_) {
            core::CoverArtProcessor::clear_terminal_image(888);
            preview_drawn_in_terminal_ = false;
        }
        if (preview_image_) {
            cover_preview_element = core::CoverArtProcessor::render_cover_element(
                *preview_image_,
                core::ImageProtocol::HalfBlock,
                888,
                18,
                8
            );
        } else {
            cover_preview_element = core::CoverArtProcessor::render_placeholder(18, 8, state_.accent_color);
        }
    } else if (preview_image_) {
        preview_drawn_in_terminal_ = true;
        cover_preview_element = core::CoverArtProcessor::render_cover_element(
            *preview_image_,
            effective_proto,
            888, // dedicated preview image id
            18,
            8
        );
    } else {
        cover_preview_element = core::CoverArtProcessor::render_placeholder(18, 8, state_.accent_color);
    }

    std::vector<std::string> proto_desc_lines;
    if (image_protocol_idx_ == 0) {
        proto_desc_lines = {
            "Auto-detects terminal capability",
            "(Kitty, Ghostty, WezTerm, iTerm2, or Unicode fallback)."
        };
    } else if (image_protocol_idx_ == 1) {
        proto_desc_lines = {
            "Sends 24-bit TrueColor PNG image payload",
            "directly to terminal GPU canvas."
        };
    } else if (image_protocol_idx_ == 2) {
        proto_desc_lines = {
            "OSC 1337 protocol for iTerm2, WezTerm,",
            "and compatible terminals."
        };
    } else if (image_protocol_idx_ == 3) {
        proto_desc_lines = {
            "Universal 2-pixel-per-cell ANSI 24-bit",
            "half-blocks. 100% compatible with all terminals."
        };
    } else {
        proto_desc_lines = {
            "No artwork will be rendered in player",
            "or fullscreen modes."
        };
    }

    Elements desc_elements;
    for (const auto& line : proto_desc_lines) {
        desc_elements.push_back(text(line) | color(Color::GrayLight));
    }

    auto proto_card = vbox({
        hbox({
            text(" Cover Art Protocol:   ") | color(Color::White) | bold,
            proto_elem,
            filler()
        }),
        text(""),
        hbox({
            cover_preview_element,
            text("  "),
            vbox({
                text("Live Protocol Preview Comparison:") | color(Color::White) | bold,
                text("Fixture: Sindrom Sobstvennika (shquus)") | color(Color::GrayDark),
                text(""),
                text("Rendering Mode: " + std::string(protos[std::clamp(image_protocol_idx_, 0, 4)])) | color(accent),
                vbox(std::move(desc_elements)),
                text(""),
                text("Supports standard window resizing and") | color(Color::GrayDark),
                text("dynamic accent color extraction.") | color(Color::GrayDark)
            }) | xflex
        })
    }) | borderRounded | color(selected_field_ == 1 ? accent : Color::RGB(60, 60, 75)) | bgcolor(Color::RGB(16, 16, 22)) | size(WIDTH, LESS_THAN, 82);

    body.push_back(hbox({ filler(), proto_card, filler() }));
    body.push_back(text(""));

    // 3. Navigation: [Previous] and [Next] buttons
    auto btn_prev = text(" [ ← Previous ] ")
        | (selected_field_ == 2 ? bgcolor(accent) | color(Color::Black) | bold : bgcolor(Color::RGB(30, 30, 38)) | color(Color::White) | bold);

    auto btn_next = text(" [ Next → ] ")
        | (selected_field_ == 3 ? bgcolor(accent) | color(Color::Black) | bold : bgcolor(Color::RGB(30, 30, 38)) | color(Color::White) | bold);

    body.push_back(hbox({
        filler(),
        btn_prev,
        text("   "),
        btn_next,
        filler()
    }));

    body.push_back(text(""));
    body.push_back(text("[h/l, Left/Right] Change Options  •  [j/k, Up/Down, Tab] Move  •  [Enter] Confirm") | color(Color::GrayDark) | center);

    return vbox(std::move(body));
}

Element OnboardingView::render_step3_engine() {
    core::CoverArtProcessor::clear_terminal_image(888);
    preview_drawn_in_terminal_ = false;
    auto accent = state_.get_accent_color();
    clickable_zones_.clear();

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
            return box | borderRounded | color(accent) | bgcolor(Color::RGB(18, 18, 24)) | size(WIDTH, GREATER_THAN, 38);
        } else {
            return box | borderRounded | color(Color::GrayDark) | size(WIDTH, GREATER_THAN, 38);
        }
    };

    Elements body;
    body.push_back(text(""));
    body.push_back(text("Configure networking, playback engines, and audio integrations.") | color(Color::White) | bold | center);
    body.push_back(text(""));

    // --- Card 1: Networking & Geo-unblock ---
    const char* dns_modes[] = { "System Default", "DNS-over-HTTPS (DoH)", "Custom DNS Server" };
    auto dns_mode_elem = text("< " + std::string(dns_modes[std::clamp(dns_mode_idx_, 0, 2)]) + " >")
        | (selected_field_ == 0 ? color(accent) | bold : color(Color::White));

    const char* doh_provs[] = { "Cloudflare (1.1.1.1)", "Google (8.8.8.8)", "Quad9 (9.9.9.9)", "Custom URL" };
    auto doh_prov_elem = text("< " + std::string(doh_provs[std::clamp(doh_provider_idx_, 0, 3)]) + " >")
        | (selected_field_ == 1 ? color(accent) | bold : color(Color::White));

    Element custom_net_elem;
    std::string custom_net_label = "Custom Server Endpoint";
    bool field_2_editing = (selected_field_ == 2);
    if (dns_mode_idx_ == 1 && doh_provider_idx_ == 3) {
        custom_net_label = "Custom DoH URL";
        custom_net_elem = render_input_box(doh_url_input_, "https://cloudflare-dns.com/dns-query", doh_cursor_pos_, field_2_editing);
    } else if (dns_mode_idx_ == 2) {
        custom_net_label = "Custom DNS Server IP";
        custom_net_elem = render_input_box(custom_dns_input_, "1.1.1.1", dns_cursor_pos_, field_2_editing);
    } else {
        custom_net_elem = text("(Not required for current DNS mode)") | color(Color::GrayDark);
    }

    bool field_3_editing = (selected_field_ == 3);
    auto proxy_elem = render_input_box(proxy_url_input_, "socks5://127.0.0.1:1080 or http://127.0.0.1:7890", proxy_cursor_pos_, field_3_editing);

    Elements net_card_rows;
    net_card_rows.push_back(hbox({
        text(" " + ThemeSymbols::get(state_.icon_style).network + " NETWORKING & GEO-UNBLOCK ") | color(accent) | bold,
        filler()
    }));
    net_card_rows.push_back(text(""));
    net_card_rows.push_back(hbox({
        text("  DNS Resolution:       ") | color(selected_field_ == 0 ? accent : Color::GrayLight),
        dns_mode_elem
    }));
    if (dns_mode_idx_ == 1) {
        net_card_rows.push_back(hbox({
            text("  DoH Provider:         ") | color(selected_field_ == 1 ? accent : Color::GrayLight),
            doh_prov_elem
        }));
    }
    if ((dns_mode_idx_ == 1 && doh_provider_idx_ == 3) || dns_mode_idx_ == 2) {
        net_card_rows.push_back(hbox({
            text("  " + custom_net_label + ": ") | color(selected_field_ == 2 ? accent : Color::GrayLight),
            custom_net_elem
        }));
    }
    net_card_rows.push_back(hbox({
        text("  HTTP/SOCKS5 Proxy:    ") | color(selected_field_ == 3 ? accent : Color::GrayLight),
        proxy_elem
    }));

    auto net_card = vbox(std::move(net_card_rows))
        | borderRounded
        | color((selected_field_ >= 0 && selected_field_ <= 3) ? accent : Color::RGB(60, 60, 75))
        | bgcolor(Color::RGB(16, 16, 22))
        | size(WIDTH, LESS_THAN, 82);

    body.push_back(hbox({ filler(), net_card, filler() }));
    body.push_back(text(""));

    // --- Card 2: Audio Engine & Integrations ---
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
        drm_status_str = "< Enabled (CastLabs Daemon) >";
        drm_status_color = Color::GreenLight;
    }

    auto drm_elem = text(drm_status_str)
        | (selected_field_ == 4 ? color(accent) | bold : color(drm_status_color));

    auto drm_likes_elem = text(download_drm_tracks_in_likes_ ? "< Enabled (Non-GO tracks) >" : "< Disabled >")
        | (selected_field_ == 5 ? color(accent) | bold : color(download_drm_tracks_in_likes_ ? Color::GreenLight : Color::GrayLight));

    auto discord_elem = text(discord_rpc_enabled_ ? "< Enabled >" : "< Disabled >")
        | (selected_field_ == 6 ? color(accent) | bold : color(discord_rpc_enabled_ ? Color::GreenLight : Color::GrayLight));

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
    auto vis_elem = text(vis_str)
        | (selected_field_ == 7 ? color(accent) | bold : color(fullscreen_visualizer_style_ != core::FullscreenVisualizerStyle::Stretched ? Color::CyanLight : Color::White));

    auto mouse_elem = text(mouse_control_ ? "< Enabled >" : "< Disabled >")
        | (selected_field_ == 8 ? color(accent) | bold : color(mouse_control_ ? Color::GreenLight : Color::GrayLight));

    Elements audio_card_rows;
    audio_card_rows.push_back(hbox({
        text(" ⚡ AUDIO ENGINE & INTEGRATIONS ") | color(accent) | bold,
        filler()
    }));
    audio_card_rows.push_back(text(""));
    audio_card_rows.push_back(hbox({
        text("  DRM Playback Engine:  ") | color(selected_field_ == 4 ? accent : Color::GrayLight),
        drm_elem
    }));
    audio_card_rows.push_back(hbox({
        text("  Download DRM in Likes:") | color(selected_field_ == 5 ? accent : Color::GrayLight),
        drm_likes_elem
    }));
    audio_card_rows.push_back(hbox({
        text("  Discord Rich Presence:") | color(selected_field_ == 6 ? accent : Color::GrayLight),
        discord_elem
    }));
    audio_card_rows.push_back(hbox({
        text("  Fullscreen Visualizer:") | color(selected_field_ == 7 ? accent : Color::GrayLight),
        vis_elem
    }));
    audio_card_rows.push_back(hbox({
        text("  TUI Mouse Controls:   ") | color(selected_field_ == 8 ? accent : Color::GrayLight),
        mouse_elem
    }));

    auto audio_card = vbox(std::move(audio_card_rows))
        | borderRounded
        | color((selected_field_ >= 4 && selected_field_ <= 8) ? accent : Color::RGB(60, 60, 75))
        | bgcolor(Color::RGB(16, 16, 22))
        | size(WIDTH, LESS_THAN, 82);

    body.push_back(hbox({ filler(), audio_card, filler() }));
    body.push_back(text(""));

    // 2. Navigation: [Previous] and [Confirm & Start] buttons
    auto btn_prev = text(" [ ← Previous ] ")
        | (selected_field_ == 9 ? bgcolor(accent) | color(Color::Black) | bold : bgcolor(Color::RGB(30, 30, 38)) | color(Color::White) | bold);

    auto btn_start = text(" [ 🚀 Confirm & Start SCTUI ] ")
        | (selected_field_ == 10 ? bgcolor(accent) | color(Color::Black) | bold : bgcolor(Color::RGB(35, 45, 35)) | color(Color::GreenLight) | bold);

    body.push_back(hbox({
        filler(),
        btn_prev,
        text("   "),
        btn_start,
        filler()
    }));

    body.push_back(text(""));
    body.push_back(text("[h/l, Left/Right] Toggle Options  •  [j/k, Up/Down, Tab] Move  •  [Enter] Confirm & Launch") | color(Color::GrayDark) | center);

    return vbox(std::move(body));
}

Element OnboardingView::render_step4_installing_electron() {
    auto accent = state_.get_accent_color();
    clickable_zones_.clear();

    int term_w = std::max(70, state_.terminal_width);
    int card_width = std::clamp(term_w - 8, 64, 94);

    Elements body;
    body.push_back(text(""));
    body.push_back(text("Setting Up Widevine DRM Playback Engine") | color(Color::White) | bold | center);
    body.push_back(text("Installing Castlabs Electron before launching SCTUI...") | color(Color::GrayLight) | center);
    body.push_back(text(""));

    Elements card_content;
    card_content.push_back(hbox({
        text(" ⚡ CASTLABS ELECTRON SETUP ") | color(accent) | bold,
        filler()
    }));
    card_content.push_back(text(""));

    if (!install_electron_failed_) {
        card_content.push_back(text(" ⏳ Downloading Castlabs Electron (v44.9.3+wvcus)...") | color(Color::YellowLight) | bold);
        std::string cmd_str = "    Command: " + std::string(audio::DrmSatelliteDaemon::ElectronInstallCommand);
        if (cmd_str.length() > static_cast<size_t>(card_width - 8)) {
            card_content.push_back(text("    Command:") | color(Color::GrayDark));
            card_content.push_back(text("      " + std::string(audio::DrmSatelliteDaemon::ElectronInstallCommand)) | color(Color::GrayDark));
        } else {
            card_content.push_back(text(cmd_str) | color(Color::GrayDark));
        }
        card_content.push_back(text(""));
        card_content.push_back(text("    Please wait while Widevine CDM and VMP binaries are configured.") | color(Color::GrayLight));
        card_content.push_back(text("    SCTUI will automatically start once download completes.") | color(Color::GreenLight) | bold);
    } else {
        card_content.push_back(text(" ✖ Failed to download Castlabs Electron:") | color(Color::RedLight) | bold);
        card_content.push_back(text(""));

        // Dynamically wrap long error message into multiple lines so it is never clipped
        size_t max_err_line_len = static_cast<size_t>(std::max(40, card_width - 10));
        std::istringstream err_stream(install_electron_err_);
        std::string raw_paragraph;
        while (std::getline(err_stream, raw_paragraph)) {
            while (!raw_paragraph.empty() && (raw_paragraph.back() == '\r' || raw_paragraph.back() == ' ')) {
                raw_paragraph.pop_back();
            }
            if (raw_paragraph.empty()) continue;

            std::istringstream word_stream(raw_paragraph);
            std::string word;
            std::string cur_line;
            while (word_stream >> word) {
                if (cur_line.empty()) {
                    cur_line = word;
                } else if (cur_line.length() + 1 + word.length() <= max_err_line_len) {
                    cur_line += " " + word;
                } else {
                    card_content.push_back(text("    " + cur_line) | color(Color::RedLight));
                    cur_line = word;
                }
            }
            if (!cur_line.empty()) {
                card_content.push_back(text("    " + cur_line) | color(Color::RedLight));
            }
        }
        card_content.push_back(text(""));

        auto btn_retry = text(" [ 🔄 Retry Installation ] ")
            | (selected_field_ == 0 ? bgcolor(accent) | color(Color::Black) | bold : bgcolor(Color::RGB(40, 40, 50)) | color(Color::White) | bold);
        auto btn_continue = text(" [ ⏭ Continue Without DRM ] ")
            | (selected_field_ == 1 ? bgcolor(accent) | color(Color::Black) | bold : bgcolor(Color::RGB(40, 40, 50)) | color(Color::White) | bold);

        card_content.push_back(hbox({
            filler(),
            btn_retry,
            text("   "),
            btn_continue,
            filler()
        }));
    }

    auto install_card = vbox(std::move(card_content))
        | borderRounded
        | color(!install_electron_failed_ ? accent : Color::RedLight)
        | bgcolor(Color::RGB(16, 16, 22))
        | size(WIDTH, EQUAL, card_width);

    body.push_back(hbox({ filler(), install_card, filler() }));
    return vbox(std::move(body));
}

Element OnboardingView::render() {
    Elements layout;

    // Layout & Persistent Header:
    // Vertically anchored at ~3/4 coordinate from the bottom (top 25% of the screen)
    int term_h = std::max(24, state_.terminal_height);
    int top_margin = std::clamp((term_h - 26) / 4, 1, 4);

    for (int i = 0; i < top_margin; ++i) {
        layout.push_back(text(""));
    }

    // Persistent header remains identical across all steps
    layout.push_back(render_persistent_header());

    // Transitioning content area below persistent header
    if (current_step_ == OnboardingStep::Authentication) {
        layout.push_back(render_step1_auth());
    } else if (current_step_ == OnboardingStep::Appearance) {
        layout.push_back(render_step2_appearance());
    } else if (current_step_ == OnboardingStep::EngineConnectivity) {
        layout.push_back(render_step3_engine());
    } else {
        layout.push_back(render_step4_installing_electron());
    }

    layout.push_back(filler());

    return vbox(std::move(layout)) | bgcolor(Color::RGB(10, 10, 14));
}

} // namespace sctui::ui::views
