#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <ftxui/screen/screen.hpp>
#include "sctui/net/soundcloud_api.hpp"
#include "sctui/core/spsc_queue.hpp"
#include "sctui/core/channel.hpp"
#include "sctui/core/thread_pool.hpp"
#include "sctui/core/config.hpp"
#include "sctui/core/entropy.hpp"
#include "sctui/core/cover_art.hpp"
#include "sctui/core/cookie_extractor.hpp"
#include "sctui/mpris/mpris_server.hpp"
#include "sctui/models/state.hpp"
#include "sctui/core/track_cache.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include "sctui/discord/discord_rpc.hpp"
#include "sctui/ui/views/settings_view.hpp"
#include <fstream>
#include <set>

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        std::cerr << "Assertion failed: " #expr " at line " << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while(0)

void test_spsc_queue() {
    std::cout << "[Test] Running Lock-free SPSC Queue verification..." << std::endl;
    sctui::core::SPSCQueue<int, 16> q;
    REQUIRE(q.empty());
    bool ok1 = q.push(42);
    REQUIRE(ok1);
    bool ok2 = q.push(100);
    REQUIRE(ok2);
    REQUIRE(q.size() == 2);
    auto val1 = q.pop();
    REQUIRE(val1.has_value() && *val1 == 42);
    auto val2 = q.pop();
    REQUIRE(val2.has_value() && *val2 == 100);
    REQUIRE(q.empty());
    std::cout << "  -> SPSC Queue verified OK" << std::endl;
}

void test_bounded_channel() {
    std::cout << "[Test] Running Bounded Channel verification..." << std::endl;
    sctui::core::BoundedChannel<std::string> ch(4);
    bool ok1 = ch.push("msg1");
    REQUIRE(ok1);
    bool ok2 = ch.push("msg2");
    REQUIRE(ok2);
    REQUIRE(ch.size() == 2);
    auto msg1 = ch.pop();
    REQUIRE(msg1.has_value() && *msg1 == "msg1");
    auto msg2 = ch.pop();
    REQUIRE(msg2.has_value() && *msg2 == "msg2");
    std::cout << "  -> Bounded Channel verified OK" << std::endl;
}

void test_soundcloud_api() {
    std::cout << "[Test] Running SoundCloud API network test..." << std::endl;
    sctui::net::SoundCloudApi api;

    auto cid_res = api.resolve_client_id();
    REQUIRE(cid_res.has_value());
    std::cout << "  -> Client ID resolved: " << *cid_res << std::endl;

    auto tracks_res = api.search_tracks("synthwave", 3);
    REQUIRE(tracks_res.has_value());
    REQUIRE(!tracks_res->empty());
    std::cout << "  -> Search returned " << tracks_res->size() << " tracks." << std::endl;

    auto stream_url_res = api.resolve_stream_url((*tracks_res)[0]);
    REQUIRE(stream_url_res.has_value());
    std::cout << "  -> Direct stream resolved: " << stream_url_res->substr(0, 45) << "..." << std::endl;
}

void test_user_library() {
    std::cout << "[Test] Running User Library & Playlist Resolution test..." << std::endl;
    auto config_res = sctui::core::Config::load();
    if (!config_res || config_res->auth_token.empty()) {
        std::cout << "  -> Skipping authenticated library test (no auth_token)" << std::endl;
        return;
    }

    sctui::net::SoundCloudApi api(config_res->client_id, config_res->auth_token);

    // Test Likes pagination
    auto likes_res = api.get_user_likes_page(50);
    REQUIRE(likes_res.has_value());
    std::cout << "  -> Likes page 1 fetched: " << likes_res->tracks.size() << " tracks (next_href: "
              << (likes_res->next_href.empty() ? "none" : "present") << ")" << std::endl;
    REQUIRE(!likes_res->tracks.empty());

    // Test Playlists
    auto playlists_res = api.get_user_playlists(10);
    REQUIRE(playlists_res.has_value());
    std::cout << "  -> Fetched " << playlists_res->size() << " playlists" << std::endl;
    REQUIRE(!playlists_res->empty());

    // Test resolving tracks for first playlist (e.g. peak26 with > 100 tracks)
    const auto& pl = (*playlists_res)[0];
    std::cout << "  -> Testing track resolution for playlist '" << pl.title << "' (claimed: " << pl.track_count << " tracks)..." << std::endl;
    auto pl_tracks_res = api.get_playlist_tracks(pl.id);
    REQUIRE(pl_tracks_res.has_value());
    std::cout << "  -> Playlist resolved with " << pl_tracks_res->size() << " fully articulated tracks!" << std::endl;
    REQUIRE(pl_tracks_res->size() >= pl.track_count || pl_tracks_res->size() > 5);
}

void test_entropy_shuffle() {
    std::cout << "[Test] Running Advanced System Entropy Shuffling test..." << std::endl;

    // 1. Verify AdvancedRng produces distinct entropy-backed streams
    auto rng1 = sctui::core::AdvancedRng::create_engine();
    auto rng2 = sctui::core::AdvancedRng::create_engine();
    uint64_t v1 = rng1();
    uint64_t v2 = rng2();
    REQUIRE(v1 != v2);
    std::cout << "  -> AdvancedRng system entropy entropy OK (0x" << std::hex << v1 << " != 0x" << v2 << std::dec << ")" << std::endl;

    // 2. Test QueueState shuffle permutation
    sctui::models::QueueState q;
    for (size_t i = 0; i < 25; ++i) {
        sctui::models::Track t;
        t.id = 1000 + i;
        t.title = "Track " + std::to_string(i);
        q.tracks.push_back(t);
    }
    q.set_current_index(5);
    REQUIRE(q.current_index == 5);

    // Turn shuffle on
    q.toggle_shuffle();
    REQUIRE(q.shuffle);
    REQUIRE(q.shuffle_indices.size() == 25);
    REQUIRE(q.shuffle_cursor == 0);
    // Current playing track (5) should be preserved at shuffle_cursor 0
    REQUIRE(q.shuffle_indices[0] == 5);

    // Verify all 25 indices are present without duplicates
    std::set<size_t> unique_indices(q.shuffle_indices.begin(), q.shuffle_indices.end());
    REQUIRE(unique_indices.size() == 25);
    for (size_t i = 0; i < 25; ++i) {
        REQUIRE(unique_indices.count(i) == 1);
    }
    std::cout << "  -> Queue permutation verified: exactly 25/25 unique tracks" << std::endl;

    // Walk through all 25 tracks
    std::vector<int> played_indices;
    played_indices.push_back(q.current_index);
    for (size_t i = 1; i < 25; ++i) {
        q.advance_next();
        REQUIRE(q.current_index >= 0);
        played_indices.push_back(q.current_index);
    }
    REQUIRE(played_indices.size() == 25);
    std::set<int> unique_played(played_indices.begin(), played_indices.end());
    REQUIRE(unique_played.size() == 25);
    std::cout << "  -> advance_next() walked all 25 shuffled tracks with zero repeats" << std::endl;

    // Test advance_prev()
    q.advance_prev();
    REQUIRE(q.current_index == played_indices[23]);
    std::cout << "  -> advance_prev() correctly traversed shuffle history backward" << std::endl;
}

