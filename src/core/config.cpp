#include "sctui/core/config.hpp"
#include <fstream>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace sctui::core {

std::filesystem::path Config::get_default_config_path() {
    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        return std::filesystem::path(xdg_config) / "soundcloud-tui" / "config.json";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home) / ".config" / "soundcloud-tui" / "config.json";
    }
    return std::filesystem::current_path() / "config.json";
}

std::filesystem::path Config::get_default_cache_path() {
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    if (xdg_cache && *xdg_cache) {
        return std::filesystem::path(xdg_cache) / "soundcloud-tui";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home) / ".cache" / "soundcloud-tui";
    }
    return std::filesystem::temp_directory_path() / "soundcloud-tui";
}

Result<Config> Config::load(const std::filesystem::path& path) {
    Config config;
    config.cache_dir = get_default_cache_path().string();

    if (!std::filesystem::exists(path)) {
        // Create default config file
        auto save_res = config.save(path);
        if (!save_res) {
            return config; // Return in-memory default if unable to write
        }
        return config;
    }

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return config;
        }

        nlohmann::json j;
        file >> j;

        if (j.contains("client_id") && j["client_id"].is_string()) {
            config.client_id = j["client_id"].get<std::string>();
        }
        if (j.contains("auth_token") && j["auth_token"].is_string()) {
            config.auth_token = j["auth_token"].get<std::string>();
        }
        if (j.contains("volume") && j["volume"].is_number_integer()) {
            config.volume = std::clamp(j["volume"].get<int>(), 0, 150);
        }
        if (j.contains("mouse_control") && j["mouse_control"].is_boolean()) {
            config.mouse_control = j["mouse_control"].get<bool>();
        }
        if (j.contains("repeat_mode") && j["repeat_mode"].is_number_integer()) {
            int r = j["repeat_mode"].get<int>();
            config.repeat_mode = (r >= 0 && r <= 2) ? static_cast<RepeatMode>(r) : RepeatMode::Off;
        }
        if (j.contains("shuffle") && j["shuffle"].is_boolean()) {
            config.shuffle = j["shuffle"].get<bool>();
        }
        if (j.contains("search_limit") && j["search_limit"].is_number_unsigned()) {
            config.search_limit = j["search_limit"].get<size_t>();
        }
        if (j.contains("auto_extract_client_id") && j["auto_extract_client_id"].is_boolean()) {
            config.auto_extract_client_id = j["auto_extract_client_id"].get<bool>();
        }
        if (j.contains("accent_color") && j["accent_color"].is_string()) {
            config.accent_color = j["accent_color"].get<std::string>();
        }
        if (j.contains("extracted_browser") && j["extracted_browser"].is_string()) {
            config.extracted_browser = j["extracted_browser"].get<std::string>();
        }
        if (j.contains("image_protocol") && j["image_protocol"].is_number_integer()) {
            int p = j["image_protocol"].get<int>();
            config.image_protocol = (p >= 0 && p <= 4) ? static_cast<ImageProtocol>(p) : ImageProtocol::Auto;
        }
        if (j.contains("icon_style") && j["icon_style"].is_number_integer()) {
            int is = j["icon_style"].get<int>();
            config.icon_style = (is >= 0 && is <= 2) ? static_cast<IconStyle>(is) : IconStyle::Emoji;
        } else if (j.contains("icon_style") && j["icon_style"].is_string()) {
            config.icon_style = string_to_icon_style(j["icon_style"].get<std::string>());
        }
        if (j.contains("fullscreen_visualizer_mirrored") && j["fullscreen_visualizer_mirrored"].is_boolean()) {
            config.fullscreen_visualizer_mirrored = j["fullscreen_visualizer_mirrored"].get<bool>();
        }
        if (j.contains("dns_mode") && j["dns_mode"].is_number_integer()) {
            int d = j["dns_mode"].get<int>();
            config.dns_mode = (d >= 0 && d <= 2) ? static_cast<DnsMode>(d) : DnsMode::System;
        }
        if (j.contains("doh_provider") && j["doh_provider"].is_number_integer()) {
            int dp = j["doh_provider"].get<int>();
            config.doh_provider = (dp >= 0 && dp <= 3) ? static_cast<DohProvider>(dp) : DohProvider::Cloudflare;
        }
        if (j.contains("doh_url") && j["doh_url"].is_string()) {
            config.doh_url = j["doh_url"].get<std::string>();
        }
        if (j.contains("custom_dns_server") && j["custom_dns_server"].is_string()) {
            config.custom_dns_server = j["custom_dns_server"].get<std::string>();
        }
        if (j.contains("proxy_url") && j["proxy_url"].is_string()) {
            config.proxy_url = j["proxy_url"].get<std::string>();
        }
        if (j.contains("discord_rpc_enabled") && j["discord_rpc_enabled"].is_boolean()) {
            config.discord_rpc_enabled = j["discord_rpc_enabled"].get<bool>();
        }
        if (j.contains("drm_engine_enabled") && j["drm_engine_enabled"].is_boolean()) {
            config.drm_engine_enabled = j["drm_engine_enabled"].get<bool>();
        }
        if (j.contains("download_drm_tracks_in_likes") && j["download_drm_tracks_in_likes"].is_boolean()) {
            config.download_drm_tracks_in_likes = j["download_drm_tracks_in_likes"].get<bool>();
        }
        if (j.contains("onboarding_completed") && j["onboarding_completed"].is_boolean()) {
            config.onboarding_completed = j["onboarding_completed"].get<bool>();
        }
        if (j.contains("discord_client_id") && j["discord_client_id"].is_string()) {
            config.discord_client_id = j["discord_client_id"].get<std::string>();
        }
        if (config.discord_client_id.empty() || config.discord_client_id == "1343314565749211247") {
            config.discord_client_id = "1187340825991069706";
        }
        if (j.contains("max_cache_size_mb") && j["max_cache_size_mb"].is_number_unsigned()) {
            config.max_cache_size_mb = j["max_cache_size_mb"].get<size_t>();
        }
        if (j.contains("lock_liked_tracks_in_cache") && j["lock_liked_tracks_in_cache"].is_boolean()) {
            config.lock_liked_tracks_in_cache = j["lock_liked_tracks_in_cache"].get<bool>();
        }
        if (j.contains("dont_count_liked_in_general_cache") && j["dont_count_liked_in_general_cache"].is_boolean()) {
            config.dont_count_liked_in_general_cache = j["dont_count_liked_in_general_cache"].get<bool>();
        }
        if (j.contains("cache_dir") && j["cache_dir"].is_string()) {
            config.cache_dir = j["cache_dir"].get<std::string>();
        }

        return config;
    } catch (const std::exception& e) {
        return MakeError(ErrorCode::InvalidConfig, std::string("Config parse error: ") + e.what());
    }
}

