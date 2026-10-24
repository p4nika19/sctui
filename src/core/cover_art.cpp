#include "sctui/core/cover_art.hpp"
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <ftxui/dom/node.hpp>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#pragma clang diagnostic ignored "-Wpedantic"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize2.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace sctui::core {

namespace {

class TerminalGraphicsNode : public ftxui::Node {
public:
    TerminalGraphicsNode(int width, int height, std::string escape_seq, uint32_t image_id = 1)
        : width_(width), height_(height), escape_seq_(std::move(escape_seq)), image_id_(image_id) {}

    void ComputeRequirement() override {
        requirement_.min_x = width_;
        requirement_.min_y = height_;
    }

    void Render(ftxui::Screen& screen) override {
        if (box_.x_min > box_.x_max || box_.y_min > box_.y_max) return;
        // Guard against terminal bottom overflow: if bottom of box exceeds screen, do not draw image
        if (box_.y_max >= screen.dimy()) return;

        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            for (int x = box_.x_min; x <= box_.x_max; ++x) {
                if (x >= 0 && x < screen.dimx() && y >= 0 && y < screen.dimy()) {
                    auto& p = screen.PixelAt(x, y);
                    p.character = " ";
                }
            }
        }

        // Always place the graphics escape sequence at the top-left cell.
        // FTXUI's internal double-buffer screen diffing automatically prevents
        // re-emitting identical cell payloads across unchanged frames.
        if (box_.x_min >= 0 && box_.x_min < screen.dimx() &&
            box_.y_min >= 0 && box_.y_min < screen.dimy()) {
            screen.PixelAt(box_.x_min, box_.y_min).character = escape_seq_;
        }
    }

private:
    int width_;
    int height_;
    std::string escape_seq_;
    [[maybe_unused]] uint32_t image_id_{1};
};

} // namespace

ImageProtocol CoverArtProcessor::detect_terminal_graphics_protocol() {
    // Check for environments that do NOT support raw graphics protocols
    const char* zed_term = std::getenv("ZED_TERM");
    if (zed_term && *zed_term) return ImageProtocol::HalfBlock;

    const char* term_prog = std::getenv("TERM_PROGRAM");
    if (term_prog) {
        std::string tp = term_prog;
        if (tp == "zed") return ImageProtocol::HalfBlock;
    }

    const char* term = std::getenv("TERM");
    if (term) {
        std::string t = term;
        if (t == "dumb" || t == "linux") return ImageProtocol::HalfBlock;
    }

    const char* kitty_id = std::getenv("KITTY_WINDOW_ID");
    const char* kitty_pid = std::getenv("KITTY_PID");
    const char* ghostty_res = std::getenv("GHOSTTY_RESOURCES_DIR");
    const char* term_info = std::getenv("TERMINFO");

    if ((kitty_id && *kitty_id) || (kitty_pid && *kitty_pid)) {
        return ImageProtocol::Kitty;
    }
    if (ghostty_res && *ghostty_res) {
        return ImageProtocol::Kitty;
    }
    if (term_prog) {
        std::string tp = term_prog;
        if (tp == "ghostty" || tp == "WezTerm") return ImageProtocol::Kitty;
        if (tp == "iTerm.app" || tp == "vscode") return ImageProtocol::ITerm2;
    }
    if (term) {
        std::string t = term;
        if (t.find("kitty") != std::string::npos) return ImageProtocol::Kitty;
    }
    if (term_info && std::string(term_info).find("kitty") != std::string::npos) {
        return ImageProtocol::Kitty;
    }

    const char* lc_term = std::getenv("LC_TERMINAL");
    if (lc_term && std::string(lc_term) == "iTerm2") {
        return ImageProtocol::ITerm2;
    }

    return ImageProtocol::HalfBlock;
}

ImageProtocol CoverArtProcessor::resolve_image_protocol(ImageProtocol config_proto) {
    if (config_proto == ImageProtocol::Auto) {
        return detect_terminal_graphics_protocol();
    }
    return config_proto;
}