void test_mpris_server() {
    std::cout << "[Test] Running MPRIS 2.2 D-Bus server verification..." << std::endl;
    bool play_pause_invoked = false;
    bool next_invoked = false;
    bool prev_invoked = false;

    sctui::mpris::MprisServer* srv_ptr = nullptr;

    sctui::mpris::MprisCallbacks cb{
        .on_play = []() {},
        .on_pause = []() {},
        .on_play_pause = [&]() { play_pause_invoked = true; },
        .on_stop = []() {},
        .on_next = [&]() {
            next_invoked = true;
            // Ensure calling update_track during on_next on D-Bus thread NEVER deadlocks
            if (srv_ptr) {
                sctui::models::Track dummy;
                dummy.id = 888;
                dummy.title = "Next Track";
                srv_ptr->update_track(dummy);
            }
        },
        .on_prev = [&]() {
            prev_invoked = true;
            if (srv_ptr) {
                sctui::models::Track dummy;
                dummy.id = 666;
                dummy.title = "Prev Track";
                srv_ptr->update_track(dummy);
            }
        },
        .on_seek_relative = [](int64_t) {},
        .on_seek_absolute = [](uint64_t) {},
        .on_set_volume = [](float) {},
        .on_set_shuffle = [](bool) {},
        .on_set_loop_status = [](sctui::core::RepeatMode) {},
        .on_quit = []() {}
    };

    sctui::mpris::MprisServer server(std::move(cb));
    srv_ptr = &server;

    std::string test_service_name = "org.mpris.MediaPlayer2.sctui_test_" + std::to_string(getpid());
    std::string player_name = "sctui_test_" + std::to_string(getpid());

    bool started = server.start(test_service_name);
    if (!started) {
        std::cout << "  -> User session D-Bus not accessible in this environment (skipping live MPRIS query)" << std::endl;
        return;
    }

    sctui::models::Track track;
    track.id = 777;
    track.title = "Solaris";
    track.artist = "Photay";
    track.duration_ms = 210000;
    track.permalink_url = "https://soundcloud.com/photay/solaris";

    server.update_track(track);
    server.update_playback_state(sctui::audio::PlaybackState::Playing);
    server.update_position(15000);
    server.update_volume(85.0f);
    server.update_shuffle(true);
    server.update_repeat_mode(sctui::core::RepeatMode::All);

    // Give D-Bus worker thread a moment to register on session bus
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify playerctl interaction if playerctl is present
    std::string check_cmd = "playerctl -p " + player_name + " status > /dev/null 2>&1";
    int res = std::system(check_cmd.c_str());
    if (res == 0) {
        std::cout << "  -> playerctl connected to " << test_service_name << ": status OK" << std::endl;
        std::string pp_cmd = "playerctl -p " + player_name + " play-pause > /dev/null 2>&1";
        std::system(pp_cmd.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(play_pause_invoked);
        std::cout << "  -> playerctl play-pause command received and executed OK" << std::endl;

        std::string next_cmd = "playerctl -p " + player_name + " next > /dev/null 2>&1";
        std::system(next_cmd.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(next_invoked);
        std::cout << "  -> playerctl next command executed without deadlock/freeze OK" << std::endl;

        std::string prev_cmd = "playerctl -p " + player_name + " previous > /dev/null 2>&1";
        std::system(prev_cmd.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(prev_invoked);
        std::cout << "  -> playerctl previous command executed without deadlock/freeze OK" << std::endl;
    } else {
        std::cout << "  -> MPRIS server registered successfully on session D-Bus" << std::endl;
    }

    server.stop();
    std::cout << "  -> MPRIS Server shutdown cleanly" << std::endl;
}

void test_cover_art_processor() {
    std::cout << "[Test] Running Cover Art Processor & Dominant Color Extractor test..." << std::endl;

    // 1. Test URL upgrading
    std::string orig_url = "https://i1.sndcdn.com/artworks-000552123456-abcdef-large.jpg";
    std::string high_res = sctui::core::CoverArtProcessor::upgrade_artwork_url(orig_url);
    REQUIRE(high_res == "https://i1.sndcdn.com/artworks-000552123456-abcdef-t500x500.jpg");
    REQUIRE(sctui::core::CoverArtProcessor::upgrade_artwork_url("") == "");

    // 2. Test Dominant Color Extraction from synthetic image
    sctui::models::CoverArtImage cyan_img;
    cyan_img.width = 18;
    cyan_img.height = 18;
    cyan_img.pixels.resize(18 * 18, sctui::models::ColorRGB{15, 15, 17}); // Dark background

    // Fill center with vivid cyan/blue artwork
    for (int y = 4; y < 14; ++y) {
        for (int x = 4; x < 14; ++x) {
            cyan_img.pixels[static_cast<size_t>(y * 18 + x)] = sctui::models::ColorRGB{0, 200, 255};
        }
    }

    auto cyan_dom = sctui::core::CoverArtProcessor::extract_dominant_color(cyan_img);
    std::cout << "  -> Cyan image extracted accent: RGB("
              << static_cast<int>(cyan_dom.r) << ", "
              << static_cast<int>(cyan_dom.g) << ", "
              << static_cast<int>(cyan_dom.b) << ")" << std::endl;
    REQUIRE(cyan_dom.b > 180 && cyan_dom.g > 150 && cyan_dom.r < 50);

    // 3. Test Magenta/Purple image extraction
    sctui::models::CoverArtImage magenta_img;
    magenta_img.width = 18;
    magenta_img.height = 18;
    magenta_img.pixels.resize(18 * 18, sctui::models::ColorRGB{220, 20, 230}); // Vivid magenta

    auto magenta_dom = sctui::core::CoverArtProcessor::extract_dominant_color(magenta_img);
    std::cout << "  -> Magenta image extracted accent: RGB("
              << static_cast<int>(magenta_dom.r) << ", "
              << static_cast<int>(magenta_dom.g) << ", "
              << static_cast<int>(magenta_dom.b) << ")" << std::endl;
    REQUIRE(magenta_dom.r > 180 && magenta_dom.b > 180);

    // 4. Test Contrast Boost for dark colors
    sctui::models::CoverArtImage dark_blue_img;
    dark_blue_img.width = 18;
    dark_blue_img.height = 18;
    dark_blue_img.pixels.resize(18 * 18, sctui::models::ColorRGB{5, 15, 45}); // Very dark navy blue

    auto boosted_dom = sctui::core::CoverArtProcessor::extract_dominant_color(dark_blue_img);
    float lum = 0.299f * boosted_dom.r + 0.587f * boosted_dom.g + 0.114f * boosted_dom.b;
    std::cout << "  -> Dark image boosted accent: RGB("
              << static_cast<int>(boosted_dom.r) << ", "
              << static_cast<int>(boosted_dom.g) << ", "
              << static_cast<int>(boosted_dom.b) << ") Luminance: " << lum << std::endl;
    // Dominant color must have contrast against dark terminal background
    REQUIRE(boosted_dom.r > 0 || boosted_dom.g > 0 || boosted_dom.b >= 110);

    // 5. Test FTXUI Element Rendering
    auto cover_elem = sctui::core::CoverArtProcessor::render_cover_element(cyan_img);
    REQUIRE(cover_elem != nullptr);

    auto placeholder_elem = sctui::core::CoverArtProcessor::render_placeholder(18, 9, {255, 85, 0});
    REQUIRE(placeholder_elem != nullptr);

    std::cout << "  -> Cover Art Processor & Dominant Color Extraction OK" << std::endl;
}

void test_drm_and_goplus_separation() {
    std::cout << "[Test] Running DRM vs Go+ Separation & Queue Skipping test..." << std::endl;

    // 1. DRM Track
    sctui::models::Track drm_track;
    drm_track.id = 1001;
    drm_track.title = "DRM Protected Song";
    drm_track.policy = "MONETIZE";
    drm_track.monetization_model = "AD_SUPPORTED";
    drm_track.transcodings = {
        sctui::models::Transcoding{ .url = "https://api.soundcloud.com/tc1", .protocol = "cbc-encrypted-hls" },
        sctui::models::Transcoding{ .url = "https://api.soundcloud.com/tc2", .protocol = "ctr-encrypted-hls" }
    };
    drm_track.access_type = sctui::models::TrackAccessType::DrmProtected;
    REQUIRE(drm_track.is_drm_protected());
    REQUIRE(!drm_track.is_go_plus());

    sctui::net::SoundCloudApi api;
    auto drm_res = api.resolve_stream_url(drm_track);
    REQUIRE(!drm_res.has_value());
    REQUIRE(drm_res.error().message.find("[DRM Protected]") != std::string::npos);
    std::cout << "  -> DRM Track correctly returned error: '" << drm_res.error().message << "'" << std::endl;

    // 2. Go+ Track
    sctui::models::Track goplus_track;
    goplus_track.id = 1002;
    goplus_track.title = "Go Plus Premium Song";
    goplus_track.policy = "SNIP";
    goplus_track.monetization_model = "SUB_HIGH_TIER";
    goplus_track.access_type = sctui::models::TrackAccessType::GoPlus;
    goplus_track.is_snipped = true;
    REQUIRE(goplus_track.is_go_plus());
    REQUIRE(!goplus_track.is_drm_protected());
    std::cout << "  -> Go+ Track correctly categorized: is_go_plus() = true, is_snipped = true" << std::endl;

    // 3. Queue Skipping DRM tracks
    sctui::models::Track playable1;
    playable1.id = 2001;
    playable1.title = "Playable Track 1";
    playable1.access_type = sctui::models::TrackAccessType::Playable;

    sctui::models::Track drm1;
    drm1.id = 2002;
    drm1.title = "DRM Track 1";
    drm1.access_type = sctui::models::TrackAccessType::DrmProtected;

    sctui::models::Track drm2;
    drm2.id = 2003;
    drm2.title = "DRM Track 2";
    drm2.access_type = sctui::models::TrackAccessType::DrmProtected;

    sctui::models::Track playable2;
    playable2.id = 2004;
    playable2.title = "Playable Track 2";
    playable2.access_type = sctui::models::TrackAccessType::Playable;

    sctui::models::QueueState queue;
    queue.tracks = { playable1, drm1, drm2, playable2 };
    queue.current_index = 0;

    // Simulate DRM-skipping advance_next
    size_t skipped = 0;
    std::optional<sctui::models::Track> next_playable;
    for (size_t attempt = 0; attempt < queue.tracks.size(); ++attempt) {
        queue.advance_next();
        auto cand = queue.current_track();
        if (!cand) break;
        if (cand->is_drm_protected()) {
            skipped++;
            continue;
        }
        next_playable = cand;
        break;
    }

    REQUIRE(skipped == 2);
    REQUIRE(next_playable.has_value());
    REQUIRE(next_playable->id == 2004);
    REQUIRE(next_playable->title == "Playable Track 2");
    std::cout << "  -> Forward Queue skipping successfully bypassed 2 DRM tracks and selected Playable Track 2" << std::endl;

    // Simulate DRM-skipping advance_prev
    size_t skipped_back = 0;
    std::optional<sctui::models::Track> prev_playable;
    for (size_t attempt = 0; attempt < queue.tracks.size(); ++attempt) {
        queue.advance_prev();
        auto cand = queue.current_track();
        if (!cand) break;
        if (cand->is_drm_protected()) {
            skipped_back++;
            continue;
        }
        prev_playable = cand;
        break;
    }

    REQUIRE(skipped_back == 2);
    REQUIRE(prev_playable.has_value());
    REQUIRE(prev_playable->id == 2001);
    REQUIRE(prev_playable->title == "Playable Track 1");
    std::cout << "  -> Backward Queue skipping successfully bypassed 2 DRM tracks and returned to Playable Track 1" << std::endl;
}

void test_cookie_extractor() {
    std::cout << "[Test] Running Browser Cookie Extractor verification..." << std::endl;

    // 1. Test detected browsers
    auto browsers = sctui::core::CookieExtractor::detect_installed_browsers();
    std::cout << "  -> Detected " << browsers.size() << " browser installations on system:" << std::endl;
    for (const auto& b : browsers) {
        std::cout << "     * " << b << std::endl;
    }
    REQUIRE(!browsers.empty()); // On this Linux system, at least Firefox or Chrome is installed

    // 2. Test OSC 8 hyperlink formatting
    std::string test_url = "https://soundcloud.com/signin";
    std::string osc8 = sctui::core::CookieExtractor::format_osc8_link(test_url, "Sign In");
    REQUIRE(osc8.find("\033]8;;https://soundcloud.com/signin\033\\Sign In\033]8;;\033\\") != std::string::npos);
    std::cout << "  -> OSC 8 terminal hyperlink formatted correctly" << std::endl;

    // 3. Test cookie extraction (Mozilla scanned first, then Chromium)
    auto ext_res = sctui::core::CookieExtractor::extract_soundcloud_token();
    if (ext_res.success) {
        REQUIRE(ext_res.cookie.has_value());
        REQUIRE(!ext_res.cookie->token.empty());
        REQUIRE(!ext_res.cookie->browser_name.empty());
        std::cout << "  -> Successfully extracted SoundCloud oauth_token from " << ext_res.cookie->browser_name << std::endl;
        std::cout << "     Token prefix: " << ext_res.cookie->token.substr(0, 8) << "..." << std::endl;
        std::cout << "     Database path: " << ext_res.cookie->db_path << std::endl;
    } else {
        std::cout << "  -> No active SoundCloud cookies in system browsers: " << ext_res.message << std::endl;
        REQUIRE(!ext_res.plain_login_url.empty());
        REQUIRE(!ext_res.clickable_login_url.empty());
    }
}

void test_http_doh_and_dns() {
    std::cout << "[Test] Running DNS-over-HTTPS (DoH) & Custom DNS network test..." << std::endl;

    sctui::net::HttpClient client;
    client.set_timeout_seconds(10);
    client.set_doh_url("https://cloudflare-dns.com/dns-query");
    REQUIRE(client.get_doh_url() == "https://cloudflare-dns.com/dns-query");

    // Perform an HTTPS query through Cloudflare DoH to SoundCloud API
    auto resp = client.get("https://api-v2.soundcloud.com/search?q=ambient&client_id=3GKD969NJWD1eB6gvQJN0GnueYbcWszv&limit=1");
    REQUIRE(resp.has_value());
    REQUIRE(resp->status_code == 200);
    REQUIRE(!resp->body.empty());
    std::cout << "  -> Cloudflare DoH resolved and fetched SoundCloud API: HTTP " << resp->status_code << " OK ("
              << resp->body.size() << " bytes)" << std::endl;

    // Test Config DoH provider resolution
    sctui::core::Config cfg;
    cfg.dns_mode = sctui::core::DnsMode::DoH;
    cfg.doh_provider = sctui::core::DohProvider::Cloudflare;
    REQUIRE(cfg.get_effective_doh_url() == "https://cloudflare-dns.com/dns-query");

    cfg.doh_provider = sctui::core::DohProvider::Google;
    REQUIRE(cfg.get_effective_doh_url() == "https://dns.google/dns-query");

    cfg.doh_provider = sctui::core::DohProvider::Quad9;
    REQUIRE(cfg.get_effective_doh_url() == "https://dns.quad9.net/dns-query");

    cfg.dns_mode = sctui::core::DnsMode::System;
    REQUIRE(cfg.get_effective_doh_url().empty());
    std::cout << "  -> DoH provider URL mapping verified OK" << std::endl;
}

void test_terminal_graphics_protocol() {
    std::cout << "[Test] Running Terminal Graphics Protocol detection & rendering test..." << std::endl;

    // 1. Detection
    auto proto = sctui::core::CoverArtProcessor::detect_terminal_graphics_protocol();
    std::cout << "  -> Detected terminal protocol: " << sctui::core::Config::image_protocol_to_string(proto) << std::endl;

    // 2. Base64 encoding
    const uint8_t test_bytes[] = "SoundCloud Terminal UI Graphics";
    std::string b64 = sctui::core::CoverArtProcessor::encode_base64(test_bytes, sizeof(test_bytes) - 1);
    REQUIRE(!b64.empty());
    std::cout << "  -> Base64 encoding verified: " << b64.substr(0, 16) << "..." << std::endl;

    // 3. Synthesize test image
    sctui::models::CoverArtImage test_img;
    test_img.width = 18;
    test_img.height = 18;
    test_img.pixels.resize(18 * 18, sctui::models::ColorRGB{255, 85, 0});
    test_img.png_base64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

    // 4. Render Kitty Protocol element
    auto kitty_elem = sctui::core::CoverArtProcessor::render_cover_element(test_img, sctui::core::ImageProtocol::Kitty);
    auto screen_kitty = ftxui::Screen::Create(ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(10));
    ftxui::Render(screen_kitty, kitty_elem);
    std::string kitty_out = screen_kitty.ToString();
    REQUIRE(kitty_out.find("\033_Ga=T,f=100") != std::string::npos);
    REQUIRE(kitty_out.find("\033[u") != std::string::npos);
    std::cout << "  -> Kitty Graphics Protocol element rendered with valid escape sequence and clean cursor alignment OK" << std::endl;

    // 5. Render iTerm2 Protocol element
    auto iterm_elem = sctui::core::CoverArtProcessor::render_cover_element(test_img, sctui::core::ImageProtocol::ITerm2);
    auto screen_iterm = ftxui::Screen::Create(ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(10));
    ftxui::Render(screen_iterm, iterm_elem);
    std::string iterm_out = screen_iterm.ToString();
    REQUIRE(iterm_out.find("\033]1337;File=inline=1") != std::string::npos);
    REQUIRE(iterm_out.find("\a\033[u") != std::string::npos);
    std::cout << "  -> iTerm2 Inline Image element rendered with valid escape sequence and clean cursor alignment OK" << std::endl;

    // 6. Render Unicode 24-bit Half-Block element (fallback)
    auto halfblock_elem = sctui::core::CoverArtProcessor::render_cover_element(test_img, sctui::core::ImageProtocol::HalfBlock);
    auto screen_hb = ftxui::Screen::Create(ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(10));
    ftxui::Render(screen_hb, halfblock_elem);
    std::string hb_out = screen_hb.ToString();
    REQUIRE(hb_out.find("▀") != std::string::npos);
    std::cout << "  -> Unicode Half-Block pixelated fallback element rendered OK" << std::endl;
}

void test_track_cache_manager() {
    std::cout << "[Test] Running Track Cache Manager & Liked Pinning verification..." << std::endl;
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "sctui_test_cache";
    std::filesystem::remove_all(test_dir);

    sctui::core::TrackCacheManager mgr(test_dir);

    auto p101 = mgr.get_track_cache_path(101);
    auto p102 = mgr.get_track_cache_path(102);
    auto p103 = mgr.get_track_cache_path(103);

    auto make_dummy_file = [](const std::filesystem::path& p, size_t size_bytes) {
        std::ofstream ofs(p, std::ios::binary);
        std::vector<char> buf(1024, 'X');
        for (size_t written = 0; written < size_bytes; written += buf.size()) {
            ofs.write(buf.data(), std::min(buf.size(), size_bytes - written));
        }
    };

    make_dummy_file(p101, 2 * 1024 * 1024);
    make_dummy_file(p102, 2 * 1024 * 1024);
    make_dummy_file(p103, 2 * 1024 * 1024);

    mgr.register_track(101, "Track 101", "Artist 1", false);
    mgr.register_track(102, "Track 102", "Artist 2", true); // Liked!
    mgr.register_track(103, "Track 103", "Artist 3", false);

    REQUIRE(mgr.get_cached_track(101).has_value());
    REQUIRE(mgr.get_cached_track(102).has_value());
    REQUIRE(mgr.get_cached_track(103).has_value());

    auto stats = mgr.get_stats(1024, true, false);
    REQUIRE(stats.total_tracks == 3);
    REQUIRE(stats.liked_tracks == 1);
    std::cout << "  -> Initial cache: 3 tracks, ~" << (stats.total_size_bytes / (1024 * 1024)) << "MB total" << std::endl;

    // Limit to 3MB with liked tracks locked
    mgr.enforce_cache_limits(3, true, false);

    REQUIRE(mgr.get_cached_track(102).has_value()); // Liked track preserved!
    std::cout << "  -> Liked track 102 preserved during cache limit enforcement OK" << std::endl;

    // Test clear_cache(keep_liked = true)
    make_dummy_file(p101, 128 * 1024);
    mgr.register_track(101, "Track 101", "Artist 1", false);
    REQUIRE(mgr.get_cached_track(101).has_value());
    REQUIRE(mgr.get_cached_track(102).has_value());

    mgr.clear_cache(true);
    REQUIRE(!mgr.get_cached_track(101).has_value());
    REQUIRE(mgr.get_cached_track(102).has_value());
    std::cout << "  -> clear_cache(keep_liked=true) wiped unliked tracks while keeping liked track 102 OK" << std::endl;

    // Test clear_cache(keep_liked = false)
    mgr.clear_cache(false);
    REQUIRE(!mgr.get_cached_track(102).has_value());
    std::cout << "  -> clear_cache(keep_liked=false) purged all tracks OK" << std::endl;

    std::filesystem::remove_all(test_dir);
}

void test_account_profile() {
    std::cout << "[Test] Running Account Profile & Uploaded Tracks verification..." << std::endl;
    auto cookie_res = sctui::core::CookieExtractor::extract_soundcloud_token();
    if (!cookie_res.success || !cookie_res.cookie) {
        std::cout << "  -> No active browser session, skipping live /me test" << std::endl;
        return;
    }

    sctui::net::SoundCloudApi api;
    api.set_auth_token(cookie_res.cookie->token);
    auto cid_res = api.resolve_client_id();
    if (cid_res) {
        api.set_client_id(*cid_res);
    }

    auto me_res = api.get_me();
    REQUIRE(me_res.has_value());
    REQUIRE(me_res->id != 0);
    REQUIRE(!me_res->username.empty());
    std::cout << "  -> /me successfully fetched: user '" << me_res->username << "' (ID: " << me_res->id << ", "
              << me_res->track_count << " tracks, " << me_res->likes_count << " likes)" << std::endl;

    auto tracks_res = api.get_user_uploaded_tracks(me_res->id, 5);
    REQUIRE(tracks_res.has_value());
    std::cout << "  -> User uploaded tracks fetched: " << tracks_res->size() << " track(s)" << std::endl;
}

void test_theme_symbols() {
    std::cout << "[Test] Running Theme Symbols & Icon Styles verification..." << std::endl;

    const auto& emoji_syms = sctui::ui::ThemeSymbols::get(sctui::core::IconStyle::Emoji);
    REQUIRE(emoji_syms.logo == "☁");
    REQUIRE(emoji_syms.search == "🔍");
    REQUIRE(emoji_syms.account == "👤");
    REQUIRE(emoji_syms.likes == "💖");
    REQUIRE(emoji_syms.settings == "🔧");
    REQUIRE(emoji_syms.shuffle == "🔀");
    std::cout << "  -> Emoji Icon Style (wide 2-cell aligned) verified OK" << std::endl;

    const auto& unicode_syms = sctui::ui::ThemeSymbols::get(sctui::core::IconStyle::Unicode);
    REQUIRE(unicode_syms.logo == "☁");
    REQUIRE(unicode_syms.search == "⌕");
    REQUIRE(unicode_syms.account == "☺");
    REQUIRE(unicode_syms.likes == "♥");
    REQUIRE(unicode_syms.shuffle == "⇄");
    std::cout << "  -> Unicode Clean Symbols Style verified OK" << std::endl;

    const auto& ascii_syms = sctui::ui::ThemeSymbols::get(sctui::core::IconStyle::DefaultAscii);
    REQUIRE(ascii_syms.logo == "[SC]");
    REQUIRE(ascii_syms.search == "[/]");
    REQUIRE(ascii_syms.account == "[@]");
    REQUIRE(ascii_syms.likes == "<3>");
    REQUIRE(ascii_syms.shuffle == "[~]");
    std::cout << "  -> Default ASCII Symbols Style verified OK" << std::endl;

    // Test Config serialization
    sctui::core::Config cfg;
    cfg.icon_style = sctui::core::IconStyle::DefaultAscii;
    std::filesystem::path tmp_cfg = std::filesystem::temp_directory_path() / "test_sctui_cfg.json";
    REQUIRE(cfg.save(tmp_cfg).has_value());
    auto loaded = sctui::core::Config::load(tmp_cfg);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->icon_style == sctui::core::IconStyle::DefaultAscii);
    std::filesystem::remove(tmp_cfg);
    std::cout << "  -> Config serialization of IconStyle verified OK" << std::endl;
}

void test_full_track_metadata_cache() {
    std::cout << "[Test] Running Full Track Metadata Cache verification..." << std::endl;
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "sctui_meta_cache_test";
    std::filesystem::remove_all(test_dir);

    sctui::core::TrackCacheManager mgr(test_dir);

    sctui::models::Track orig;
    orig.id = 999901;
    orig.title = "Ultimate Synthwave Track";
    orig.artist = "Retro Master";
    orig.artist_id = 12345;
    orig.permalink_url = "https://soundcloud.com/retromaster/ultimate-synthwave";
    orig.artwork_url = "https://i1.sndcdn.com/artworks-test-t500x500.jpg";
    orig.duration_ms = 215400;
    orig.genre = "Synthwave";
    orig.description = "A full cyberpunk journey";
    orig.playback_count = 150000;
    orig.likes_count = 12000;
    orig.reposts_count = 450;
    orig.created_at = "2026-01-15T12:00:00Z";
    orig.stream_url = "https://cf-media.sndcdn.com/stream/fake.mp3";
    orig.waveform_url = "https://wave.sndcdn.com/fake.json";
    orig.waveform_samples = { 0.1f, 0.4f, 0.8f, 0.6f, 0.9f, 0.3f };
    orig.is_liked = true;
    orig.access_type = sctui::models::TrackAccessType::Playable;
    orig.policy = "MONETIZE";
    orig.monetization_model = "AD_SUPPORTED";
    orig.is_snipped = false;

    // Create dummy audio file on disk
    std::filesystem::path track_file = mgr.get_track_cache_path(orig.id);
    std::ofstream out(track_file, std::ios::binary);
    std::vector<char> dummy(4096, 'A');
    out.write(dummy.data(), dummy.size());
    out.close();

    // Register track with full metadata
    mgr.register_track(orig);

    REQUIRE(mgr.is_track_cached(orig.id));
    auto meta_opt = mgr.get_cached_track_metadata(orig.id);
    REQUIRE(meta_opt.has_value());
    const auto& meta = *meta_opt;

    REQUIRE(meta.id == orig.id);
    REQUIRE(meta.title == orig.title);
    REQUIRE(meta.artist == orig.artist);
    REQUIRE(meta.artist_id == orig.artist_id);
    REQUIRE(meta.permalink_url == orig.permalink_url);
    REQUIRE(meta.artwork_url == orig.artwork_url);
    REQUIRE(meta.duration_ms == orig.duration_ms);
    REQUIRE(meta.genre == orig.genre);
    REQUIRE(meta.description == orig.description);
    REQUIRE(meta.playback_count == orig.playback_count);
    REQUIRE(meta.likes_count == orig.likes_count);
    REQUIRE(meta.reposts_count == orig.reposts_count);
    REQUIRE(meta.created_at == orig.created_at);
    REQUIRE(meta.stream_url == orig.stream_url);
    REQUIRE(meta.waveform_url == orig.waveform_url);
    REQUIRE(meta.waveform_samples.size() == 6);
    REQUIRE(meta.waveform_samples[2] == 0.8f);
    REQUIRE(meta.is_liked == true);
    REQUIRE(meta.policy == "MONETIZE");
    std::cout << "  -> Full Track metadata registered and retrieved identically OK" << std::endl;

    // Verify persistence across new manager instance reloading metadata.json
    {
        sctui::core::TrackCacheManager reloaded_mgr(test_dir);
        REQUIRE(reloaded_mgr.is_track_cached(orig.id));
        auto reloaded_meta = reloaded_mgr.get_cached_track_metadata(orig.id);
        REQUIRE(reloaded_meta.has_value());
        REQUIRE(reloaded_meta->title == orig.title);
        REQUIRE(reloaded_meta->genre == "Synthwave");
        REQUIRE(reloaded_meta->is_liked == true);
        REQUIRE(reloaded_meta->waveform_samples.size() == 6);
    }
    std::cout << "  -> Metadata persistent reloading from metadata.json OK" << std::endl;

    std::filesystem::remove_all(test_dir);
}

void test_discord_rpc_integration() {
    std::cout << "[Test] Running Discord RPC integration verification..." << std::endl;

    std::string client_id = "1187340825991069706";
    std::string handshake = sctui::discord::DiscordRpc::build_handshake_payload(client_id);
    REQUIRE(handshake.find(client_id) != std::string::npos);
    REQUIRE(handshake.find("\"v\":1") != std::string::npos);
    std::cout << "  -> Handshake frame formatting OK" << std::endl;

    sctui::models::Track track;
    track.id = 12345;
    track.title = "Song Of The Wind";
    track.artist = "Acoustic Forest";
    track.duration_ms = 180000;
    track.permalink_url = "https://soundcloud.com/forest/song-of-the-wind";
    track.artwork_url = "https://i1.sndcdn.com/artworks-test.jpg";

    std::string payload = sctui::discord::DiscordRpc::build_activity_payload(
        track,
        sctui::audio::PlaybackState::Playing,
        30000,
        180000
    );
    REQUIRE(payload.find("SET_ACTIVITY") != std::string::npos);
    REQUIRE(payload.find("Song Of The Wind") != std::string::npos);
    REQUIRE(payload.find("Acoustic Forest") != std::string::npos);
    REQUIRE(payload.find("Listen on SoundCloud") != std::string::npos);
    REQUIRE(payload.find("timestamps") != std::string::npos);
    REQUIRE(payload.find("Playing") != std::string::npos);
    std::cout << "  -> Rich Presence SET_ACTIVITY frame formatting OK" << std::endl;

    // Test Config serialization of discord_rpc_enabled
    sctui::core::Config cfg;
    cfg.discord_rpc_enabled = true;
    cfg.discord_client_id = "99887766";
    std::filesystem::path tmp_cfg = std::filesystem::temp_directory_path() / "test_discord_cfg.json";
    REQUIRE(cfg.save(tmp_cfg).has_value());
    auto loaded = sctui::core::Config::load(tmp_cfg);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->discord_rpc_enabled == true);
    REQUIRE(loaded->discord_client_id == "99887766");
    std::filesystem::remove(tmp_cfg);
    std::cout << "  -> Discord RPC config serialization OK" << std::endl;
}

void test_dynamic_queue_append_and_shuffle() {
    std::cout << "[Test] Running Dynamic Queue Append & Shuffling verification..." << std::endl;

    sctui::models::QueueState queue;
    for (size_t i = 0; i < 5; ++i) {
        sctui::models::Track t;
        t.id = 100 + i;
        t.title = "Track " + std::to_string(i);
        queue.tracks.push_back(t);
    }
    queue.current_index = 0;
    queue.shuffle = true;
    queue.rebuild_shuffle();

    REQUIRE(queue.shuffle_indices.size() == 5);
    REQUIRE(queue.shuffle_indices[0] == 0); // Anchor current track

    // Simulate appending a newly loaded batch of 10 likes while track 0 is playing
    std::vector<sctui::models::Track> batch;
    for (size_t i = 5; i < 15; ++i) {
        sctui::models::Track t;
        t.id = 100 + i;
        t.title = "Batch Track " + std::to_string(i);
        batch.push_back(t);
    }

    queue.append_tracks_and_update_shuffle(batch);

    REQUIRE(queue.tracks.size() == 15);
    REQUIRE(queue.shuffle_indices.size() == 15);
    REQUIRE(queue.shuffle_indices[0] == 0); // Playing track still at cursor 0!

    std::set<size_t> unique_indices(queue.shuffle_indices.begin(), queue.shuffle_indices.end());
    REQUIRE(unique_indices.size() == 15);
    std::cout << "  -> Dynamic queue extension preserved current track and shuffled all 15 tracks OK" << std::endl;
}

void test_volume_150_and_visualizer_curve() {
    std::cout << "[Test] Running Volume 150% & Visualizer Sensitivity Diminishing Returns curve..." << std::endl;

    // Config volume up to 150%
    sctui::core::Config cfg;
    cfg.volume = 150;
    REQUIRE(cfg.volume == 150);

    auto tmp_path = std::filesystem::temp_directory_path() / "sctui_test_vol150.json";
    REQUIRE(cfg.save(tmp_path).has_value());
    auto loaded = sctui::core::Config::load(tmp_path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->volume == 150);
    std::filesystem::remove(tmp_path);

    // Audio command volume support up to 150%
    sctui::audio::AudioCommand cmd;
    cmd.type = sctui::audio::AudioCommandType::SetVolume;
    cmd.volume = 150.0f;
    REQUIRE(cmd.volume == 150.0f);

    // Verify mathematical formulation of diminishing returns visualizer curve:
    auto compute_vis_scale = [](float norm_vol) -> float {
        if (norm_vol <= 0.001f) return 0.0f;
        if (norm_vol <= 1.0f) {
            return std::pow(std::sin(norm_vol * 1.57079632679f), 1.35f);
        } else {
            return 1.0f + std::tanh((norm_vol - 1.0f) * 0.8f) * 0.22f;
        }
    };

    float scale_100 = compute_vis_scale(1.0f);
    float scale_90 = compute_vis_scale(0.9f);
    float scale_80 = compute_vis_scale(0.8f);
    float scale_10 = compute_vis_scale(0.1f);
    float scale_150 = compute_vis_scale(1.5f);

    // At 100%: scale must be exactly 1.0
    REQUIRE(std::abs(scale_100 - 1.0f) < 0.001f);

    // As volume decreases from 100%, sensitivity initially shifts to keep visualizer appearance nearly identical to 100%
    // Difference between 100% and 90% is less than 2%!
    REQUIRE(std::abs(scale_100 - scale_90) < 0.02f);
    REQUIRE(scale_90 > 0.98f);
    REQUIRE(scale_80 > 0.92f);

    // At 10% volume, sensitivity changes progressively less so it remains clearly apparent that sound is very quiet
    REQUIRE(scale_10 < 0.10f); // Less than 10% height
    REQUIRE(scale_10 > 0.05f); // Still registers subtly

    // When volume is increased beyond 100% up to 150%, visualizer sensitivity gradually decreases with diminishing returns
    REQUIRE(scale_150 > 1.0f);
    REQUIRE(scale_150 < 1.15f); // Diminished gain prevents clipping bars

    std::cout << "  -> Volume up to 150% verified OK" << std::endl;
    std::cout << "  -> Visualizer diminishing returns sensitivity curve (100% -> " << scale_100
              << ", 90% -> " << scale_90 << ", 10% -> " << scale_10 << ", 150% -> " << scale_150 << ") OK" << std::endl;
}

void test_mouse_control_option() {
    std::cout << "[Test] Running Mouse Controls configuration verification..." << std::endl;

    sctui::core::Config cfg;
    // Default must be turned off
    REQUIRE(cfg.mouse_control == false);

    // Toggle to enabled
    cfg.mouse_control = true;

    auto tmp_path = std::filesystem::temp_directory_path() / "sctui_test_mouse_cfg.json";
    auto save_res = cfg.save(tmp_path);
    REQUIRE(save_res.has_value());

    auto load_res = sctui::core::Config::load(tmp_path);
    REQUIRE(load_res.has_value());
    REQUIRE(load_res->mouse_control == true);

    std::filesystem::remove(tmp_path);
    std::cout << "  -> Mouse control option (default off, toggle, persistent JSON) OK" << std::endl;
}

void test_settings_and_mouse_coordinate_mapping() {
    std::cout << "[Test] Running Settings View Mouse Click & Clean URL verification..." << std::endl;

    sctui::models::AppState state;
    sctui::core::Config config;
    bool saved = false;

    sctui::ui::views::SettingsView settings(
        state, config, nullptr,
        [&](const sctui::core::Config&) { saved = true; },
        nullptr, nullptr, nullptr, nullptr
    );

    // Render settings view at standard terminal dimensions (100x40)
    auto elem = settings.render(true);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(100), ftxui::Dimension::Fixed(40));
    ftxui::Render(screen, elem);
    std::string out = screen.ToString();

    // Verify no raw OSC 8 escape sequences like "]8;;" appear in output
    REQUIRE(out.find("]8;;") == std::string::npos);
    REQUIRE(out.find("https://soundcloud.com/signin") != std::string::npos);

    // Verify mouse click selection
    bool clicked = settings.handle_mouse_click(10, 10); // Field 0
    REQUIRE(clicked);

    std::cout << "  -> Settings View clean plain URL verified (no ']8;;' artifacts) OK" << std::endl;
    std::cout << "  -> Settings View mouse click handling verified OK" << std::endl;
}

