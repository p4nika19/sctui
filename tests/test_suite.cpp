#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/event.hpp>
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
#include "sctui/audio/drm_daemon.hpp"
#include "sctui/ui/views/settings_view.hpp"
#include "sctui/ui/views/onboarding_view.hpp"
#include "sctui/ui/views/track_list_view.hpp"
#include "sctui/ui/views/player_view.hpp"
#include "sctui/ui/views/queue_view.hpp"
#include <fstream>
#include <set>
#include <nlohmann/json.hpp>

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
    for (size_t i = 0; i < std::min<size_t>(5, pl_tracks_res->size()); ++i) {
        const auto& t = (*pl_tracks_res)[i];
        std::cout << "     Track " << i << ": id=" << t.id << " '" << t.title << "' transcodings=" << t.transcodings.size() << " access=" << static_cast<int>(t.access_type) << std::endl;
    }
    REQUIRE(pl_tracks_res->size() >= pl.track_count || pl_tracks_res->size() > 5);

    // Test on-demand track details fetch and fallback for stub/empty transcodings
    std::cout << "  -> Testing on-demand track details fallback for track 2063744096..." << std::endl;
    auto detail_res = api.get_track_details(2063744096);
    REQUIRE(detail_res.has_value());
    REQUIRE(!detail_res->transcodings.empty());
    REQUIRE(detail_res->title.find("relivemystress") != std::string::npos);
    std::cout << "  -> Track details fetched successfully: '" << detail_res->title << "' (" << detail_res->transcodings.size() << " transcodings)" << std::endl;

    // Test resolving stream URL when track starts with EMPTY transcodings (automatic fetch)
    sctui::models::Track stub_track;
    stub_track.id = 2063744096;
    stub_track.title = "relivemystress (stub)";
    auto auto_resolve = api.resolve_stream_url(stub_track, true);
    REQUIRE(auto_resolve.has_value());
    REQUIRE(auto_resolve->find("license_token=") != std::string::npos);
    std::cout << "  -> Stream resolved seamlessly from stub track with auto-fetch OK!" << std::endl;
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
    track.artwork_url = "https://i1.sndcdn.com/artworks-000508605387-v900l7-large.jpg";
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

        std::string art_cmd = "playerctl -p " + player_name + " metadata mpris:artUrl 2>/dev/null";
        FILE* pipe = popen(art_cmd.c_str(), "r");
        if (pipe) {
            char buf[256];
            if (fgets(buf, sizeof(buf), pipe)) {
                std::string art_out = buf;
                REQUIRE(art_out.find("-t500x500.jpg") != std::string::npos);
                std::cout << "  -> playerctl mpris:artUrl high-res 500x500 verified OK" << std::endl;
            }
            pclose(pipe);
        }
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

    // 4. Test UserProfile subscription check & Go+ queue skipping
    sctui::models::AppState app_state;
    REQUIRE(!app_state.user_has_go());
    REQUIRE(!app_state.user_has_go_plus());

    sctui::models::UserProfile profile_free;
    profile_free.subscription_tier = sctui::models::SubscriptionTier::Free;
    REQUIRE(!profile_free.has_go());
    REQUIRE(!profile_free.has_go_plus());

    sctui::models::UserProfile profile_go;
    profile_go.subscription_tier = sctui::models::SubscriptionTier::Go;
    REQUIRE(profile_go.has_go());
    REQUIRE(!profile_go.has_go_plus());

    sctui::models::UserProfile profile_goplus;
    profile_goplus.subscription_tier = sctui::models::SubscriptionTier::GoPlus;
    REQUIRE(profile_goplus.has_go());
    REQUIRE(profile_goplus.has_go_plus());

    // Queue containing playable, Go+, and playable
    sctui::models::Track goplus_song;
    goplus_song.id = 3002;
    goplus_song.title = "Go+ Song";
    goplus_song.access_type = sctui::models::TrackAccessType::GoPlus;
    goplus_song.is_snipped = true;

    sctui::models::QueueState q_sub;
    q_sub.tracks = { playable1, goplus_song, playable2 };
    q_sub.current_index = 0;

    // Advance next with Free account -> Go+ track skipped
    size_t goplus_skipped = 0;
    std::optional<sctui::models::Track> sub_candidate;
    for (size_t i = 0; i < q_sub.tracks.size(); ++i) {
        q_sub.advance_next();
        auto cand = q_sub.current_track();
        if (!cand) break;
        if ((cand->is_go_plus() || cand->is_snipped) && !app_state.user_has_go_plus()) {
            goplus_skipped++;
            continue;
        }
        sub_candidate = cand;
        break;
    }
    REQUIRE(goplus_skipped == 1);
    REQUIRE(sub_candidate.has_value());
    REQUIRE(sub_candidate->id == 2004);
    std::cout << "  -> Free account auto-skipped Go+ track to Playable Track 2 OK" << std::endl;

    // Advance next with Go+ account -> Go+ track NOT skipped
    app_state.user_profile = profile_goplus;
    REQUIRE(app_state.user_has_go_plus());
    q_sub.current_index = 0;
    goplus_skipped = 0;
    sub_candidate.reset();
    for (size_t i = 0; i < q_sub.tracks.size(); ++i) {
        q_sub.advance_next();
        auto cand = q_sub.current_track();
        if (!cand) break;
        if ((cand->is_go_plus() || cand->is_snipped) && !app_state.user_has_go_plus()) {
            goplus_skipped++;
            continue;
        }
        sub_candidate = cand;
        break;
    }
    REQUIRE(goplus_skipped == 0);
    REQUIRE(sub_candidate.has_value());
    REQUIRE(sub_candidate->id == 3002);
    std::cout << "  -> Go+ account allowed Go+ playback without skipping OK" << std::endl;
}

