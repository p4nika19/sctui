#include "sctui/core/cookie_extractor.hpp"
#include "sctui/core/logger.hpp"
#include <sqlite3.h>
#include <openssl/evp.h>
#include <cstdlib>
#include <vector>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace sctui::core {

namespace {

std::filesystem::path get_home_dir() {
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home);
    }
    return std::filesystem::current_path();
}

struct BrowserDef {
    std::string name;
    bool is_mozilla;
    std::vector<std::filesystem::path> search_roots;
};

std::vector<BrowserDef> get_browser_definitions() {
    auto home = get_home_dir();
    std::vector<BrowserDef> defs;

    // --- 1. Mozilla-based browsers (Scanned FIRST) ---
    defs.push_back({
        .name = "Mozilla Firefox",
        .is_mozilla = true,
        .search_roots = {
            home / ".mozilla" / "firefox",
            home / ".config" / "mozilla" / "firefox",
            home / ".var" / "app" / "org.mozilla.firefox" / ".mozilla" / "firefox",
            home / "snap" / "firefox" / "common" / ".mozilla" / "firefox"
        }
    });

    defs.push_back({
        .name = "LibreWolf",
        .is_mozilla = true,
        .search_roots = {
            home / ".librewolf",
            home / ".config" / "librewolf",
            home / ".var" / "app" / "io.gitlab.librewolf-community" / ".librewolf"
        }
    });

    defs.push_back({
        .name = "Floorp",
        .is_mozilla = true,
        .search_roots = {
            home / ".floorp",
            home / ".config" / "floorp",
            home / ".var" / "app" / "one.ablaze.floorp" / ".floorp"
        }
    });

    defs.push_back({
        .name = "Waterfox",
        .is_mozilla = true,
        .search_roots = {
            home / ".waterfox",
            home / ".config" / "waterfox"
        }
    });

    defs.push_back({
        .name = "Pale Moon",
        .is_mozilla = true,
        .search_roots = {
            home / ".moonchild productions" / "pale moon"
        }
    });

    defs.push_back({
        .name = "SeaMonkey",
        .is_mozilla = true,
        .search_roots = {
            home / ".mozilla" / "seamonkey"
        }
    });

    // --- 2. Chromium-based browsers (Scanned AFTER Mozilla) ---
    defs.push_back({
        .name = "Google Chrome",
        .is_mozilla = false,
        .search_roots = {
            home / ".config" / "google-chrome",
            home / ".var" / "app" / "com.google.Chrome" / "config" / "google-chrome"
        }
    });

    defs.push_back({
        .name = "Chromium",
        .is_mozilla = false,
        .search_roots = {
            home / ".config" / "chromium",
            home / ".var" / "app" / "org.chromium.Chromium" / "config" / "chromium",
            home / "snap" / "chromium" / "common" / "chromium"
        }
    });

    defs.push_back({
        .name = "Brave Browser",
        .is_mozilla = false,
        .search_roots = {
            home / ".config" / "BraveSoftware" / "Brave-Browser",
            home / ".var" / "app" / "com.brave.Browser" / "config" / "BraveSoftware" / "Brave-Browser"
        }
    });

    defs.push_back({
        .name = "Microsoft Edge",
        .is_mozilla = false,
        .search_roots = {
            home / ".config" / "microsoft-edge",
            home / ".config" / "microsoft-edge-dev",
            home / ".config" / "microsoft-edge-beta"
        }
    });

    defs.push_back({
        .name = "Opera",
        .is_mozilla = false,
        .search_roots = {
            home / ".config" / "opera"
        }
    });

    defs.push_back({
        .name = "Vivaldi",
        .is_mozilla = false,
        .search_roots = {
            home / ".config" / "vivaldi"
        }
    });

    defs.push_back({
        .name = "Yandex Browser",
        .is_mozilla = false,
        .search_roots = {
            home / ".config" / "yandex-browser"
        }
    });

    return defs;
}

std::vector<std::filesystem::path> find_cookie_files(const BrowserDef& browser) {
    std::vector<std::filesystem::path> results;
    std::error_code ec;

    for (const auto& root : browser.search_roots) {
        if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
            continue;
        }

        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) continue;

                auto filename = entry.path().filename().string();
                if (browser.is_mozilla) {
                    if (filename == "cookies.sqlite") {
                        results.push_back(entry.path());
                    }
                } else {
                    if (filename == "Cookies") {
                        results.push_back(entry.path());
                    }
                }
            }
        } catch (...) {
            // Ignore filesystem permission errors
        }
    }

    return results;
}