std::string CoverArtProcessor::encode_base64(const uint8_t* data, size_t length) {
    static const char* kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((length + 2) / 3) * 4);

    size_t i = 0;
    while (i < length) {
        uint32_t octet_a = i < length ? data[i++] : 0;
        uint32_t octet_b = i < length ? data[i++] : 0;
        uint32_t octet_c = i < length ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        result.push_back(kChars[(triple >> 18) & 0x3F]);
        result.push_back(kChars[(triple >> 12) & 0x3F]);
        result.push_back((i > length + 1) ? '=' : kChars[(triple >> 6) & 0x3F]);
        result.push_back((i > length) ? '=' : kChars[triple & 0x3F]);
    }
    return result;
}

std::string CoverArtProcessor::upgrade_artwork_url(const std::string& original_url) {
    if (original_url.empty()) return "";

    static const std::string kPatterns[] = {
        "-large.", "-badge.", "-small.", "-tiny.", "-crop.", "-t67x67.", "-t300x300."
    };

    for (const auto& pat : kPatterns) {
        auto pos = original_url.find(pat);
        if (pos != std::string::npos) {
            std::string upgraded = original_url;
            upgraded.replace(pos, pat.length() - 1, "-t500x500");
            return upgraded;
        }
    }
    return original_url;
}

std::optional<models::CoverArtImage> CoverArtProcessor::decode_and_resize(
    const uint8_t* data,
    size_t size,
    int target_w,
    int target_h
) {
    if (!data || size == 0 || target_w <= 0 || target_h <= 0) {
        return std::nullopt;
    }

    int in_w = 0;
    int in_h = 0;
    int channels = 0;

    unsigned char* decoded = stbi_load_from_memory(
        data,
        static_cast<int>(size),
        &in_w,
        &in_h,
        &channels,
        3 // Force 3 channels (RGB)
    );

    if (!decoded) {
        return std::nullopt;
    }

    std::vector<uint8_t> resized(static_cast<size_t>(target_w * target_h * 3));
    unsigned char* res = stbir_resize_uint8_srgb(
        decoded,
        in_w,
        in_h,
        0,
        resized.data(),
        target_w,
        target_h,
        0,
        STBIR_RGB
    );

    models::CoverArtImage image;
    image.width = target_w;
    image.height = target_h;
    image.pixels.resize(static_cast<size_t>(target_w * target_h));
    image.raw_image_bytes.assign(data, data + size);

    if (res) {
        for (int i = 0; i < target_w * target_h; ++i) {
            image.pixels[static_cast<size_t>(i)] = models::ColorRGB{
                resized[static_cast<size_t>(i * 3)],
                resized[static_cast<size_t>(i * 3 + 1)],
                resized[static_cast<size_t>(i * 3 + 2)]
            };
        }
    }

    // Generate high-resolution PNG for Kitty / iTerm2 non-pixelated graphics protocols
    // Keep full crisp native resolution up to 500x500
    int hr_w = in_w;
    int hr_h = in_h;
    const uint8_t* png_src = decoded;
    std::vector<uint8_t> hr_resized;

    if (in_w > 500 || in_h > 500) {
        hr_w = 500;
        hr_h = 500;
        hr_resized.resize(static_cast<size_t>(hr_w * hr_h * 3));
        if (stbir_resize_uint8_srgb(decoded, in_w, in_h, 0, hr_resized.data(), hr_w, hr_h, 0, STBIR_RGB)) {
            png_src = hr_resized.data();
        } else {
            hr_w = in_w;
            hr_h = in_h;
            png_src = decoded;
        }
    }

    int png_len = 0;
    unsigned char* png_mem = stbi_write_png_to_mem(png_src, hr_w * 3, hr_w, hr_h, 3, &png_len);
    if (png_mem && png_len > 0) {
        image.png_base64 = encode_base64(png_mem, static_cast<size_t>(png_len));
        STBIW_FREE(png_mem);
    }

    stbi_image_free(decoded);

    if (!res) {
        return std::nullopt;
    }

    return image;
}