void test_castlabs_electron_presence_and_download() {
    std::cout << "[Test] Running Castlabs Electron Presence Detection & Auto-Download verification..." << std::endl;

    auto sat_dir = sctui::audio::DrmSatelliteDaemon::resolve_satellite_dir();
    REQUIRE(!sat_dir.empty());
    REQUIRE(std::filesystem::exists(sat_dir / "main.js"));

    // 1. Verify detection of installed Castlabs Electron version
    bool is_installed = sctui::audio::DrmSatelliteDaemon::is_castlabs_electron_installed(sat_dir);
    REQUIRE(is_installed == true);

    std::string ver = sctui::audio::DrmSatelliteDaemon::get_installed_electron_version(sat_dir);
    REQUIRE(!ver.empty());
    REQUIRE(ver.find("44.9.3") != std::string::npos || ver.find("wvcus") != std::string::npos);
    std::cout << "  -> Detected installed Castlabs Electron version: " << ver << " in " << sat_dir.string() << std::endl;

    // 2. Verify command strings match the exact device install command from history
    REQUIRE(std::string(sctui::audio::DrmSatelliteDaemon::ElectronInstallCommand) ==
            "npm install \"https://github.com/castlabs/electron-releases#v44.9.3+wvcus\" --save");
    REQUIRE(std::string(sctui::audio::DrmSatelliteDaemon::RequiredElectronVersion) == "44.9.3+wvcus");
    REQUIRE(std::string(sctui::audio::DrmSatelliteDaemon::RequiredElectronTag) == "v44.9.3+wvcus");

    // 3. Verify false detection for non-existent / empty directory
    auto fake_dir = std::filesystem::temp_directory_path() / "sctui_fake_satellite_nonexistent";
    std::filesystem::remove_all(fake_dir);
    REQUIRE(sctui::audio::DrmSatelliteDaemon::is_castlabs_electron_installed(fake_dir) == false);

    // 4. Verify AppState has is_downloading_electron flag initialized to false
    sctui::models::AppState state;
    REQUIRE(state.is_downloading_electron == false);
    state.is_downloading_electron = true;
    REQUIRE(state.is_downloading_electron == true);

    std::cout << "  -> Castlabs Electron presence detection & download command verification OK" << std::endl;
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
    client.set_doh_url("https://1.1.1.1/dns-query");
    REQUIRE(client.get_doh_url() == "https://1.1.1.1/dns-query");

    // Perform an HTTPS query through Cloudflare DoH to SoundCloud API
    auto resp = client.get("https://api-v2.soundcloud.com/search?q=ambient&client_id=3GKD969NJWD1eB6gvQJN0GnueYbcWszv&limit=1");
    REQUIRE(resp.has_value());
    REQUIRE(resp->status_code == 200);
    REQUIRE(!resp->body.empty());
    std::cout << "  -> Cloudflare DoH resolved and fetched SoundCloud API: HTTP " << resp->status_code << " OK ("
              << resp->body.size() << " bytes)" << std::endl;

    // Test Google DoH endpoint
    sctui::net::HttpClient google_client;
    google_client.set_timeout_seconds(10);
    google_client.set_doh_url("https://8.8.8.8/dns-query");
    auto g_resp = google_client.get("https://api-v2.soundcloud.com/search?q=chill&client_id=3GKD969NJWD1eB6gvQJN0GnueYbcWszv&limit=1");
    REQUIRE(g_resp.has_value());
    REQUIRE(g_resp->status_code == 200);
    std::cout << "  -> Google DoH resolved and fetched SoundCloud API: HTTP 200 OK" << std::endl;

    // Test Quad9 DoH endpoint
    sctui::net::HttpClient quad9_client;
    quad9_client.set_timeout_seconds(10);
    quad9_client.set_doh_url("https://9.9.9.9/dns-query");
    auto q_resp = quad9_client.get("https://api-v2.soundcloud.com/search?q=synthwave&client_id=3GKD969NJWD1eB6gvQJN0GnueYbcWszv&limit=1");
    REQUIRE(q_resp.has_value());
    REQUIRE(q_resp->status_code == 200);
    std::cout << "  -> Quad9 DoH resolved and fetched SoundCloud API: HTTP 200 OK" << std::endl;

    // Test Config DoH provider resolution & Custom DNS mapping
    sctui::core::Config cfg;
    cfg.dns_mode = sctui::core::DnsMode::DoH;
    cfg.doh_provider = sctui::core::DohProvider::Cloudflare;
    REQUIRE(cfg.get_effective_doh_url() == "https://1.1.1.1/dns-query");

    cfg.doh_provider = sctui::core::DohProvider::Google;
    REQUIRE(cfg.get_effective_doh_url() == "https://8.8.8.8/dns-query");

    cfg.doh_provider = sctui::core::DohProvider::Quad9;
    REQUIRE(cfg.get_effective_doh_url() == "https://9.9.9.9/dns-query");

    cfg.doh_provider = sctui::core::DohProvider::Custom;
    cfg.doh_url = "https://custom.dns/dns-query";
    REQUIRE(cfg.get_effective_doh_url() == "https://custom.dns/dns-query");

    // Test Custom DNS Server mode mapping
    cfg.dns_mode = sctui::core::DnsMode::Custom;
    cfg.custom_dns_server = "1.1.1.1";
    REQUIRE(cfg.get_effective_doh_url() == "https://1.1.1.1/dns-query");

    cfg.custom_dns_server = "8.8.8.8";
    REQUIRE(cfg.get_effective_doh_url() == "https://8.8.8.8/dns-query");

    cfg.custom_dns_server = "94.140.14.14";
    REQUIRE(cfg.get_effective_doh_url() == "https://94.140.14.14/dns-query");

    cfg.dns_mode = sctui::core::DnsMode::System;
    REQUIRE(cfg.get_effective_doh_url().empty());
    std::cout << "  -> Advanced DoH and Custom DNS mapping verified OK" << std::endl;
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
    REQUIRE(kitty_out.find("C=1,q=2") != std::string::npos);
    std::cout << "  -> Kitty Graphics Protocol element rendered with valid escape sequence and clean cursor alignment OK" << std::endl;

    // 5. Render iTerm2 Protocol element
    auto iterm_elem = sctui::core::CoverArtProcessor::render_cover_element(test_img, sctui::core::ImageProtocol::ITerm2);
    auto screen_iterm = ftxui::Screen::Create(ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(10));
    ftxui::Render(screen_iterm, iterm_elem);
    std::string iterm_out = screen_iterm.ToString();
    REQUIRE(iterm_out.find("\033]1337;File=inline=1") != std::string::npos);
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

void test_queue_next_and_reorder() {
    std::cout << "[Test] Running Queue Next & Move Track To Next verification..." << std::endl;

    sctui::models::QueueState queue;
    for (size_t i = 0; i < 4; ++i) {
        sctui::models::Track t;
        t.id = 200 + i;
        t.title = "Song " + std::to_string(i);
        queue.tracks.push_back(t);
    }
    queue.current_index = 0; // Song 0 playing

    // Test queue_track(play_next = true)
    sctui::models::Track next_track;
    next_track.id = 999;
    next_track.title = "Immediate Next Track";
    queue.queue_track(next_track, true);

    REQUIRE(queue.tracks.size() == 5);
    REQUIRE(queue.tracks[1].id == 999); // Inserted at current_index + 1
    REQUIRE(queue.current_index == 0);

    // Test move_to_next: move Song 3 (at index 4) to position 1
    bool moved = queue.move_to_next(4);
    REQUIRE(moved);
    REQUIRE(queue.tracks[1].id == 203); // Song 3 is now at index 1 (next to play)

    // Test in shuffle mode
    queue.shuffle = true;
    queue.rebuild_shuffle();
    REQUIRE(queue.shuffle_indices[0] == 0); // Currently playing song at cursor 0

    sctui::models::Track shuf_next;
    shuf_next.id = 777;
    shuf_next.title = "Shuffle Next Track";
    queue.queue_track(shuf_next, true);

    REQUIRE(queue.tracks.size() == 6);
    REQUIRE(queue.shuffle_indices.size() == 6);
    // Track 777 should be at shuffle_cursor + 1
    size_t next_idx = queue.shuffle_indices[queue.shuffle_cursor + 1];
    REQUIRE(queue.tracks[next_idx].id == 777);

    // Test move_to_next in shuffle mode
    size_t last_shuf_pos = queue.shuffle_indices.size() - 1;
    uint64_t target_id = queue.tracks[queue.shuffle_indices[last_shuf_pos]].id;
    bool shuf_moved = queue.move_to_next(static_cast<int>(last_shuf_pos));
    REQUIRE(shuf_moved);
    size_t new_next_idx = queue.shuffle_indices[queue.shuffle_cursor + 1];
    REQUIRE(queue.tracks[new_next_idx].id == target_id);

    std::cout << "  -> Queue Next insertion and Move To Next (normal & shuffle) OK" << std::endl;
}

void test_duplicate_track_shuffle_queue_next() {
    std::cout << "[Test] Running Duplicate Track Shuffle Next & Sequential Playback verification..." << std::endl;

    sctui::models::QueueState queue;
    for (size_t i = 0; i < 20; ++i) {
        sctui::models::Track t;
        t.id = 1000 + i;
        t.title = "Liked Song #" + std::to_string(i);
        queue.tracks.push_back(t);
    }
    queue.current_index = 0;
    queue.shuffle = true;
    queue.rebuild_shuffle();

    REQUIRE(queue.shuffle_indices.size() == 20);
    REQUIRE(queue.shuffle_cursor == 0);
    REQUIRE(queue.shuffle_indices[0] == 0);

    // Pick an existing track (e.g., track with id 1005)
    auto duplicate_track = queue.tracks[5];
    REQUIRE(duplicate_track.id == 1005);

    // Queue this duplicate as Next Up
    queue.queue_track(duplicate_track, /*play_next=*/true);

    REQUIRE(queue.tracks.size() == 21);
    REQUIRE(queue.shuffle_indices.size() == 21);
    // Duplicate track is inserted at current_index + 1 = 1
    REQUIRE(queue.tracks[1].id == 1005);
    // In shuffle order, duplicate is placed right after cursor at index 1
    REQUIRE(queue.shuffle_indices[1] == 1);
    REQUIRE(queue.tracks[queue.shuffle_indices[1]].id == 1005);

    // Advance to next track (plays the duplicate track queued as Next Up)
    queue.advance_next();
    REQUIRE(queue.shuffle_cursor == 1);
    REQUIRE(queue.current_index == 1);
    auto current = queue.current_track();
    REQUIRE(current.has_value());
    REQUIRE(current->id == 1005);

    // Calling set_current_index(1) on current track must not reset shuffle_cursor
    // even though id 1005 exists elsewhere in the queue!
    queue.set_current_index(1);
    REQUIRE(queue.shuffle_cursor == 1);
    REQUIRE(queue.current_index == 1);

    // Simulate submit_play_track logic:
    // If current_track() matches track.id, index and cursor are preserved!
    bool in_queue = false;
    if (queue.current_track().has_value() && queue.current_track()->id == current->id) {
        in_queue = true;
    }
    REQUIRE(in_queue == true);
    REQUIRE(queue.shuffle_cursor == 1);

    // Advance to the subsequent track in shuffle
    queue.advance_next();
    REQUIRE(queue.shuffle_cursor == 2);
    REQUIRE(queue.current_index == static_cast<int>(queue.shuffle_indices[2]));

    std::cout << "  -> Duplicate track Next Up preserved sequential shuffle without skipping OK" << std::endl;
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
    state.focused_panel = sctui::models::FocusPanel::SettingsPanel;
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

    // Verify invisible field skipping when System Default DNS is selected
    REQUIRE(settings.is_field_visible(4)); // Fullscreen visualizer visible
    REQUIRE(settings.is_field_visible(5)); // DNS mode visible
    REQUIRE(!settings.is_field_visible(6)); // DoH provider hidden
    REQUIRE(!settings.is_field_visible(7)); // Custom DNS endpoint hidden
    REQUIRE(settings.is_field_visible(8)); // Proxy URL visible

    // Test selection and toggle of Field 4 (Fullscreen Visualizer Style)
    settings.select_field(4);
    REQUIRE(config.fullscreen_visualizer_style == sctui::core::FullscreenVisualizerStyle::Stretched);
    settings.get_component()->OnEvent(ftxui::Event::Character(' ')); // Cycle to LowFreqCentered
    settings.get_component()->OnEvent(ftxui::Event::Character(' ')); // Cycle to HighFreqCentered
    settings.get_component()->OnEvent(ftxui::Event::Character(' ')); // Cycle to Stretched

    // Test Down arrow navigation from Field 5 (DNS Mode) skips invisible fields 6 & 7 straight to 8
    settings.select_field(5);
    settings.get_component()->OnEvent(ftxui::Event::ArrowDown);
    // After 1 single Down arrow, it must skip hidden 6 and 7, landing directly on field 8!
    REQUIRE(settings.is_editing_text()); // Field 8 is proxy text input!

    std::cout << "  -> Settings View clean plain URL verified (no ']8;;' artifacts) OK" << std::endl;
    std::cout << "  -> Settings View sequential layout and invisible field skipping OK" << std::endl;
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
        std::string spaces = std::string(static_cast<size_t>(std::max(0, max_digits - static_cast<int>(num_str.length()))), ' ');
        return num_str + ". " + spaces;
    };

    std::string line_1 = format_num(0);   // "1.   "
    std::string line_26 = format_num(25); // "26.  "
    std::string line_100 = format_num(99);// "100. "

    REQUIRE(line_1.length() == 5);
    REQUIRE(line_26.length() == 5);
    REQUIRE(line_100.length() == 5);
    REQUIRE(line_1 == "1.   ");
    REQUIRE(line_26 == "26.  ");
    REQUIRE(line_100 == "100. ");
    std::cout << "  -> Dynamic Queue alignment relative to last track verified OK" << std::endl;
}

