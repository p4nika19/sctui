#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <ftxui/dom/elements.hpp>
#include "sctui/models/state.hpp"
#include "sctui/core/config.hpp"

namespace sctui::core {

class CoverArtProcessor {
public:
    // Detects whether the terminal supports Kitty or iTerm2 graphics protocols, or fallback
    [[nodiscard]] static ImageProtocol detect_terminal_graphics_protocol();

    // Resolves Auto to concrete protocol
    [[nodiscard]] static ImageProtocol resolve_image_protocol(ImageProtocol config_proto);

    // Encodes raw byte buffer to Base64
    [[nodiscard]] static std::string encode_base64(const uint8_t* data, size_t length);

    // Transforms SoundCloud artwork URL to high-resolution (e.g. -large.jpg -> -t500x500.jpg)
    [[nodiscard]] static std::string upgrade_artwork_url(const std::string& original_url);

    // Decodes binary image data (JPEG, PNG, etc.) and downsamples to target dimensions
    [[nodiscard]] static std::optional<models::CoverArtImage> decode_and_resize(
        const uint8_t* data,
        size_t size,
        int target_w = 18,
        int target_h = 18
    );

    // Extracts dominant vibrant accent color from the image using weighted HSV hue binning
    [[nodiscard]] static models::ColorRGB extract_dominant_color(const models::CoverArtImage& image);

    // Renders the cover art into FTXUI Element using requested/detected protocol
    [[nodiscard]] static ftxui::Element render_cover_element(
        const models::CoverArtImage& image,
        ImageProtocol protocol = ImageProtocol::Auto,
        uint32_t image_id = 1,
        int target_w = 18,
        int target_h = 8
    );

    // Clears Kitty terminal image overlay (0 = all images, or specific image_id)
    static void clear_terminal_image(uint32_t image_id = 0);

    // Renders a stylized placeholder when artwork is missing or loading
    [[nodiscard]] static ftxui::Element render_placeholder(int width = 18, int height = 9, const models::ColorRGB& accent = {255, 85, 0});
};

} // namespace sctui::core
