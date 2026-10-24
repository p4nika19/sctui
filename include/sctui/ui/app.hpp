#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include <ftxui/component/screen_interactive.hpp>
#include "sctui/core/config.hpp"
#include "sctui/core/channel.hpp"
#include "sctui/core/thread_pool.hpp"
#include "sctui/models/state.hpp"
#include "sctui/net/soundcloud_api.hpp"
#include "sctui/audio/audio_engine.hpp"
#include "sctui/mpris/mpris_server.hpp"
#include "sctui/ui/views/header_view.hpp"
#include "sctui/ui/views/sidebar_view.hpp"
#include "sctui/ui/views/track_list_view.hpp"
#include "sctui/ui/views/player_view.hpp"
#include "sctui/ui/views/queue_view.hpp"
#include "sctui/ui/views/settings_view.hpp"
#include "sctui/ui/views/account_view.hpp"
#include "sctui/core/track_cache.hpp"
#include "sctui/discord/discord_rpc.hpp"

namespace sctui::ui {

// Asynchronous UI events dispatched by background workers to main thread
enum class AsyncEventType {
    SearchResultsReady,
    StreamTracksReady,
    LikesTracksReady,
    LikesTracksAppended,
    LikesTracksDiffPrepended,
    LikesFullyLoaded,
    LikesDownloadProgress,
    LikesDownloadFinished,
    PlaylistsReady,
    PlaylistTracksReady,
    StreamResolvedAndPlay,
    TrackFinished,
    WaveformReady,
    CoverArtReady,
    ErrorMessage,
    InfoMessage,
    ClientIdResolved,
    MprisNext,
    MprisPrevious,
    MprisPlayPause,
    MprisStop,
    UserProfileReady,
    UserTracksReady
};

struct AsyncUIEvent {
    AsyncEventType type{AsyncEventType::InfoMessage};
    std::string text_payload{};
    std::vector<models::Track> tracks_payload{};
    std::vector<models::Playlist> playlists_payload{};
    std::vector<float> waveform_payload{};
    uint64_t numeric_payload{0};
    models::CoverArtImage cover_art_payload{};
    models::ColorRGB accent_color_payload{255, 85, 0};
    models::UserProfile profile_payload{};
};

class App {
public:
    explicit App(core::Config config);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    core::Result<void> run();
    void request_exit();

private:
    void init_ui();
    void setup_keybindings();
    void start_worker_threads();
    void stop_worker_threads();

    // Async task submission
    void submit_search(const std::string& query);
    void submit_play_track(models::Track track);
    void submit_load_view(models::ViewMode view);
    void submit_reload_current_view(bool force_from_scratch = false);
    void submit_load_playlist(models::Playlist playlist);
    void submit_fetch_waveform(const models::Track& track);
    void submit_toggle_like(models::Track track);
    void play_next_track();
    void play_previous_track();
    void apply_network_settings();
    void trigger_auto_extract_cookies();
    void submit_load_account_profile();
    void trigger_download_all_likes();
    void cancel_download_all_likes();

    // Event handlers
    void process_async_events();
    void handle_track_finished(uint64_t track_id);
    void cycle_focus(bool reverse = false);
    void update_telemetry();
    bool handle_mouse_event(ftxui::Event event);
    ftxui::Element render_fullscreen_playback();

    core::Config config_;
    models::AppState state_;
    net::SoundCloudApi api_;
    audio::AudioEngine audio_;

    ftxui::ScreenInteractive screen_;
    ftxui::Component main_component_;

    // MPRIS Server
    std::unique_ptr<mpris::MprisServer> mpris_server_;

    // Discord Rich Presence
    std::unique_ptr<discord::DiscordRpc> discord_rpc_;

    // Views
    std::unique_ptr<views::HeaderView> header_view_;
    std::unique_ptr<views::SidebarView> sidebar_view_;
    std::unique_ptr<views::TrackListView> track_list_view_;
    std::unique_ptr<views::PlayerView> player_view_;
    std::unique_ptr<views::QueueView> queue_view_;
    std::unique_ptr<views::SettingsView> settings_view_;
    std::unique_ptr<views::AccountView> account_view_;

    std::shared_ptr<core::TrackCacheManager> cache_manager_;

    // Concurrency channels
    core::BoundedChannel<AsyncUIEvent> ui_event_channel_{256};
    std::atomic<bool> is_running_{true};
    core::ThreadPool thread_pool_{6};
    std::atomic<uint64_t> likes_fetch_generation_{0};
    std::jthread worker_thread_;
    std::jthread telemetry_thread_;
    std::string last_search_query_;
    bool avatar_drawn_in_terminal_{false};
    bool fullscreen_cover_drawn_{false};
    bool queue_source_is_likes_{false};
    std::atomic<bool> is_downloading_likes_{false};
    std::jthread likes_download_thread_;
    std::string likes_resume_href_;
    std::atomic<bool> likes_fully_fetched_{false};
};

} // namespace sctui::ui