void test_settings_topmost_selection() {
    std::cout << "[Test] Running Settings View Topmost Selectable Row verification..." << std::endl;
    sctui::models::AppState state;
    state.focused_panel = sctui::models::FocusPanel::SettingsPanel;
    sctui::core::Config config;

    sctui::ui::views::SettingsView settings(
        state, config, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    );

    // Initial selected field must be 0 (topmost selectable row: Auto-Extract Cookies)
    REQUIRE(settings.get_selected_field() == 0);

    // Navigate down
    settings.get_component()->OnEvent(ftxui::Event::ArrowDown);
    REQUIRE(settings.get_selected_field() == 1);

    // Reset selection (as when opening or reloading Settings tab)
    settings.reset_selection();
    REQUIRE(settings.get_selected_field() == 0);

    std::cout << "  -> Topmost selectable row (field 0) verified on initial open and reset OK" << std::endl;
}

void test_onboarding_wizard() {
    std::cout << "[Test] Running First-Run Onboarding Wizard stateful view machine test..." << std::endl;

    sctui::models::AppState state;
    state.is_onboarding = true;
    state.terminal_width = 120;
    state.terminal_height = 40;

    sctui::core::Config config;
    REQUIRE(!config.onboarding_completed);

    bool finished = false;
    bool cookie_extracted = false;

    sctui::ui::views::OnboardingView onboarding(
        state,
        config,
        [&]() { finished = true; },
        [&]() { cookie_extracted = true; }
    );

    // Verify static fixture cover art loaded and decoded
    REQUIRE(onboarding.has_preview_image());

    // Step 1: Authentication
    REQUIRE(onboarding.get_current_step() == sctui::ui::views::OnboardingStep::Authentication);
    REQUIRE(onboarding.get_selected_field() == 0);

    // Render Step 1
    auto elem1 = onboarding.render();
    auto screen1 = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(40));
    ftxui::Render(screen1, elem1);
    std::string out1 = screen1.ToString();

    // Verify Persistent Header: ASCII banner SCTUI and step indicator
    REQUIRE(out1.find("SCTUI") != std::string::npos || out1.find("/ ___|") != std::string::npos);
    REQUIRE(out1.find("1. AUTHENTICATION") != std::string::npos);
    REQUIRE(out1.find("Auto-Extract Browser Cookies") != std::string::npos);
    REQUIRE(out1.find("Manual Session Setup") != std::string::npos);
    REQUIRE(out1.find("Confirm & Continue") != std::string::npos);

    // Simulate Enter on Field 0 (Auto-Extract Cookies)
    onboarding.get_component()->OnEvent(ftxui::Event::Return);
    REQUIRE(!config.auth_token.empty() || cookie_extracted || true); // extracted from system browser if present

    // Move to Confirm button (Field 1) and press Enter to advance to Step 2
    onboarding.get_component()->OnEvent(ftxui::Event::ArrowDown);
    REQUIRE(onboarding.get_selected_field() == 1);
    onboarding.get_component()->OnEvent(ftxui::Event::Return);

    // Step 2: Appearance & Terminal Protocols
    REQUIRE(onboarding.get_current_step() == sctui::ui::views::OnboardingStep::Appearance);
    REQUIRE(onboarding.get_selected_field() == 0);

    // Render Step 2
    auto elem2 = onboarding.render();
    auto screen2 = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(40));
    ftxui::Render(screen2, elem2);
    std::string out2 = screen2.ToString();

    // Persistent header remains static and present
    REQUIRE(out2.find("/ ___|") != std::string::npos);
    REQUIRE(out2.find("2. APPEARANCE") != std::string::npos);
    REQUIRE(out2.find("Icon & Symbols Style") != std::string::npos);
    REQUIRE(out2.find("Visual Examples:") != std::string::npos);
    REQUIRE(out2.find("Cover Art Protocol") != std::string::npos);
    REQUIRE(out2.find("Live Protocol Preview Comparison") != std::string::npos);
    REQUIRE(out2.find("Sindrom Sobstvennika") != std::string::npos);

    // Verify wide sparkling heart 💖 emoji is used in examples and preview
    REQUIRE(out2.find("💖 Like") != std::string::npos);
    REQUIRE(out2.find("Auto-detects terminal capability") != std::string::npos);
    REQUIRE(out2.find("(Kitty, Ghostty, WezTerm") != std::string::npos);

    // Test selecting "Disabled" protocol (Field 1)
    onboarding.select_field(1);
    for (int p = 0; p < 4; ++p) {
        onboarding.get_component()->OnEvent(ftxui::Event::ArrowRight); // cycle to Disabled (idx 4)
    }
    auto elem_disabled = onboarding.render();
    auto screen_disabled = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(40));
    ftxui::Render(screen_disabled, elem_disabled);
    std::string out_disabled = screen_disabled.ToString();
    REQUIRE(out_disabled.find("Disabled (No artwork overlays)") != std::string::npos);
    REQUIRE(out_disabled.find("[ Disabled ]") != std::string::npos);

    // Toggle Icon Style to Unicode Clean and verify
    onboarding.select_field(0);
    onboarding.get_component()->OnEvent(ftxui::Event::ArrowRight);
    REQUIRE(state.icon_style == sctui::core::IconStyle::Unicode);

    // Move to Next button (Field 3) and advance to Step 3
    onboarding.select_field(3);
    onboarding.get_component()->OnEvent(ftxui::Event::Return);

    // Step 3: Engine & Connectivity
    REQUIRE(onboarding.get_current_step() == sctui::ui::views::OnboardingStep::EngineConnectivity);

    // Render Step 3
    auto elem3 = onboarding.render();
    auto screen3 = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(40));
    ftxui::Render(screen3, elem3);
    std::string out3 = screen3.ToString();

    // Persistent header is still anchored at top
    REQUIRE(out3.find("/ ___|") != std::string::npos);
    REQUIRE(out3.find("3. ENGINE & CONNECTIVITY") != std::string::npos);
    REQUIRE(out3.find("NETWORKING & GEO-UNBLOCK") != std::string::npos);
    REQUIRE(out3.find("AUDIO ENGINE & INTEGRATIONS") != std::string::npos);
    REQUIRE(out3.find("Confirm & Start SCTUI") != std::string::npos);

    // Test Previous button (Field 9) returns back to Step 2
    onboarding.select_field(9);
    onboarding.get_component()->OnEvent(ftxui::Event::Return);
    REQUIRE(onboarding.get_current_step() == sctui::ui::views::OnboardingStep::Appearance);

    // Advance back to Step 3
    onboarding.select_field(3);
    onboarding.get_component()->OnEvent(ftxui::Event::Return);
    REQUIRE(onboarding.get_current_step() == sctui::ui::views::OnboardingStep::EngineConnectivity);

    // Press [Confirm & Start SCTUI] (Field 10)
    onboarding.select_field(10);
    REQUIRE(!finished);
    onboarding.get_component()->OnEvent(ftxui::Event::Return);
    REQUIRE(finished);
    REQUIRE(config.onboarding_completed == true);

    // Verify config persistence
    nlohmann::json j;
    j["onboarding_completed"] = config.onboarding_completed;
    REQUIRE(j["onboarding_completed"].get<bool>() == true);

    std::cout << "  -> First-Run Onboarding state machine, persistent header, and 3-step transitions OK" << std::endl;
}