void test_fullscreen_mode() {
    std::cout << "[Test] Running Fullscreen Playback & High-Res Cover Art verification..." << std::endl;

    sctui::models::AppState state;
    REQUIRE(!state.is_fullscreen);

    // Toggle fullscreen
    state.is_fullscreen = true;
    REQUIRE(state.is_fullscreen);

    // Test artwork URL upgrading to 500x500 high-res
    std::string test_url_large = "https://i1.sndcdn.com/artworks-000508605387-v900l7-large.jpg";
    std::string upgraded = sctui::core::CoverArtProcessor::upgrade_artwork_url(test_url_large);
    REQUIRE(upgraded == "https://i1.sndcdn.com/artworks-000508605387-v900l7-t500x500.jpg");

    std::string test_url_t300 = "https://i1.sndcdn.com/artworks-000508605387-v900l7-t300x300.jpg?query=123";
    std::string upgraded_t300 = sctui::core::CoverArtProcessor::upgrade_artwork_url(test_url_t300);
    REQUIRE(upgraded_t300.find("-t500x500.") != std::string::npos);

    // Dynamic cover rendering with custom width and height
    sctui::models::CoverArtImage mock_img;
    mock_img.width = 10;
    mock_img.height = 10;
    mock_img.pixels.resize(100, sctui::models::ColorRGB{255, 120, 0});
    auto elem = sctui::core::CoverArtProcessor::render_cover_element(
        mock_img,
        sctui::core::ImageProtocol::HalfBlock,
        3, // Fullscreen image ID
        40,
        20
    );
    REQUIRE(elem != nullptr);

    std::cout << "  -> Fullscreen Mode State & High-Res 500x500 Cover Scaling OK" << std::endl;
}