models::ColorRGB CoverArtProcessor::extract_dominant_color(const models::CoverArtImage& image) {
    if (image.pixels.empty()) {
        return models::ColorRGB{255, 85, 0};
    }

    constexpr int kNumBins = 24;
    float bin_weights[kNumBins] = {0.0f};
    float bin_r[kNumBins] = {0.0f};
    float bin_g[kNumBins] = {0.0f};
    float bin_b[kNumBins] = {0.0f};

    float total_r = 0.0f;
    float total_g = 0.0f;
    float total_b = 0.0f;

    for (const auto& px : image.pixels) {
        float r = static_cast<float>(px.r) / 255.0f;
        float g = static_cast<float>(px.g) / 255.0f;
        float b = static_cast<float>(px.b) / 255.0f;

        total_r += r;
        total_g += g;
        total_b += b;

        float cmax = std::max({r, g, b});
        float cmin = std::min({r, g, b});
        float delta = cmax - cmin;

        float lum = 0.299f * r + 0.587f * g + 0.114f * b;
        float sat = (cmax > 0.001f) ? (delta / cmax) : 0.0f;

        if (lum < 0.12f || (lum > 0.92f && sat < 0.15f) || sat < 0.18f) {
            continue;
        }

        float hue = 0.0f;
        if (delta > 0.0001f) {
            if (cmax == r) {
                hue = 60.0f * std::fmod(((g - b) / delta), 6.0f);
            } else if (cmax == g) {
                hue = 60.0f * (((b - r) / delta) + 2.0f);
            } else {
                hue = 60.0f * (((r - g) / delta) + 4.0f);
            }
            if (hue < 0.0f) {
                hue += 360.0f;
            }
        }

        int bin = std::clamp(static_cast<int>(hue / 15.0f), 0, kNumBins - 1);

        float lum_weight = 1.0f - std::min(1.0f, std::abs(lum - 0.60f) * 1.5f);
        float weight = std::pow(sat, 1.4f) * lum_weight;
        if (weight <= 0.001f) weight = 0.001f;

        bin_weights[bin] += weight;
        bin_r[bin] += r * weight;
        bin_g[bin] += g * weight;
        bin_b[bin] += b * weight;
    }

    int best_bin = -1;
    float max_weight = 0.0f;
    for (int i = 0; i < kNumBins; ++i) {
        if (bin_weights[i] > max_weight) {
            max_weight = bin_weights[i];
            best_bin = i;
        }
    }

    float res_r = 255.0f;
    float res_g = 85.0f;
    float res_b = 0.0f;

    if (best_bin >= 0 && max_weight > 0.01f) {
        res_r = (bin_r[best_bin] / bin_weights[best_bin]) * 255.0f;
        res_g = (bin_g[best_bin] / bin_weights[best_bin]) * 255.0f;
        res_b = (bin_b[best_bin] / bin_weights[best_bin]) * 255.0f;
    } else {
        float avg_lum = (0.299f * total_r + 0.587f * total_g + 0.114f * total_b) / static_cast<float>(image.pixels.size());
        if (avg_lum > 0.4f) {
            return models::ColorRGB{220, 220, 225};
        }
        return models::ColorRGB{255, 85, 0};
    }

    float perceived_lum = 0.299f * res_r + 0.587f * res_g + 0.114f * res_b;
    if (perceived_lum < 110.0f) {
        float boost = 120.0f / std::max(perceived_lum, 1.0f);
        res_r = std::min(255.0f, res_r * boost);
        res_g = std::min(255.0f, res_g * boost);
        res_b = std::min(255.0f, res_b * boost);
    }

    float max_c = std::max({res_r, res_g, res_b});
    if (max_c < 150.0f && max_c > 0.0f) {
        float boost = 170.0f / max_c;
        res_r = std::min(255.0f, res_r * boost);
        res_g = std::min(255.0f, res_g * boost);
        res_b = std::min(255.0f, res_b * boost);
    }

    return models::ColorRGB{
        static_cast<uint8_t>(std::clamp(std::round(res_r), 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(std::round(res_g), 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(std::round(res_b), 0.0f, 255.0f))
    };
}

ftxui::Element CoverArtProcessor::render_cover_element(
    const models::CoverArtImage& image,
    ImageProtocol protocol,
    uint32_t image_id,
    int target_w,
    int target_h
) {
    using namespace ftxui;
    if (target_w <= 0 || target_h <= 0) {
        target_w = 18;
        target_h = 9;
    }

    if (image.pixels.empty() || image.width <= 0 || image.height <= 0) {
        return render_placeholder(target_w, target_h);
    }

    ImageProtocol active_proto = resolve_image_protocol(protocol);

    if (active_proto == ImageProtocol::Disabled) {
        return render_placeholder(target_w, target_h, extract_dominant_color(image));
    }

    if (active_proto == ImageProtocol::Kitty && !image.png_base64.empty()) {
        constexpr size_t kChunkSize = 4096;
        const std::string& b64 = image.png_base64;
        std::string seq;

        if (b64.size() <= kChunkSize) {
            seq = "\033_Ga=T,f=100,t=d,i=" + std::to_string(image_id) +
                  ",c=" + std::to_string(target_w) + ",r=" + std::to_string(target_h) +
                  ",C=1,q=2;" + b64 + "\033\\ ";
        } else {
            size_t offset = 0;
            bool first = true;
            while (offset < b64.size()) {
                size_t len = std::min(kChunkSize, b64.size() - offset);
                bool last = (offset + len >= b64.size());
                std::string chunk = b64.substr(offset, len);

                if (first) {
                    seq += "\033_Ga=T,f=100,t=d,i=" + std::to_string(image_id) +
                           ",c=" + std::to_string(target_w) + ",r=" + std::to_string(target_h) +
                           ",C=1,q=2,m=" + std::string(last ? "0" : "1") + ";" + chunk + "\033\\";
                    first = false;
                } else {
                    seq += "\033_Gm=" + std::string(last ? "0" : "1") + ";" + chunk + "\033\\";
                }
                offset += len;
            }
            seq += " "; // Space advances cursor 1 cell to synchronize with FTXUI's layout
        }
        return std::make_shared<TerminalGraphicsNode>(target_w, target_h, std::move(seq), image_id);
    }

    if (active_proto == ImageProtocol::ITerm2 && !image.png_base64.empty()) {
        std::string seq = "\033]1337;File=inline=1;width=" + std::to_string(target_w) +
                          ";height=" + std::to_string(target_h) + ";preserveAspectRatio=1:" +
                          image.png_base64 + "\a ";
        return std::make_shared<TerminalGraphicsNode>(target_w, target_h, std::move(seq), image_id);
    }

    // High-fidelity 24-bit truecolor Unicode half-blocks (▀)
    std::vector<models::ColorRGB> pixel_grid;
    int grid_w = target_w;
    int grid_h = target_h * 2;

    if (!image.raw_image_bytes.empty()) {
        int in_w = 0, in_h = 0, ch = 0;
        unsigned char* dec = stbi_load_from_memory(
            image.raw_image_bytes.data(),
            static_cast<int>(image.raw_image_bytes.size()),
            &in_w, &in_h, &ch, 3
        );
        if (dec) {
            std::vector<uint8_t> res_buf(static_cast<size_t>(grid_w * grid_h * 3));
            if (stbir_resize_uint8_srgb(dec, in_w, in_h, 0, res_buf.data(), grid_w, grid_h, 0, STBIR_RGB)) {
                pixel_grid.resize(static_cast<size_t>(grid_w * grid_h));
                for (int i = 0; i < grid_w * grid_h; ++i) {
                    pixel_grid[static_cast<size_t>(i)] = models::ColorRGB{
                        res_buf[static_cast<size_t>(i * 3)],
                        res_buf[static_cast<size_t>(i * 3 + 1)],
                        res_buf[static_cast<size_t>(i * 3 + 2)]
                    };
                }
            }
            stbi_image_free(dec);
        }
    }

    if (pixel_grid.empty()) {
        pixel_grid.resize(static_cast<size_t>(grid_w * grid_h));
        for (int y = 0; y < grid_h; ++y) {
            int src_y = std::clamp(y * image.height / grid_h, 0, image.height - 1);
            for (int x = 0; x < grid_w; ++x) {
                int src_x = std::clamp(x * image.width / grid_w, 0, image.width - 1);
                pixel_grid[static_cast<size_t>(y * grid_w + x)] =
                    image.pixels[static_cast<size_t>(src_y * image.width + src_x)];
            }
        }
    }

    Elements rows;
    rows.reserve(static_cast<size_t>(target_h));

    for (int y = 0; y < target_h; ++y) {
        Elements cols;
        cols.reserve(static_cast<size_t>(target_w));

        int top_row = 2 * y;
        int bot_row = 2 * y + 1;

        for (int x = 0; x < target_w; ++x) {
            const auto& top = pixel_grid[static_cast<size_t>(top_row * grid_w + x)];
            const auto& bot = pixel_grid[static_cast<size_t>(bot_row * grid_w + x)];

            cols.push_back(
                text("▀")
                | color(Color::RGB(top.r, top.g, top.b))
                | bgcolor(Color::RGB(bot.r, bot.g, bot.b))
            );
        }
        rows.push_back(hbox(std::move(cols)));
    }

    return vbox(std::move(rows));
}

ftxui::Element CoverArtProcessor::render_placeholder(int width, int height, const models::ColorRGB& accent) {
    using namespace ftxui;
    auto accent_c = Color::RGB(accent.r, accent.g, accent.b);
    auto dim_c = Color::GrayDark;

    if (width == 18 && height == 9) {
        return vbox({
            text(".----------------.") | color(dim_c) | center,
            hbox({ text("|") | color(dim_c), text("     .----.     ") | color(accent_c), text("|") | color(dim_c) }) | center,
            hbox({ text("|") | color(dim_c), text("    /  ●   \\    ") | color(accent_c), text("|") | color(dim_c) }) | center,
            hbox({ text("|") | color(dim_c), text("   |   ◎    |   ") | color(accent_c) | bold, text("|") | color(dim_c) }) | center,
            hbox({ text("|") | color(dim_c), text("    \\  ●   /    ") | color(accent_c), text("|") | color(dim_c) }) | center,
            hbox({ text("|") | color(dim_c), text("     '----'     ") | color(accent_c), text("|") | color(dim_c) }) | center,
            hbox({ text("|") | color(dim_c), text("                ") | color(dim_c), text("|") | color(dim_c) }) | center,
            hbox({ text("|") | color(dim_c), text("   SOUNDCLOUD   ") | color(accent_c) | bold, text("|") | color(dim_c) }) | center,
            text("'----------------'") | color(dim_c) | center
        });
    }

    Elements lines;
    lines.push_back(text("." + std::string(std::max(2, width - 2), '-') + ".") | color(dim_c) | center);
    for (int y = 1; y < height - 1; ++y) {
        if (y == height / 2 - 1) {
            lines.push_back(hbox({ text("|") | color(dim_c), text(" ◎ ") | color(accent_c) | bold | center | xflex, text("|") | color(dim_c) }));
        } else if (y == height / 2) {
            lines.push_back(hbox({ text("|") | color(dim_c), text(" SOUNDCLOUD ") | color(accent_c) | bold | center | xflex, text("|") | color(dim_c) }));
        } else {
            lines.push_back(hbox({ text("|") | color(dim_c), filler(), text("|") | color(dim_c) }));
        }
    }
    lines.push_back(text("'" + std::string(std::max(2, width - 2), '-') + "'") | color(dim_c) | center);
    return vbox(std::move(lines)) | size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height);
}

void CoverArtProcessor::clear_terminal_image(uint32_t image_id) {
    if (image_id == 0) {
        std::cout << "\033_Ga=d,d=a,q=2\033\\" << std::flush;
    } else {
        std::cout << "\033_Ga=d,d=i,i=" << image_id << ",q=2\033\\" << std::flush;
    }
}

} // namespace sctui::core