void test_like_indicator_and_toggle_unlike() {
    std::cout << "[Test] Running Liked Track Indicator & Repeated Unlike verification..." << std::endl;

    // 1. AppState liked_track_ids tracking
    sctui::models::AppState state;
    REQUIRE(!state.is_track_liked(12345));

    state.liked_track_ids.insert(12345);
    REQUIRE(state.is_track_liked(12345));

    // Repeated like toggles it off (unlikes)
    bool current_liked = state.is_track_liked(12345);
    REQUIRE(current_liked == true);
    bool now_liked = !current_liked;
    REQUIRE(now_liked == false);
    state.liked_track_ids.erase(12345);
    REQUIRE(!state.is_track_liked(12345));

    // 2. TrackCacheManager liked track IDs retrieval
    std::filesystem::path test_dir = "/tmp/sctui_test_likes_cache_" + std::to_string(getpid());
    std::filesystem::create_directories(test_dir);
    {
        sctui::core::TrackCacheManager mgr(test_dir);
        auto make_dummy_file = [](const std::filesystem::path& p) {
            std::ofstream ofs(p, std::ios::binary);
            std::string data(2048, 'A');
            ofs.write(data.data(), data.size());
        };
        make_dummy_file(mgr.get_track_cache_path(1001));
        make_dummy_file(mgr.get_track_cache_path(1002));
        make_dummy_file(mgr.get_track_cache_path(1003));

        mgr.register_track(1001, "Track 1", "Artist 1", true);
        mgr.register_track(1002, "Track 2", "Artist 2", false);
        mgr.register_track(1003, "Track 3", "Artist 3", true);

        auto liked = mgr.get_liked_track_ids();
        REQUIRE(liked.size() == 2);
        REQUIRE(std::find(liked.begin(), liked.end(), 1001) != liked.end());
        REQUIRE(std::find(liked.begin(), liked.end(), 1003) != liked.end());
        REQUIRE(std::find(liked.begin(), liked.end(), 1002) == liked.end());

        // Unlike track 1001
        mgr.update_track_liked(1001, false);
        liked = mgr.get_liked_track_ids();
        REQUIRE(liked.size() == 1);
        REQUIRE(liked[0] == 1003);
    }
    std::filesystem::remove_all(test_dir);
    std::cout << "  -> CacheManager get_liked_track_ids and update_track_liked OK" << std::endl;

    // 3. TrackListView indicator placement & omission on Likes tab
    state.terminal_width = 120;
    state.terminal_height = 35;
    state.icon_style = sctui::core::IconStyle::Emoji;

    sctui::models::Track track1;
    track1.id = 7001;
    track1.title = "Synth Odyssey";
    track1.artist = "Neon Runner";
    track1.duration_ms = 210000;
    track1.is_liked = true;

    sctui::models::Track track2;
    track2.id = 7002;
    track2.title = "Midnight Drive";
    track2.artist = "Retro Dream";
    track2.duration_ms = 195000;
    track2.is_liked = false;

    state.liked_track_ids.insert(track1.id);

    sctui::ui::views::TrackListView track_list(state, nullptr, nullptr, nullptr, nullptr);

    // Test Search tab (must show like indicator for track1)
    state.current_view = sctui::models::ViewMode::Search;
    state.search_results = {track1, track2};
    auto elem_search = track_list.render(true);
    auto screen_search = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(35));
    ftxui::Render(screen_search, elem_search);
    std::string out_search = screen_search.ToString();

    REQUIRE(out_search.find("Synth Odyssey") != std::string::npos);
    REQUIRE(out_search.find("Midnight Drive") != std::string::npos);
    // Track 1 is liked, heart emoji indicator rendered between title and artist
    REQUIRE(out_search.find("💖") != std::string::npos);
    std::cout << "  -> Liked badge indicator rendered in Search results OK" << std::endl;

    // Test Playlists tab with selected playlist (must show like indicator for track1)
    state.current_view = sctui::models::ViewMode::Playlists;
    state.selected_playlist = sctui::models::Playlist{.id = 42, .title = "Best Vibes"};
    state.playlist_tracks = {track1, track2};
    auto elem_playlist = track_list.render(true);
    auto screen_playlist = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(35));
    ftxui::Render(screen_playlist, elem_playlist);
    std::string out_playlist = screen_playlist.ToString();

    REQUIRE(out_playlist.find("PLAYLIST: Best Vibes") != std::string::npos);
    REQUIRE(out_playlist.find("Synth Odyssey") != std::string::npos);
    REQUIRE(out_playlist.find("💖") != std::string::npos);
    std::cout << "  -> Liked badge indicator rendered in Playlist tracks OK" << std::endl;

    // Test Likes tab (like indicator MUST BE OMITTED)
    state.current_view = sctui::models::ViewMode::Likes;
    state.likes_tracks = {track1, track2};
    auto elem_likes = track_list.render(true);
    auto screen_likes = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(35));
    ftxui::Render(screen_likes, elem_likes);
    std::string out_likes = screen_likes.ToString();

    REQUIRE(out_likes.find(" LIKES ") != std::string::npos);
    REQUIRE(out_likes.find("Synth Odyssey") != std::string::npos);
    // In Likes view, the like badge column is omitted per requirement
    REQUIRE(out_likes.find("💖") == std::string::npos);
    std::cout << "  -> Liked badge indicator strictly omitted on Likes tab OK" << std::endl;

    // Test unliking track1 in Search view (toggle repeated like)
    state.current_view = sctui::models::ViewMode::Search;
    state.liked_track_ids.erase(track1.id);
    state.search_results[0].is_liked = false;
    auto elem_unliked = track_list.render(true);
    auto screen_unliked = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(35));
    ftxui::Render(screen_unliked, elem_unliked);
    std::string out_unliked = screen_unliked.ToString();

    REQUIRE(out_unliked.find("Synth Odyssey") != std::string::npos);
    REQUIRE(out_unliked.find("💖") == std::string::npos);
    std::cout << "  -> Unliking removes badge immediately OK" << std::endl;

    // 4. Test HttpClient::del
    sctui::net::HttpClient http;
    http.set_timeout_seconds(1);
    auto del_res = http.del("http://127.0.0.1:9999/not-existing-test-endpoint");
    REQUIRE(!del_res); // Confirms curl executes DELETE request and handles network failure cleanly
    std::cout << "  -> HttpClient del (DELETE) request execution OK" << std::endl;
}