std::string get_linux_secret_storage_password(const std::string& browser_name) {
    std::vector<std::vector<std::string>> candidates;
    std::string b_lower = browser_name;
    std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(), ::tolower);

    if (b_lower.find("chrome") != std::string::npos) {
        candidates = {
            {"secret-tool", "lookup", "application", "chrome"},
            {"secret-tool", "lookup", "application", "google-chrome"},
            {"secret-tool", "lookup", "application", "Google Chrome"},
            {"secret-tool", "lookup", "label", "Chrome Safe Storage"}
        };
    } else if (b_lower.find("chromium") != std::string::npos) {
        candidates = {
            {"secret-tool", "lookup", "application", "chromium"},
            {"secret-tool", "lookup", "label", "Chromium Safe Storage"}
        };
    } else if (b_lower.find("brave") != std::string::npos) {
        candidates = {
            {"secret-tool", "lookup", "application", "brave"},
            {"secret-tool", "lookup", "label", "Brave Safe Storage"}
        };
    } else if (b_lower.find("edge") != std::string::npos) {
        candidates = {
            {"secret-tool", "lookup", "application", "microsoft-edge"},
            {"secret-tool", "lookup", "label", "Microsoft Edge Safe Storage"}
        };
    } else if (b_lower.find("vivaldi") != std::string::npos) {
        candidates = {
            {"secret-tool", "lookup", "application", "vivaldi"},
            {"secret-tool", "lookup", "label", "Vivaldi Safe Storage"}
        };
    } else if (b_lower.find("opera") != std::string::npos) {
        candidates = {
            {"secret-tool", "lookup", "application", "opera"},
            {"secret-tool", "lookup", "label", "Opera Safe Storage"}
        };
    } else {
        candidates = {
            {"secret-tool", "lookup", "application", "chrome"},
            {"secret-tool", "lookup", "label", "Chrome Safe Storage"}
        };
    }

    for (const auto& cmd_args : candidates) {
        std::string cmd = cmd_args[0] + " " + cmd_args[1] + " " + cmd_args[2] + " \"" + cmd_args[3] + "\" 2>/dev/null";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            char buffer[512];
            std::string result;
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                result += buffer;
            }
            pclose(pipe);
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
                result.pop_back();
            }
            if (!result.empty()) {
                return result;
            }
        }
    }

    return "peanuts";
}

std::optional<std::string> decrypt_chromium_value(
    const unsigned char* encrypted_data,
    size_t encrypted_len,
    const std::string& browser_name
) {
    if (!encrypted_data || encrypted_len < 3) {
        return std::nullopt;
    }

    // Check version prefix
    bool is_v10 = (encrypted_len >= 3 && encrypted_data[0] == 'v' && encrypted_data[1] == '1' && encrypted_data[2] == '0');
    bool is_v11 = (encrypted_len >= 3 && encrypted_data[0] == 'v' && encrypted_data[1] == '1' && encrypted_data[2] == '1');

    if (!is_v10 && !is_v11) {
        std::string raw(reinterpret_cast<const char*>(encrypted_data), encrypted_len);
        if (!raw.empty()) return raw;
        return std::nullopt;
    }

    std::string password = get_linux_secret_storage_password(browser_name);
    const unsigned char salt[] = "saltysalt";
    unsigned char key[16];

    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.length()), salt, 8, 1, EVP_sha1(), 16, key) != 1) {
        return std::nullopt;
    }

    // Linux Chromium uses 16 space characters (0x20) as IV
    unsigned char iv[16];
    std::memset(iv, 0x20, 16);

    const unsigned char* ciphertext = encrypted_data + 3;
    int ciphertext_len = static_cast<int>(encrypted_len - 3);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }

    std::vector<unsigned char> plaintext(static_cast<size_t>(ciphertext_len) + 32);
    int out_len1 = 0;
    if (EVP_DecryptUpdate(ctx, plaintext.data(), &out_len1, ciphertext, ciphertext_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }

    int out_len2 = 0;
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len1, &out_len2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }

    EVP_CIPHER_CTX_free(ctx);
    int total_len = out_len1 + out_len2;
    if (total_len <= 0) return std::nullopt;

    // In v11, the first 32 bytes are a SHA256 HMAC/nonce prefix
    int start_offset = 0;
    if (is_v11) {
        if (total_len <= 32) return std::nullopt;
        start_offset = 32;
    }

    std::string decrypted_str(reinterpret_cast<char*>(plaintext.data() + start_offset), static_cast<size_t>(total_len - start_offset));
    if (decrypted_str.empty()) return std::nullopt;

    return decrypted_str;
}

