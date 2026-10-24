#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <cstdint>
#include <format>
#include <chrono>
#include <algorithm>
#include <ftxui/screen/string.hpp>

namespace sctui::core {

constexpr std::string_view kVersion = "1.0.0";

enum class ErrorCode {
    Success = 0,
    InvalidConfig,
    AuthError,
    NetworkError,
    HttpError,
    RateLimited,
    JsonParseError,
    ResourceNotFound,
    AudioDeviceError,
    AudioDecodeError,
    QueueEmpty,
    InternalError
};

struct Error {
    ErrorCode code{ErrorCode::Success};
    std::string message;
    int http_status{0};

    [[nodiscard]] bool ok() const noexcept {
        return code == ErrorCode::Success;
    }

    [[nodiscard]] std::string to_string() const {
        if (http_status > 0) {
            return std::format("[{}] {} (HTTP {})", static_cast<int>(code), message, http_status);
        }
        return std::format("[{}] {}", static_cast<int>(code), message);
    }
};

template <typename T>
using Result = std::expected<T, Error>;

inline Result<void> Ok() noexcept {
    return {};
}

template <typename T>
inline Result<T> Success(T&& value) {
    return std::forward<T>(value);
}

inline std::unexpected<Error> MakeError(ErrorCode code, std::string message, int http_status = 0) {
    return std::unexpected(Error{code, std::move(message), http_status});
}

inline std::string format_duration_ms(uint64_t ms) {
    uint64_t total_seconds = ms / 1000;
    uint64_t minutes = total_seconds / 60;
    uint64_t seconds = total_seconds % 60;
    uint64_t hours = minutes / 60;
    minutes = minutes % 60;

    if (hours > 0) {
        return std::format("{:d}:{:02d}:{:02d}", hours, minutes, seconds);
    }
    return std::format("{:02d}:{:02d}", minutes, seconds);
}

// Unicode and display-width aware truncation
inline std::string truncate_display_width(std::string_view str, int max_display_width, std::string_view suffix = "...") {
    if (max_display_width <= 0) {
        return "";
    }

    int total_width = ftxui::string_width(str);
    if (total_width <= max_display_width) {
        return std::string(str);
    }

    int suffix_w = ftxui::string_width(suffix);
    int target_w = max_display_width - suffix_w;
    if (target_w <= 0) {
        return std::string(suffix.substr(0, std::max(0, max_display_width)));
    }

    std::string result;
    int current_w = 0;
    size_t i = 0;

    while (i < str.length()) {
        size_t char_len = 1;
        unsigned char c = static_cast<unsigned char>(str[i]);
        if (c >= 0xF0) char_len = 4;
        else if (c >= 0xE0) char_len = 3;
        else if (c >= 0xC0) char_len = 2;

        if (i + char_len > str.length()) {
            break; // Stop at malformed UTF-8 sequence
        }

        std::string_view ch = str.substr(i, char_len);
        int ch_w = ftxui::string_width(ch);
        if (current_w + ch_w > target_w) {
            break;
        }

        result.append(ch);
        current_w += ch_w;
        i += char_len;
    }

    result.append(suffix);
    return result;
}

inline std::string truncate_string(std::string_view str, size_t max_len, std::string_view suffix = "...") {
    return truncate_display_width(str, static_cast<int>(max_len), suffix);
}

} // namespace sctui::core
