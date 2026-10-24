#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace sctui::core {

struct ExtractedCookie {
    std::string token;
    std::string browser_name;
    std::filesystem::path db_path;
};

struct ExtractionResult {
    bool success{false};
    std::optional<ExtractedCookie> cookie;
    std::vector<std::string> detected_browsers;
    std::string message;
    std::string clickable_login_url; // OSC 8 terminal hyperlink
    std::string plain_login_url{"https://soundcloud.com/signin"};
};

class CookieExtractor {
public:
    // Scans Mozilla-based browsers first, then Chromium-based browsers.
    // If found, returns ExtractedCookie with oauth_token and browser name.
    // If not found, lists all detected browsers and provides login instructions with OSC 8 clickable link.
    static ExtractionResult extract_soundcloud_token();

    // Returns a list of installed/detected browser names on the system
    static std::vector<std::string> detect_installed_browsers();

    // Helper: formats an OSC 8 terminal hyperlink
    static std::string format_osc8_link(const std::string& url, const std::string& label);
};

} // namespace sctui::core
