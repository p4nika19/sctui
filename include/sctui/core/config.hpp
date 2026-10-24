#pragma once

#include <string>
#include <filesystem>
#include <cstdint>
#include <optional>
#include "sctui/core/types.hpp"

namespace sctui::core {

enum class RepeatMode {
    Off = 0,
    One = 1,
    All = 2
};

enum class ImageProtocol {
    Auto = 0,
    Kitty = 1,
    ITerm2 = 2,
    HalfBlock = 3,
    Disabled = 4
};

enum class DnsMode {
    System = 0,
    DoH = 1,
    Custom = 2
};

enum class DohProvider {
    Cloudflare = 0,
    Google = 1,
    Quad9 = 2,
    Custom = 3
};

enum class IconStyle {
    Emoji = 0,       // Full color emojis: ☁, 🔍, 👤, ❤, 📁, 🔀, 🔁, 🔊, 🔇, ⚙
    Unicode = 1,     // Clean Unicode typographic symbols: ☁, ⌕, ☺, ♥, ≡, ⇄, ↻, ☊, ⛭
    DefaultAscii = 2 // Plain ASCII / Default symbols: [SC], [/], [@], <3>, [=], [~], [R], (Vol), [*]
};

struct Config {
    std::string client_id;
    std::string auth_token;
    std::string extracted_browser; // Auto-detected browser source for auth_token
    int volume{75}; // 0 - 150
    RepeatMode repeat_mode{RepeatMode::Off};
    bool shuffle{false};
    std::string cache_dir;
    size_t search_limit{20};
    bool auto_extract_client_id{true};
    std::string accent_color{"#FF5500"};

    // Mouse control in TUI (by default turned off)
    bool mouse_control{false};

    // Terminal cover art protocol
    ImageProtocol image_protocol{ImageProtocol::Auto};

    // UI Symbols and Icons style
    IconStyle icon_style{IconStyle::Emoji};

    // Track caching & Liked Locking options
    size_t max_cache_size_mb{1024}; // In megabytes (0 = unlimited)
    bool lock_liked_tracks_in_cache{true}; // Lock/pin liked tracks in cache to prevent eviction
    bool dont_count_liked_in_general_cache{false}; // Liked tracks do not count toward max_cache_size_mb

    // DNS & Geo-unblock networking
    DnsMode dns_mode{DnsMode::System};
    DohProvider doh_provider{DohProvider::Cloudflare};
    std::string doh_url{"https://cloudflare-dns.com/dns-query"};
    std::string custom_dns_server{"1.1.1.1"};
    std::string proxy_url; // e.g. "socks5://127.0.0.1:1080" or "http://127.0.0.1:7890"

    // Discord Rich Presence (RPC)
    bool discord_rpc_enabled{true};
    std::string discord_client_id{"1187340825991069706"};

    // DRM Satellite Playback (castlabs-electron)
    bool drm_engine_enabled{true};
    bool download_drm_tracks_in_likes{true};

    [[nodiscard]] std::string get_effective_doh_url() const {
        if (dns_mode != DnsMode::DoH) return "";
        switch (doh_provider) {
            case DohProvider::Cloudflare:
                return "https://cloudflare-dns.com/dns-query";
            case DohProvider::Google:
                return "https://dns.google/dns-query";
            case DohProvider::Quad9:
                return "https://dns.quad9.net/dns-query";
            case DohProvider::Custom:
                return doh_url.empty() ? "https://cloudflare-dns.com/dns-query" : doh_url;
        }
        return doh_url;
    }

    static const char* image_protocol_to_string(ImageProtocol proto) {
        switch (proto) {
            case ImageProtocol::Auto: return "Auto (Detect)";
            case ImageProtocol::Kitty: return "Kitty Protocol";
            case ImageProtocol::ITerm2: return "iTerm2 Inline";
            case ImageProtocol::HalfBlock: return "Pixelated (Unicode 24-bit)";
            case ImageProtocol::Disabled: return "Disabled";
        }
        return "Unknown";
    }

    static const char* dns_mode_to_string(DnsMode mode) {
        switch (mode) {
            case DnsMode::System: return "System Default";
            case DnsMode::DoH: return "DNS-over-HTTPS (DoH)";
            case DnsMode::Custom: return "Custom DNS Server";
        }
        return "Unknown";
    }

    static const char* doh_provider_to_string(DohProvider prov) {
        switch (prov) {
            case DohProvider::Cloudflare: return "Cloudflare (1.1.1.1)";
            case DohProvider::Google: return "Google (8.8.8.8)";
            case DohProvider::Quad9: return "Quad9 (9.9.9.9)";
            case DohProvider::Custom: return "Custom DoH URL";
        }
        return "Unknown";
    }

    static const char* icon_style_to_string(IconStyle style) {
        switch (style) {
            case IconStyle::Emoji: return "Emoji (Color Emojis: ☁, 🔍, 👤, ❤, 📁)";
            case IconStyle::Unicode: return "Unicode (Clean Symbols: ☁, ⌕, ☺, ♥, ≡)";
            case IconStyle::DefaultAscii: return "Default (Plain ASCII: [SC], [/], [@], <3>)";
        }
        return "Emoji";
    }

    static IconStyle string_to_icon_style(const std::string& str) {
        if (str.find("Unicode") != std::string::npos) return IconStyle::Unicode;
        if (str.find("Default") != std::string::npos || str.find("ASCII") != std::string::npos) return IconStyle::DefaultAscii;
        return IconStyle::Emoji;
    }

    static std::filesystem::path get_default_config_path();
    static std::filesystem::path get_default_cache_path();

    static Result<Config> load(const std::filesystem::path& path = get_default_config_path());
    Result<void> save(const std::filesystem::path& path = get_default_config_path()) const;
};

} // namespace sctui::core