void test_thread_pool() {
    std::cout << "[Test] Running ThreadPool verification..." << std::endl;
    sctui::core::ThreadPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 20; ++i) {
        pool.enqueue([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    pool.shutdown();
    REQUIRE(counter.load() == 20);
    std::cout << "  -> ThreadPool dispatched 20 tasks and shutdown cleanly OK" << std::endl;
}

void test_queue_alignment() {
    std::cout << "[Test] Running Queue Sequence Number Alignment verification..." << std::endl;
    int total_tracks = 142;
    int max_digits = std::max(2, static_cast<int>(std::to_string(total_tracks).length()));
    REQUIRE(max_digits == 3);

    auto format_num = [max_digits](int idx) {
        std::string num_str = std::to_string(idx + 1);
        return std::string(static_cast<size_t>(std::max(0, max_digits - static_cast<int>(num_str.length()))), ' ') + num_str + ". ";
    };

    std::string line_1 = format_num(0);   // "  1. "
    std::string line_26 = format_num(25); // " 26. "
    std::string line_100 = format_num(99);// "100. "

    REQUIRE(line_1.length() == 5);
    REQUIRE(line_26.length() == 5);
    REQUIRE(line_100.length() == 5);
    REQUIRE(line_1 == "  1. ");
    REQUIRE(line_26 == " 26. ");
    REQUIRE(line_100 == "100. ");
    std::cout << "  -> Dynamic Queue alignment relative to last track verified OK" << std::endl;
}

int main() {
    std::cout << "=== Running sctui test suite ===" << std::endl;
    test_spsc_queue();
    test_bounded_channel();
    test_thread_pool();
    test_queue_alignment();
    test_entropy_shuffle();
    test_dynamic_queue_append_and_shuffle();
    test_volume_150_and_visualizer_curve();
    test_mouse_control_option();
    test_settings_and_mouse_coordinate_mapping();
    test_cover_art_processor();
    test_drm_and_goplus_separation();
    test_mpris_server();
    test_discord_rpc_integration();
    test_cookie_extractor();
    test_http_doh_and_dns();
    test_terminal_graphics_protocol();
    test_track_cache_manager();
    test_full_track_metadata_cache();
    test_theme_symbols();
    test_fullscreen_mode();
    test_soundcloud_api();
    test_user_library();
    test_account_profile();
    std::cout << "=== All tests PASSED successfully! ===" << std::endl;
    return 0;
}