std::optional<std::string> query_sqlite_token(
    const std::filesystem::path& db_path,
    bool is_mozilla,
    const std::string& browser_name = ""
) {
    sqlite3* db = nullptr;
    std::string uri = "file:" + db_path.string() + "?immutable=1";
    int rc = sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);

    // If opening with immutable URI fails or locks, try a quick temporary snapshot copy
    std::filesystem::path temp_copy_path;
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        db = nullptr;

        try {
            auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
            temp_copy_path = std::filesystem::temp_directory_path() / ("sctui_cookie_tmp_" + std::to_string(ts) + ".sqlite");
            std::error_code copy_ec;
            std::filesystem::copy_file(db_path, temp_copy_path, std::filesystem::copy_options::overwrite_existing, copy_ec);
            if (!copy_ec) {
                rc = sqlite3_open_v2(temp_copy_path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
            }
        } catch (...) {}
    }

    if (rc != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        if (!temp_copy_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(temp_copy_path, ec);
        }
        return std::nullopt;
    }

    std::string sql;
    if (is_mozilla) {
        sql = "SELECT value FROM moz_cookies WHERE host LIKE '%soundcloud.com%' AND name = 'oauth_token' ORDER BY lastAccessed DESC LIMIT 1;";
    } else {
        sql = "SELECT value, encrypted_value FROM cookies WHERE host_key LIKE '%soundcloud.com%' AND name = 'oauth_token' ORDER BY creation_utc DESC LIMIT 1;";
    }

    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        sqlite3_close(db);
        if (!temp_copy_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(temp_copy_path, ec);
        }
        return std::nullopt;
    }

    std::optional<std::string> token;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* val = sqlite3_column_text(stmt, 0);
        if (val) {
            std::string s = reinterpret_cast<const char*>(val);
            if (!s.empty()) {
                token = s;
            }
        }

        // If unencrypted value was empty, decrypt Chromium encrypted_value blob
        if (!token.has_value() && !is_mozilla) {
            const void* blob = sqlite3_column_blob(stmt, 1);
            int blob_bytes = sqlite3_column_bytes(stmt, 1);
            if (blob && blob_bytes > 3) {
                token = decrypt_chromium_value(
                    static_cast<const unsigned char*>(blob),
                    static_cast<size_t>(blob_bytes),
                    browser_name
                );
            }
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (!temp_copy_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(temp_copy_path, ec);
    }

    return token;
}

} // namespace

std::string CookieExtractor::format_osc8_link(const std::string& url, const std::string& label) {
    // OSC 8 terminal hyperlink format
    return "\033]8;;" + url + "\033\\" + label + "\033]8;;\033\\";
}

std::vector<std::string> CookieExtractor::detect_installed_browsers() {
    auto defs = get_browser_definitions();
    std::vector<std::string> installed;
    std::error_code ec;

    for (const auto& def : defs) {
        for (const auto& root : def.search_roots) {
            if (std::filesystem::exists(root, ec)) {
                installed.push_back(def.name);
                break;
            }
        }
    }

    return installed;
}

ExtractionResult CookieExtractor::extract_soundcloud_token() {
    ExtractionResult result;
    result.plain_login_url = "https://soundcloud.com/signin";
    result.clickable_login_url = format_osc8_link(result.plain_login_url, result.plain_login_url);

    auto defs = get_browser_definitions();

    // Collect installed browsers
    std::error_code ec;
    for (const auto& def : defs) {
        for (const auto& root : def.search_roots) {
            if (std::filesystem::exists(root, ec)) {
                result.detected_browsers.push_back(def.name);
                break;
            }
        }
    }

    // Step 1: Scan Mozilla-based browsers first
    for (const auto& def : defs) {
        if (!def.is_mozilla) continue;

        auto cookie_files = find_cookie_files(def);
        for (const auto& file : cookie_files) {
            auto token = query_sqlite_token(file, true);
            if (token && !token->empty()) {
                result.success = true;
                result.cookie = ExtractedCookie{
                    .token = *token,
                    .browser_name = def.name,
                    .db_path = file
                };
                result.message = "Successfully extracted oauth_token from " + def.name;
                LOG_INFO("CookieExtractor", result.message + " (" + file.string() + ")");
                return result;
            }
        }
    }

    // Step 2: Scan Chromium-based browsers second
    for (const auto& def : defs) {
        if (def.is_mozilla) continue;

        auto cookie_files = find_cookie_files(def);
        for (const auto& file : cookie_files) {
            auto token = query_sqlite_token(file, false, def.name);
            if (token && !token->empty()) {
                result.success = true;
                result.cookie = ExtractedCookie{
                    .token = *token,
                    .browser_name = def.name,
                    .db_path = file
                };
                result.message = "Successfully extracted oauth_token from " + def.name;
                LOG_INFO("CookieExtractor", result.message + " (" + file.string() + ")");
                return result;
            }
        }
    }

    // Step 3: Not found in any browser
    result.success = false;
    std::string browsers_str;
    if (result.detected_browsers.empty()) {
        browsers_str = "No standard browsers detected";
    } else {
        for (size_t i = 0; i < result.detected_browsers.size(); ++i) {
            if (i > 0) browsers_str += ", ";
            browsers_str += result.detected_browsers[i];
        }
    }

    result.message = "No active SoundCloud login session found in detected browsers (" +
                     browsers_str + "). Please log in to SoundCloud in your browser (" +
                     result.plain_login_url + ") and retry auto-extraction.";

    LOG_WARN("CookieExtractor", result.message);
    return result;
}

} // namespace sctui::core