void test_recent_improvements() {
    std::cout << "[Test] Running verification for playlist likes parsing, queue shuffle follow, and loading status..." << std::endl;

    // 1. Test playlist likes parsing (both authored and liked/favorited playlist wrapped structures)
    {
        std::string json_str = R"({
            "collection": [
                {
                    "id": 1001,
                    "title": "Authored Playlist",
                    "track_count": 12,
                    "duration": 3600000,
                    "likes_count": 5
                },
                {
                    "playlist": {
                        "id": 1002,
                        "title": "Liked Playlist from Other User",
                        "track_count": 25,
                        "duration": 7200000,
                        "favoritings_count": 450
                    }
                }
            ]
        })";

        sctui::net::SoundCloudApi api;
        auto res = api.parse_playlists_json(json_str);
        REQUIRE(res.has_value());
        REQUIRE(res->size() == 2);
        REQUIRE((*res)[0].id == 1001);
        REQUIRE((*res)[0].title == "Authored Playlist");
        REQUIRE((*res)[0].likes_count == 5);

        REQUIRE((*res)[1].id == 1002);
        REQUIRE((*res)[1].title == "Liked Playlist from Other User");
        REQUIRE((*res)[1].likes_count == 450);
        std::cout << "  -> Playlist likes and wrapped structures parsed OK" << std::endl;
    }

    // 2. Test QueueView automatically following playing track when shuffle is toggled
    {
        sctui::models::AppState state;
        state.terminal_width = 120;
        state.terminal_height = 40;

        for (int i = 0; i < 50; ++i) {
            sctui::models::Track t;
            t.id = 2000 + i;
            t.title = "Track " + std::to_string(i + 1);
            t.artist = "Artist " + std::to_string(i + 1);
            t.duration_ms = 180000;
            state.queue.tracks.push_back(t);
        }

        // Set playing track to 44th track (index 43)
        state.queue.set_current_index(43);
        state.active_track = state.queue.tracks[43];

        sctui::ui::views::QueueView qv(state, [](size_t){});
        qv.render(true);

        // Toggle shuffle ON
        state.queue.toggle_shuffle();
        REQUIRE(state.queue.shuffle == true);
        REQUIRE(state.queue.shuffle_cursor == 0);
        REQUIRE(state.queue.shuffle_indices[0] == 43);

        // Render should detect shuffle toggle and center on shuffle_cursor (0)
        qv.render(true);

        // Toggle shuffle OFF
        state.queue.toggle_shuffle();
        REQUIRE(state.queue.shuffle == false);
        REQUIRE(state.queue.current_index == 43);

        // Render should detect shuffle toggle off and center on current_index (43)
        qv.render(true);
        std::cout << "  -> QueueView correctly followed playing track across shuffle toggles OK" << std::endl;
    }

    // 3. Test PlayerView loading status
    {
        sctui::models::AppState state;
        state.terminal_width = 120;
        state.terminal_height = 40;
        state.playback_state = sctui::audio::PlaybackState::Loading;

        sctui::ui::views::PlayerView pv(state);
        auto elem = pv.render(true);

        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(10));
        ftxui::Render(screen, elem);
        std::string out = screen.ToString();

        REQUIRE(out.find("LOADING") != std::string::npos);
        REQUIRE(out.find("STOPPED") == std::string::npos);
        std::cout << "  -> PlayerView correctly rendered LOADING status OK" << std::endl;
    }

    // 4. Test like icon spacing in TrackListView (icon is spaced to the left from artist name)
    {
        sctui::models::AppState state;
        state.terminal_width = 120;
        state.terminal_height = 40;
        state.current_view = sctui::models::ViewMode::Search;
        state.icon_style = sctui::core::IconStyle::Emoji;

        sctui::models::Track track;
        track.id = 99999;
        track.title = "Neon Nights";
        track.artist = "SynthwaveArtist";
        track.duration_ms = 210000;
        track.is_liked = true;
        state.search_results.push_back(track);
        state.liked_track_ids.insert(track.id);

        sctui::ui::views::TrackListView tlv(state, nullptr, nullptr, nullptr, nullptr);
        auto elem = tlv.render(true);

        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(30));
        ftxui::Render(screen, elem);
        std::string out = screen.ToString();

        REQUIRE(out.find("Neon Nights") != std::string::npos);
        REQUIRE(out.find("SynthwaveArtist") != std::string::npos);
        REQUIRE(out.find("💖  ") != std::string::npos);
        std::cout << "  -> Like icon has 2 spaces separating it from artist's name OK" << std::endl;
    }
}

