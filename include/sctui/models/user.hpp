#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace sctui::models {

struct ColorRGB {
    uint8_t r{255};
    uint8_t g{85};
    uint8_t b{0};

    constexpr bool operator==(const ColorRGB& other) const = default;
};

struct CoverArtImage {
    int width{0};
    int height{0};
    std::vector<ColorRGB> pixels;
    std::string png_base64;
    std::vector<uint8_t> raw_image_bytes;
};

enum class SubscriptionTier {
    Free = 0,
    Go,
    GoPlus
};

struct UserProfile {
    uint64_t id{0};
    std::string username;
    std::string permalink;
    std::string permalink_url;
    std::string avatar_url;
    std::string description;
    std::string country;
    std::string city;
    std::string plan;
    std::string consumer_subscription_id{"free"};
    SubscriptionTier subscription_tier{SubscriptionTier::Free};
    std::string first_name;
    std::string last_name;
    std::string full_name;
    std::string primary_email;
    std::string created_at;
    bool is_verified{false};
    bool is_pro{false};
    uint64_t followers_count{0};
    uint64_t followings_count{0};
    uint64_t track_count{0};
    uint64_t likes_count{0};
    uint64_t playlist_count{0};
    uint64_t reposts_count{0};

    std::optional<CoverArtImage> avatar_image;

    [[nodiscard]] bool has_go() const noexcept {
        return subscription_tier == SubscriptionTier::Go || subscription_tier == SubscriptionTier::GoPlus;
    }

    [[nodiscard]] bool has_go_plus() const noexcept {
        return subscription_tier == SubscriptionTier::GoPlus;
    }
};

} // namespace sctui::models