Result<void> Config::save(const std::filesystem::path& path) const {
    try {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        nlohmann::json j;
        j["client_id"] = client_id;
        j["auth_token"] = auth_token;
        j["volume"] = volume;
        j["repeat_mode"] = static_cast<int>(repeat_mode);
        j["shuffle"] = shuffle;
        j["search_limit"] = search_limit;
        j["auto_extract_client_id"] = auto_extract_client_id;
        j["accent_color"] = accent_color;
        j["extracted_browser"] = extracted_browser;
        j["mouse_control"] = mouse_control;
        j["image_protocol"] = static_cast<int>(image_protocol);
        j["icon_style"] = static_cast<int>(icon_style);
        j["fullscreen_visualizer_mirrored"] = fullscreen_visualizer_mirrored;
        j["max_cache_size_mb"] = max_cache_size_mb;
        j["lock_liked_tracks_in_cache"] = lock_liked_tracks_in_cache;
        j["dont_count_liked_in_general_cache"] = dont_count_liked_in_general_cache;
        j["dns_mode"] = static_cast<int>(dns_mode);
        j["doh_provider"] = static_cast<int>(doh_provider);
        j["doh_url"] = doh_url;
        j["custom_dns_server"] = custom_dns_server;
        j["proxy_url"] = proxy_url;
        j["discord_rpc_enabled"] = discord_rpc_enabled;
        j["discord_client_id"] = discord_client_id;
        j["drm_engine_enabled"] = drm_engine_enabled;
        j["download_drm_tracks_in_likes"] = download_drm_tracks_in_likes;
        j["onboarding_completed"] = onboarding_completed;
        j["cache_dir"] = cache_dir;

        std::ofstream file(path);
        if (!file.is_open()) {
            return MakeError(ErrorCode::InvalidConfig, "Cannot open config file for writing: " + path.string());
        }

        file << j.dump(4) << std::endl;
        return Ok();
    } catch (const std::exception& e) {
        return MakeError(ErrorCode::InvalidConfig, std::string("Failed to save config: ") + e.what());
    }
}

} // namespace sctui::core