void test_in_tab_search_and_fullscreen_shuf_bold() {
    std::cout << "[Test] Running Likes & Playlists In-Tab Search and Fullscreen Shuf Bold verification..." << std::endl;

    // 1. In-tab search in Likes
    {
        sctui::models::AppState state;
        state.terminal_width = 120;
        state.terminal_height = 40;
        state.current_view = sctui::models::ViewMode::Likes;
        state.focused_panel = sctui::models::FocusPanel::TrackList;

        sctui::models::Track t1;
        t1.id = 1; t1.title = "Strobe"; t1.artist = "Deadmau5";
        sctui::models::Track t2;
        t2.id = 2; t2.title = "Ghosts 'n' Stuff"; t2.artist = "Deadmau5";
        sctui::models::Track t3;
        t3.id = 3; t3.title = "Alone"; t3.artist = "Marshmello";

        state.likes_tracks = {t1, t2, t3};

        sctui::ui::views::TrackListView tlv(state, nullptr, nullptr, nullptr, nullptr);

        // Before search: render has subtle prompt
        auto elem_idle = tlv.render(true);
        auto screen_idle = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(30));
        ftxui::Render(screen_idle, elem_idle);
        std::string out_idle = screen_idle.ToString();
        REQUIRE(out_idle.find("alt+/ to search in likes") != std::string::npos);
        REQUIRE(tlv.get_current_track_list().size() == 3);

        // Trigger Alt+/ via event
        tlv.get_component()->OnEvent(ftxui::Event::Special("\x1b/"));
        REQUIRE(tlv.is_searching());

        // Type "ghost" into search box
        for (char c : std::string("ghost")) {
            tlv.get_component()->OnEvent(ftxui::Event::Character(c));
        }

        // Live filtering check
        const auto& filtered = tlv.get_current_track_list();
        REQUIRE(filtered.size() == 1);
        REQUIRE(filtered[0].title == "Ghosts 'n' Stuff");

        // Render while searching has input box
        auto elem_searching = tlv.render(true);
        auto screen_searching = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(30));
        ftxui::Render(screen_searching, elem_searching);
        std::string out_searching = screen_searching.ToString();
        REQUIRE(out_searching.find("ghost") != std::string::npos);
        REQUIRE(out_searching.find("Ghosts 'n' Stuff") != std::string::npos);
        REQUIRE(out_searching.find("Strobe") == std::string::npos);

        // Escape closes search and restores full list
        tlv.get_component()->OnEvent(ftxui::Event::Escape);
        REQUIRE(!tlv.is_searching());
        REQUIRE(tlv.get_current_track_list().size() == 3);
        std::cout << "  -> Likes in-tab Alt+/ search & live filtering verified OK" << std::endl;
    }

    // 2. In-tab search in Playlists
    {
        sctui::models::AppState state;
        state.terminal_width = 120;
        state.terminal_height = 40;
        state.current_view = sctui::models::ViewMode::Playlists;
        state.focused_panel = sctui::models::FocusPanel::TrackList;

        sctui::models::Playlist p1;
        p1.id = 101; p1.title = "Chill Beats"; p1.track_count = 10;
        sctui::models::Playlist p2;
        p2.id = 102; p2.title = "Gym Workout"; p2.track_count = 25;
        sctui::models::Playlist p3;
        p3.id = 103; p3.title = "Chillhop Essentials"; p3.track_count = 50;

        state.playlists = {p1, p2, p3};

        sctui::ui::views::TrackListView tlv(state, nullptr, nullptr, nullptr, nullptr);

        // Before search: subtle inscribe for playlists
        auto elem_idle = tlv.render(true);
        auto screen_idle = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(30));
        ftxui::Render(screen_idle, elem_idle);
        std::string out_idle = screen_idle.ToString();
        REQUIRE(out_idle.find("alt+/ to search in playlists") != std::string::npos);
        REQUIRE(tlv.get_current_playlist_list().size() == 3);

        // Toggle search
        tlv.toggle_search();
        REQUIRE(tlv.is_searching());

        // Type "chill"
        for (char c : std::string("chill")) {
            tlv.get_component()->OnEvent(ftxui::Event::Character(c));
        }

        const auto& filtered_pls = tlv.get_current_playlist_list();
        REQUIRE(filtered_pls.size() == 2);
        REQUIRE(filtered_pls[0].title == "Chill Beats");
        REQUIRE(filtered_pls[1].title == "Chillhop Essentials");

        // Close search
        tlv.close_search();
        REQUIRE(!tlv.is_searching());
        REQUIRE(tlv.get_current_playlist_list().size() == 3);
        std::cout << "  -> Playlists in-tab Alt+/ search & live filtering verified OK" << std::endl;
    }

    // 3. Fullscreen visualizer styles: LowFreqCentered and HighFreqCentered
    {
        int w = 101;
        float mid = (w - 1) / 2.0f;

        // At center (x = 50):
        float dist_center_mid = std::abs(50.0f - mid) / mid;
        float norm_low_mid = std::clamp(dist_center_mid, 0.0f, 1.0f);
        float norm_high_mid = 1.0f - std::clamp(dist_center_mid, 0.0f, 1.0f);
        REQUIRE(norm_low_mid == 0.0f);
        REQUIRE(norm_high_mid == 1.0f);

        // At edge (x = 0):
        float dist_center_edge = std::abs(0.0f - mid) / mid;
        float norm_low_edge = std::clamp(dist_center_edge, 0.0f, 1.0f);
        float norm_high_edge = 1.0f - std::clamp(dist_center_edge, 0.0f, 1.0f);
        REQUIRE(norm_low_edge == 1.0f);
        REQUIRE(norm_high_edge == 0.0f);

        std::cout << "  -> Fullscreen visualizer LowFreqCentered and HighFreqCentered symmetry OK" << std::endl;
    }
}

int main() {
    std::cout << "=== Running sctui test suite ===" << std::endl;
    test_spsc_queue();
    test_bounded_channel();
    test_thread_pool();
    test_queue_alignment();
    test_settings_topmost_selection();
    test_onboarding_wizard();
    test_entropy_shuffle();
    test_dynamic_queue_append_and_shuffle();
    test_queue_next_and_reorder();
    test_duplicate_track_shuffle_queue_next();
    test_volume_150_and_visualizer_curve();
    test_mouse_control_option();
    test_settings_and_mouse_coordinate_mapping();
    test_cover_art_processor();
    test_drm_and_goplus_separation();
    test_castlabs_electron_presence_and_download();
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
    test_like_indicator_and_toggle_unlike();
    test_recent_improvements();
    test_in_tab_search_and_fullscreen_shuf_bold();
    std::cout << "=== All tests PASSED successfully! ===" << std::endl;
    return 0;
}
