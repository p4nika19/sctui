#include "sctui/ui/app.hpp"
#include "sctui/ui/views/help_popup.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include "sctui/core/logger.hpp"
#include "sctui/core/cover_art.hpp"
#include "sctui/core/cookie_extractor.hpp"
#include <ftxui/component/event.hpp>
#include <ftxui/screen/terminal.hpp>
#include <iostream>
#include <random>
#include <unordered_set>

namespace sctui::ui {

using namespace ftxui;

App::App(core::Config config)
    : config_(std::move(config)),
      api_(config_.client_id, config_.auth_token),
      screen_(ScreenInteractive::Fullscreen()) {

    cache_manager_ = std::make_shared<core::TrackCacheManager>(config_.cache_dir);
    audio_.set_cache_manager(cache_manager_);

    state_.volume_percent = static_cast<float>(config_.volume);
    state_.mouse_control = config_.mouse_control;
    state_.queue.repeat_mode = config_.repeat_mode;
    state_.queue.shuffle = config_.shuffle;
    state_.icon_style = config_.icon_style;
    state_.drm_engine_enabled = config_.drm_engine_enabled;
    state_.download_drm_tracks_in_likes = config_.download_drm_tracks_in_likes;

    mpris::MprisCallbacks mpris_cb{
        .on_play = [this]() {
            audio_.resume();
            screen_.PostEvent(Event::Custom);
        },
        .on_pause = [this]() {
            audio_.pause();
            screen_.PostEvent(Event::Custom);
        },
        .on_play_pause = [this]() {
            ui_event_channel_.push(AsyncUIEvent{ .type = AsyncEventType::MprisPlayPause });
            screen_.PostEvent(Event::Custom);
        },
        .on_stop = [this]() {
            ui_event_channel_.push(AsyncUIEvent{ .type = AsyncEventType::MprisStop });
            screen_.PostEvent(Event::Custom);
        },
        .on_next = [this]() {
            ui_event_channel_.push(AsyncUIEvent{ .type = AsyncEventType::MprisNext });
            screen_.PostEvent(Event::Custom);
        },
        .on_prev = [this]() {
            ui_event_channel_.push(AsyncUIEvent{ .type = AsyncEventType::MprisPrevious });
            screen_.PostEvent(Event::Custom);
        },
        .on_seek_relative = [this](int64_t offset_ms) {
            audio_.seek_relative(offset_ms);
            screen_.PostEvent(Event::Custom);
        },
        .on_seek_absolute = [this](uint64_t target_ms) {
            audio_.seek_absolute(target_ms);
            screen_.PostEvent(Event::Custom);
        },
        .on_set_volume = [this](float vol_pct) {
            state_.volume_percent = vol_pct;
            audio_.set_volume(vol_pct);
            state_.is_muted = false;
            screen_.PostEvent(Event::Custom);
        },
        .on_set_shuffle = [this](bool shuf) {
            if (state_.queue.shuffle != shuf) {
                state_.queue.toggle_shuffle();
                state_.notify(state_.queue.shuffle ? "Shuffle ON" : "Shuffle OFF");
                screen_.PostEvent(Event::Custom);
            }
        },
        .on_set_loop_status = [this](core::RepeatMode mode) {
            state_.queue.repeat_mode = mode;
            screen_.PostEvent(Event::Custom);
        },
        .on_quit = [this]() {
            request_exit();
        }
    };
    mpris_server_ = std::make_unique<mpris::MprisServer>(std::move(mpris_cb));
}

App::~App() {
    stop_worker_threads();
}

core::Result<void> App::run() {
    // 1. Initialize audio engine
    auto audio_res = audio_.init();
    if (!audio_res) {
        std::cerr << "Warning: Audio engine initialization issue: " << audio_res.error().message << std::endl;
    }
    audio_.set_volume(state_.volume_percent);
    audio_.set_track_finished_callback([this](uint64_t track_id) {
        ui_event_channel_.push(AsyncUIEvent{
            .type = AsyncEventType::TrackFinished,
            .numeric_payload = track_id
        });
        screen_.PostEvent(Event::Custom);
    });

    // 2. Start worker and telemetry threads
    start_worker_threads();
    if (mpris_server_) {
        mpris_server_->start();
        mpris_server_->update_volume(state_.volume_percent);
        mpris_server_->update_shuffle(state_.queue.shuffle);
        mpris_server_->update_repeat_mode(state_.queue.repeat_mode);
    }
    discord_rpc_ = std::make_unique<discord::DiscordRpc>(config_.discord_client_id);
    discord_rpc_->set_enabled(config_.discord_rpc_enabled);
    discord_rpc_->start();

    // Configure networking (DoH, custom DNS, proxy)
    apply_network_settings();

    // Auto-extract cookies if auth_token is not set
    if (config_.auth_token.empty()) {
        trigger_auto_extract_cookies();
    }

    // 3. Resolve client_id if not present
    if (config_.client_id.empty() && config_.auto_extract_client_id) {
        thread_pool_.enqueue([this]() {
            if (!is_running_) return;
            auto res = api_.resolve_client_id();
            if (res && is_running_) {
                config_.client_id = *res;
                (void)config_.save();
                ui_event_channel_.push(AsyncUIEvent{
                    .type = AsyncEventType::ClientIdResolved,
                    .text_payload = *res
                });
                screen_.PostEvent(Event::Custom);
            }
        });
    }

    // 4. Initialize UI views and layout
    init_ui();

    // 5. Initial fetch of default trending / stream tracks
    submit_load_view(models::ViewMode::Stream);

    // 6. Start FTXUI main loop
    screen_.TrackMouse(config_.mouse_control);
    screen_.Loop(main_component_);

    // 7. Cleanup on exit
    audio_.shutdown();
    core::CoverArtProcessor::clear_terminal_image(0);
    if (config_.mouse_control) {
        std::cout << "\033[?1000l\033[?1002l\033[?1003l\033[?1006l" << std::flush;
    }
    config_.volume = static_cast<int>(state_.volume_percent);
    config_.repeat_mode = state_.queue.repeat_mode;
    config_.shuffle = state_.queue.shuffle;
    config_.mouse_control = state_.mouse_control;
    (void)config_.save();

    return core::Ok();
}

void App::request_exit() {
    is_running_ = false;
    core::CoverArtProcessor::clear_terminal_image(0);
    screen_.Exit();
}

void App::init_ui() {
    // Instantiate views
    header_view_ = std::make_unique<views::HeaderView>(state_, [this](const std::string& query) {
        submit_search(query);
    });

    sidebar_view_ = std::make_unique<views::SidebarView>(state_, config_, [this](models::ViewMode view) {
        submit_load_view(view);
    });

    track_list_view_ = std::make_unique<views::TrackListView>(
        state_,
        [this](const models::Track& track, size_t index) {
            const auto& current_list = track_list_view_->get_current_track_list();
            if (!current_list.empty()) {
                queue_source_is_likes_ = (state_.current_view == models::ViewMode::Likes);
                state_.queue.tracks = current_list;
                state_.queue.current_index = static_cast<int>(index);
                if (state_.queue.shuffle) {
                    state_.queue.rebuild_shuffle();
                }
            }
            submit_play_track(track);
        },
        [this](const models::Track& track, bool play_next) {
            if (play_next && state_.queue.current_index >= 0 && state_.queue.current_index + 1 <= static_cast<int>(state_.queue.tracks.size())) {
                state_.queue.tracks.insert(state_.queue.tracks.begin() + state_.queue.current_index + 1, track);
            } else {
                state_.queue.tracks.push_back(track);
            }
            state_.notify("Added to queue: " + core::truncate_string(track.title, 25));
        },
        [this](const models::Track& track) {
            submit_toggle_like(track);
        },
        [this](const models::Playlist& playlist) {
            submit_load_playlist(playlist);
        }
    );

    player_view_ = std::make_unique<views::PlayerView>(state_);

    queue_view_ = std::make_unique<views::QueueView>(state_, [this](size_t index) {
        if (index < state_.queue.tracks.size()) {
            state_.queue.current_index = static_cast<int>(index);
            submit_play_track(state_.queue.tracks[index]);
        }
    });

    account_view_ = std::make_unique<views::AccountView>(
        state_,
        config_,
        [this](const models::Track& track, size_t index) {
            state_.queue.tracks = state_.user_tracks;
            state_.queue.current_index = static_cast<int>(index);
            if (state_.queue.shuffle) {
                state_.queue.rebuild_shuffle();
            }
            submit_play_track(track);
        },
        [this](const models::Track& track, bool play_next) {
            if (play_next && state_.queue.current_index >= 0 && state_.queue.current_index + 1 <= static_cast<int>(state_.queue.tracks.size())) {
                state_.queue.tracks.insert(state_.queue.tracks.begin() + state_.queue.current_index + 1, track);
            } else {
                state_.queue.tracks.push_back(track);
            }
            state_.notify("Added to queue: " + core::truncate_string(track.title, 25));
        },
        [this](const models::Track& track) {
            submit_toggle_like(track);
        },
        [this]() {
            trigger_auto_extract_cookies();
            submit_load_account_profile();
        }
    );

    settings_view_ = std::make_unique<views::SettingsView>(
        state_,
        config_,
        cache_manager_,
        [this](const core::Config& new_cfg) {
            config_ = new_cfg;
            (void)config_.save();
            apply_network_settings();
            core::CoverArtProcessor::clear_terminal_image(0);
            if (discord_rpc_) {
                discord_rpc_->set_enabled(config_.discord_rpc_enabled);
            }
            if (config_.mouse_control) {
                std::cout << "\033[?1000h\033[?1002h\033[?1003h\033[?1006h" << std::flush;
            } else {
                std::cout << "\033[?1000l\033[?1002l\033[?1003l\033[?1006l" << std::flush;
            }
            state_.mouse_control = config_.mouse_control;
            if (cache_manager_) {
                cache_manager_->enforce_cache_limits(
                    config_.max_cache_size_mb,
                    config_.lock_liked_tracks_in_cache,
                    config_.dont_count_liked_in_general_cache
                );
            }
            audio_.set_volume(config_.volume);
            state_.volume_percent = static_cast<float>(config_.volume);
            state_.icon_style = config_.icon_style;
            state_.notify("Settings saved and applied successfully!", false, 4);
        },
        [this]() {
            trigger_auto_extract_cookies();
        },
        [this]() {
            thread_pool_.enqueue([this]() {
                if (!is_running_) return;
                auto res = api_.resolve_client_id();
                if (res && is_running_) {
                    config_.client_id = *res;
                    (void)config_.save();
                    ui_event_channel_.push(AsyncUIEvent{
                        .type = AsyncEventType::ClientIdResolved,
                        .text_payload = *res
                    });
                    screen_.PostEvent(Event::Custom);
                }
            });
        },
        [this](bool keep_liked) {
            if (cache_manager_) {
                cache_manager_->clear_cache(keep_liked);
                state_.notify("Cache cleaned successfully!", false, 3);
            }
        },
        [this]() {
            trigger_download_all_likes();
        }
    );

    // Composite component
    auto container = Container::Vertical({
        header_view_->get_component(),
        Container::Horizontal({
            sidebar_view_->get_component(),
            track_list_view_->get_component(),
            queue_view_->get_component(),
            account_view_->get_component(),
            settings_view_->get_component()
        }),
        player_view_->get_component()
    });

    // Event filter and renderer
    auto renderer = Renderer(container, [this]() {
        // Read terminal size dynamically to support SIGWINCH / resize smoothly
        auto dim = Terminal::Size();
        state_.terminal_width = dim.dimx;
        state_.terminal_height = dim.dimy;

        if (state_.is_fullscreen) {
            if (avatar_drawn_in_terminal_) {
                core::CoverArtProcessor::clear_terminal_image(2);
                avatar_drawn_in_terminal_ = false;
            }
            fullscreen_cover_drawn_ = true;
            return render_fullscreen_playback();
        }

        if (fullscreen_cover_drawn_) {
            core::CoverArtProcessor::clear_terminal_image(3);
            fullscreen_cover_drawn_ = false;
        }

        auto header_elem = header_view_->render();
        auto sidebar_elem = sidebar_view_->render(state_.focused_panel == models::FocusPanel::Sidebar);
        auto track_list_elem = track_list_view_->render(state_.focused_panel == models::FocusPanel::TrackList);
        auto player_elem = player_view_->render(state_.focused_panel == models::FocusPanel::Player);

        // Ensure Kitty avatar overlay (image_id=2) is deleted when leaving Account view
        if (state_.current_view != models::ViewMode::Account) {
            if (avatar_drawn_in_terminal_) {
                core::CoverArtProcessor::clear_terminal_image(2);
                avatar_drawn_in_terminal_ = false;
            }
        } else {
            avatar_drawn_in_terminal_ = true;
        }

        Element main_body;
        if (state_.current_view == models::ViewMode::Settings) {
            auto settings_elem = settings_view_->render(state_.focused_panel == models::FocusPanel::SettingsPanel);
            main_body = hbox({
                sidebar_elem,
                settings_elem | xflex
            }) | yflex;
        } else if (state_.current_view == models::ViewMode::Account) {
            auto account_elem = account_view_->render(state_.focused_panel == models::FocusPanel::AccountPanel);
            main_body = hbox({
                sidebar_elem,
                account_elem | xflex
            }) | yflex;
        } else if (state_.current_view == models::ViewMode::Queue) {
            auto queue_elem = queue_view_->render(state_.focused_panel == models::FocusPanel::QueueList);
            main_body = hbox({
                sidebar_elem,
                queue_elem | xflex
            }) | yflex;
        } else if (state_.is_ultrawide()) {
            // 3-column layout in ultrawide mode
            auto queue_elem = queue_view_->render(state_.focused_panel == models::FocusPanel::QueueList);
            main_body = hbox({
                sidebar_elem,
                track_list_elem | xflex,
                queue_elem | size(WIDTH, EQUAL, 36)
            }) | yflex;
        } else {
            main_body = hbox({
                sidebar_elem,
                track_list_elem | xflex
            }) | yflex;
        }

        auto layout = vbox({
            header_elem,
            main_body,
            player_elem
        }) | size(WIDTH, EQUAL, state_.terminal_width) | bgcolor(Color::RGB(5, 5, 6));

        if (state_.focused_panel == models::FocusPanel::HelpModal) {
            return dbox({
                layout,
                views::HelpPopup::render(state_) | center
            });
        }

        return layout;
    });

    main_component_ = CatchEvent(renderer, [this](Event event) {
        // Process background events whenever any UI event happens
        process_async_events();

        if (event == Event::Custom) {
            return true;
        }

        if (event.is_mouse()) {
            if (!config_.mouse_control) {
                return false;
            }
            return handle_mouse_event(event);
        }

        // If Search Input is focused, let FTXUI Input handle typing and navigation inside the box
        if (state_.focused_panel == models::FocusPanel::SearchInput) {
            if (event == Event::Escape) {
                state_.focused_panel = models::FocusPanel::TrackList;
                return true;
            }
            if (event == Event::Tab) {
                cycle_focus(false);
                return true;
            }
            if (event == Event::TabReverse) {
                cycle_focus(true);
                return true;
            }
            return header_view_->get_component()->OnEvent(event);
        }

        // If a text input box in Settings is focused, forward ALL events directly to settings view
        if (state_.focused_panel == models::FocusPanel::SettingsPanel && settings_view_ && settings_view_->is_editing_text()) {
            return settings_view_->get_component()->OnEvent(event);
        }

        // Global Help Modal toggle
        if (event == Event::Character('?')) {
            if (state_.focused_panel == models::FocusPanel::HelpModal) {
                state_.focused_panel = models::FocusPanel::TrackList;
            } else {
                state_.focused_panel = models::FocusPanel::HelpModal;
            }
            return true;
        }

        if (state_.focused_panel == models::FocusPanel::HelpModal) {
            if (event == Event::Escape || event == Event::Character('q')) {
                state_.focused_panel = models::FocusPanel::TrackList;
                return true;
            }
            return true; // Modal traps other keys
        }

        // Fullscreen toggle keys
        if ((event == Event::Character('f') || event == Event::Character('F')) &&
            state_.focused_panel != models::FocusPanel::SearchInput) {
            state_.is_fullscreen = !state_.is_fullscreen;
            if (state_.is_fullscreen) {
                core::CoverArtProcessor::clear_terminal_image(1);
                state_.notify("Entered Fullscreen Playback (Press 'f' or 'Esc' to exit)", false, 2);
            } else {
                core::CoverArtProcessor::clear_terminal_image(3);
                fullscreen_cover_drawn_ = false;
                state_.notify("Exited Fullscreen Playback", false, 2);
            }
            return true;
        }

        // Fullscreen exit via Esc or 'q'
        if (state_.is_fullscreen) {
            if (event == Event::Escape || event == Event::Character('q')) {
                state_.is_fullscreen = false;
                core::CoverArtProcessor::clear_terminal_image(3);
                fullscreen_cover_drawn_ = false;
                return true;
            }
        }

        // Global Exit
        if (event == Event::Character('q') && state_.focused_panel != models::FocusPanel::SearchInput) {
            request_exit();
            return true;
        }

        // Global Search focus
        if (event == Event::Character('/') && state_.focused_panel != models::FocusPanel::SearchInput) {
            state_.focused_panel = models::FocusPanel::SearchInput;
            header_view_->focus_input();
            return true;
        }

        // Escape: Unfocus Search or Return to TrackList
        if (event == Event::Escape) {
            if (state_.focused_panel == models::FocusPanel::SearchInput) {
                state_.focused_panel = models::FocusPanel::TrackList;
                return true;
            }
        }

        // Panel cycling via Tab / Shift+Tab
        if (event == Event::Tab) {
            cycle_focus(false);
            return true;
        }
        if (event == Event::TabReverse) {
            cycle_focus(true);
            return true;
        }

        // Quick View Switching 1..7
        if (event == Event::Character('1')) {
            state_.focused_panel = models::FocusPanel::TrackList;
            submit_load_view(models::ViewMode::Search);
            return true;
        }
        if (event == Event::Character('2')) {
            state_.focused_panel = models::FocusPanel::TrackList;
            submit_load_view(models::ViewMode::Stream);
            return true;
        }
        if (event == Event::Character('3')) {
            state_.focused_panel = models::FocusPanel::TrackList;
            submit_load_view(models::ViewMode::Likes);
            return true;
        }
        if (event == Event::Character('4')) {
            state_.focused_panel = models::FocusPanel::TrackList;
            submit_load_view(models::ViewMode::Playlists);
            return true;
        }
        if (event == Event::Character('5')) {
            state_.focused_panel = models::FocusPanel::QueueList;
            submit_load_view(models::ViewMode::Queue);
            return true;
        }
        if (event == Event::Character('6')) {
            state_.focused_panel = models::FocusPanel::AccountPanel;
            submit_load_view(models::ViewMode::Account);
            return true;
        }
        if (event == Event::Character('7') || event == Event::Character('S')) {
            state_.focused_panel = models::FocusPanel::SettingsPanel;
            submit_load_view(models::ViewMode::Settings);
            return true;
        }

        // Reload / Refresh current view (F5 or Shift+R)
        if (event == Event::F5 || event == Event::Character('R')) {
            submit_reload_current_view(false);
            return true;
        }

        // Force Reload view from scratch (Ctrl+R)
        if (event == Event::CtrlR) {
            submit_reload_current_view(true);
            return true;
        }

        // Global Playback Controls
        if (event == Event::Character(' ')) {
            audio_.toggle_playback();
            return true;
        }
        if (event == Event::Character('n') || event == Event::Character('N') || event == Event::Character('>')) {
            play_next_track();
            return true;
        }
        if (event == Event::Character('p') || event == Event::Character('P') || event == Event::Character('<')) {
            play_previous_track();
            return true;
        }
        if (event == Event::Character('[') || event == Event::Character(',')) {
            audio_.seek_relative(-5000);
            return true;
        }
        if (event == Event::Character(']') || event == Event::Character('.')) {
            audio_.seek_relative(5000);
            return true;
        }
        if (event == Event::Character('H')) {
            audio_.seek_relative(-30000);
            return true;
        }
        if (event == Event::Character('L')) {
            audio_.seek_relative(30000);
            return true;
        }
        if (event == Event::Character('+') || event == Event::Character('=')) {
            state_.volume_percent = std::clamp(state_.volume_percent + 5.0f, 0.0f, 150.0f);
            audio_.set_volume(state_.volume_percent);
            state_.is_muted = false;
            return true;
        }
        if (event == Event::Character('-') || event == Event::Character('_')) {
            state_.volume_percent = std::clamp(state_.volume_percent - 5.0f, 0.0f, 150.0f);
            audio_.set_volume(state_.volume_percent);
            state_.is_muted = false;
            return true;
        }
        if (event == Event::Character('m')) {
            state_.is_muted = !state_.is_muted;
            if (state_.is_muted) {
                state_.pre_mute_volume = state_.volume_percent;
                audio_.set_volume(0.0f);
            } else {
                state_.volume_percent = state_.pre_mute_volume;
                audio_.set_volume(state_.volume_percent);
            }
            return true;
        }
        if (event == Event::Character('s')) {
            state_.queue.toggle_shuffle();
            state_.notify(state_.queue.shuffle ? "Shuffle ON" : "Shuffle OFF");
            return true;
        }
        if (event == Event::Character('r')) {
            int next_r = (static_cast<int>(state_.queue.repeat_mode) + 1) % 3;
            state_.queue.repeat_mode = static_cast<core::RepeatMode>(next_r);
            const char* r_str = (next_r == 0) ? "Repeat OFF" : (next_r == 1 ? "Repeat ONE" : "Repeat ALL");
            state_.notify(r_str);
            return true;
        }

        // Direct panel event dispatch based on state_.focused_panel
        switch (state_.focused_panel) {
            case models::FocusPanel::Sidebar:
                return sidebar_view_->get_component()->OnEvent(event);
            case models::FocusPanel::TrackList:
                return track_list_view_->get_component()->OnEvent(event);
            case models::FocusPanel::QueueList:
                return queue_view_->get_component()->OnEvent(event);
            case models::FocusPanel::AccountPanel:
                return account_view_->get_component()->OnEvent(event);
            case models::FocusPanel::SettingsPanel:
                return settings_view_->get_component()->OnEvent(event);
            case models::FocusPanel::Player:
                if (event == Event::Character('h') || event == Event::ArrowLeft) {
                    audio_.seek_relative(-5000);
                    return true;
                }
                if (event == Event::Character('l') || event == Event::ArrowRight) {
                    audio_.seek_relative(5000);
                    return true;
                }
                return player_view_->get_component()->OnEvent(event);
            default:
                break;
        }

        return false;
    });
}

void App::cycle_focus(bool reverse) {
    if (reverse) {
        switch (state_.focused_panel) {
            case models::FocusPanel::SearchInput:
                state_.focused_panel = models::FocusPanel::Player;
                break;
            case models::FocusPanel::Sidebar:
                state_.focused_panel = models::FocusPanel::SearchInput;
                header_view_->focus_input();
                break;
            case models::FocusPanel::TrackList:
            case models::FocusPanel::QueueList:
            case models::FocusPanel::AccountPanel:
            case models::FocusPanel::SettingsPanel:
                state_.focused_panel = models::FocusPanel::Sidebar;
                break;
            case models::FocusPanel::Player:
                if (state_.current_view == models::ViewMode::Settings) {
                    state_.focused_panel = models::FocusPanel::SettingsPanel;
                } else if (state_.current_view == models::ViewMode::Account) {
                    state_.focused_panel = models::FocusPanel::AccountPanel;
                } else if (state_.current_view == models::ViewMode::Queue) {
                    state_.focused_panel = models::FocusPanel::QueueList;
                } else {
                    state_.focused_panel = models::FocusPanel::TrackList;
                }
                break;
            case models::FocusPanel::HelpModal:
                break;
        }
    } else {
        switch (state_.focused_panel) {
            case models::FocusPanel::SearchInput:
                state_.focused_panel = models::FocusPanel::Sidebar;
                break;
            case models::FocusPanel::Sidebar:
                if (state_.current_view == models::ViewMode::Settings) {
                    state_.focused_panel = models::FocusPanel::SettingsPanel;
                } else if (state_.current_view == models::ViewMode::Account) {
                    state_.focused_panel = models::FocusPanel::AccountPanel;
                } else if (state_.current_view == models::ViewMode::Queue) {
                    state_.focused_panel = models::FocusPanel::QueueList;
                } else {
                    state_.focused_panel = models::FocusPanel::TrackList;
                }
                break;
            case models::FocusPanel::TrackList:
            case models::FocusPanel::QueueList:
            case models::FocusPanel::AccountPanel:
            case models::FocusPanel::SettingsPanel:
                state_.focused_panel = models::FocusPanel::Player;
                break;
            case models::FocusPanel::Player:
                state_.focused_panel = models::FocusPanel::SearchInput;
                header_view_->focus_input();
                break;
            case models::FocusPanel::HelpModal:
                break;
        }
    }
}

void App::start_worker_threads() {
    // 1. High-frequency telemetry update thread (20 Hz)
    telemetry_thread_ = std::jthread([this](std::stop_token stop_tok) {
        while (!stop_tok.stop_requested() && is_running_) {
            update_telemetry();
            screen_.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

void App::stop_worker_threads() {
    is_running_ = false;
    thread_pool_.shutdown();
    core::CoverArtProcessor::clear_terminal_image(0);
    if (mpris_server_) {
        mpris_server_->stop();
    }
    if (discord_rpc_) {
        discord_rpc_->stop();
    }
    cancel_download_all_likes();
    ui_event_channel_.close();
    if (telemetry_thread_.joinable()) {
        telemetry_thread_.request_stop();
        telemetry_thread_.join();
    }
}

void App::update_telemetry() {
    state_.playback_state = audio_.get_state();
    state_.position_ms = audio_.get_position_ms();
    state_.duration_ms = audio_.get_duration_ms();
    state_.buffer_percent = audio_.get_buffer_percent();
    state_.visualizer_bars = audio_.get_visualizer_bands();

    if (mpris_server_) {
        mpris_server_->update_playback_state(state_.playback_state);
        mpris_server_->update_position(state_.position_ms);
        mpris_server_->update_volume(state_.volume_percent);
        mpris_server_->update_shuffle(state_.queue.shuffle);
        mpris_server_->update_repeat_mode(state_.queue.repeat_mode);
    }

    if (discord_rpc_) {
        discord_rpc_->update_presence(state_.active_track, state_.playback_state, state_.position_ms, state_.duration_ms);
    }
}

void App::submit_search(const std::string& query) {
    if (query.empty()) return;
    last_search_query_ = query;
    state_.is_loading = true;
    state_.current_view = models::ViewMode::Search;
    state_.focused_panel = models::FocusPanel::TrackList;

    LOG_INFO("App", "Initiating search query: '" + query + "'");

    thread_pool_.enqueue([this, query]() {
        if (!is_running_) return;
        auto res = api_.search_tracks(query, config_.search_limit);
        if (!is_running_) return;
        if (res) {
            LOG_INFO("App", "Search returned " + std::to_string(res->size()) + " tracks");
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::SearchResultsReady,
                .tracks_payload = std::move(*res)
            });
        } else {
            LOG_ERROR("App", "Search failed: " + res.error().message);
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::ErrorMessage,
                .text_payload = "Search failed: " + res.error().message
            });
        }
        screen_.PostEvent(Event::Custom);
    });
}

void App::submit_load_view(models::ViewMode view) {
    if (state_.is_fullscreen) {
        state_.is_fullscreen = false;
        core::CoverArtProcessor::clear_terminal_image(3);
        fullscreen_cover_drawn_ = false;
    }

    if (view != models::ViewMode::Account) {
        core::CoverArtProcessor::clear_terminal_image(2);
        avatar_drawn_in_terminal_ = false;
    }

    state_.current_view = view;
    if (sidebar_view_) {
        sidebar_view_->set_selected_row(static_cast<int>(view));
    }

    // Only switch focused panel if not currently navigating the Sidebar
    if (state_.focused_panel != models::FocusPanel::Sidebar) {
        if (view == models::ViewMode::Settings) {
            state_.focused_panel = models::FocusPanel::SettingsPanel;
        } else if (view == models::ViewMode::Account) {
            state_.focused_panel = models::FocusPanel::AccountPanel;
        } else if (view == models::ViewMode::Queue) {
            state_.focused_panel = models::FocusPanel::QueueList;
        } else {
            state_.focused_panel = models::FocusPanel::TrackList;
        }
    }

    if (view == models::ViewMode::Settings) {
        state_.is_loading = false;
        if (settings_view_) {
            settings_view_->reload_from_config();
        }
        return;
    }

    if (view == models::ViewMode::Account) {
        state_.is_loading = true;
        submit_load_account_profile();
        return;
    }

    if (view == models::ViewMode::Queue) {
        state_.is_loading = false;
        return;
    }

    bool has_existing_likes = !state_.likes_tracks.empty();
    uint64_t top_like_id = has_existing_likes ? state_.likes_tracks.front().id : 0;
    std::unordered_set<uint64_t> existing_likes_set;
    if (view == models::ViewMode::Likes && has_existing_likes) {
        existing_likes_set.reserve(state_.likes_tracks.size());
        for (const auto& t : state_.likes_tracks) {
            existing_likes_set.insert(t.id);
        }
        state_.is_loading = false; // Show already loaded likes immediately
    } else {
        state_.is_loading = true;
    }

    thread_pool_.enqueue([this, view, has_existing_likes, top_like_id, existing_likes_set = std::move(existing_likes_set)]() mutable {
        if (!is_running_) return;
        switch (view) {
            case models::ViewMode::Settings:
            case models::ViewMode::Account:
                break;
            case models::ViewMode::Search:
                if (!last_search_query_.empty()) {
                    auto res = api_.search_tracks(last_search_query_, config_.search_limit);
                    if (res) {
                        ui_event_channel_.push(AsyncUIEvent{
                            .type = AsyncEventType::SearchResultsReady,
                            .tracks_payload = std::move(*res)
                        });
                    }
                }
                break;
            case models::ViewMode::Stream: {
                auto res = api_.get_user_stream(config_.search_limit);
                if (res) {
                    ui_event_channel_.push(AsyncUIEvent{
                        .type = AsyncEventType::StreamTracksReady,
                        .tracks_payload = std::move(*res)
                    });
                } else {
                    LOG_WARN("App", "Could not load user stream: " + res.error().message);
                    ui_event_channel_.push(AsyncUIEvent{
                        .type = AsyncEventType::ErrorMessage,
                        .text_payload = res.error().message
                    });
                }
                break;
            }
            case models::ViewMode::Likes: {
                uint64_t current_gen = ++likes_fetch_generation_;
                if (has_existing_likes) {
                    LOG_INFO("App", "Checking for new likes diff (top ID: " + std::to_string(top_like_id) + ")...");
                    std::vector<models::Track> diff_tracks;
                    std::string next_href = "";
                    bool hit_existing = false;

                    while (!hit_existing && is_running_ && likes_fetch_generation_.load() == current_gen) {
                        auto page_res = next_href.empty() ? api_.get_user_likes_page(50, "") : api_.get_user_likes_page(50, next_href);
                        if (!page_res || likes_fetch_generation_.load() != current_gen || !is_running_) {
                            break;
                        }
                        if (page_res->tracks.empty()) {
                            break;
                        }

                        for (auto& t : page_res->tracks) {
                            if (existing_likes_set.count(t.id) == 0) {
                                diff_tracks.push_back(std::move(t));
                            } else {
                                hit_existing = true;
                                break;
                            }
                        }

                        next_href = page_res->next_href;
                        if (hit_existing || next_href.empty()) {
                            break;
                        }
                    }

                    if (likes_fetch_generation_.load() == current_gen && is_running_) {
                        if (!diff_tracks.empty()) {
                            LOG_INFO("App", "Pre-pending " + std::to_string(diff_tracks.size()) + " newly liked track(s) diff");
                            for (const auto& dt : diff_tracks) {
                                existing_likes_set.insert(dt.id);
                            }
                            ui_event_channel_.push(AsyncUIEvent{
                                .type = AsyncEventType::LikesTracksDiffPrepended,
                                .tracks_payload = std::move(diff_tracks)
                            });
                        } else {
                            LOG_INFO("App", "Likes list is already completely up to date. No diff to load.");
                        }

                        // If pagination was paused/incomplete previously, resume loading from where it paused!
                        if (!likes_fully_fetched_.load() && !likes_resume_href_.empty()) {
                            LOG_INFO("App", "Resuming incomplete likes pagination from: " + likes_resume_href_);
                            while (!likes_resume_href_.empty() && is_running_ && likes_fetch_generation_.load() == current_gen) {
                                auto next_page = api_.get_user_likes_page(200, likes_resume_href_);
                                if (!next_page || likes_fetch_generation_.load() != current_gen || !is_running_) {
                                    break;
                                }
                                if (next_page->tracks.empty()) {
                                    likes_resume_href_.clear();
                                    break;
                                }
                                likes_resume_href_ = next_page->next_href;
                                std::vector<models::Track> new_tracks;
                                new_tracks.reserve(next_page->tracks.size());
                                for (auto& t : next_page->tracks) {
                                    if (existing_likes_set.count(t.id) == 0) {
                                        existing_likes_set.insert(t.id);
                                        new_tracks.push_back(std::move(t));
                                    }
                                }
                                if (!new_tracks.empty()) {
                                    ui_event_channel_.push(AsyncUIEvent{
                                        .type = AsyncEventType::LikesTracksAppended,
                                        .tracks_payload = std::move(new_tracks)
                                    });
                                    screen_.PostEvent(Event::Custom);
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            }
                        }

                        if (likes_fetch_generation_.load() == current_gen && is_running_) {
                            if (likes_resume_href_.empty()) {
                                likes_fully_fetched_.store(true);
                                ui_event_channel_.push(AsyncUIEvent{
                                    .type = AsyncEventType::LikesFullyLoaded
                                });
                            } else {
                                likes_fully_fetched_.store(false);
                                ui_event_channel_.push(AsyncUIEvent{
                                    .type = AsyncEventType::ErrorMessage,
                                    .text_payload = "Likes download paused at " + std::to_string(existing_likes_set.size()) +
                                                    " tracks. Press F5 or Shift+R to resume."
                                });
                            }
                            screen_.PostEvent(Event::Custom);
                        }
                    }
                } else {
                    LOG_INFO("App", "Initial load of user likes (page 1)...");
                    likes_fully_fetched_.store(false);
                    likes_resume_href_.clear();
                    auto page_res = api_.get_user_likes_page(200, "");
                    if (likes_fetch_generation_.load() != current_gen || !is_running_) {
                        return;
                    }
                    if (page_res) {
                        LOG_INFO("App", "Likes page 1 returned " + std::to_string(page_res->tracks.size()) + " tracks");
                        likes_resume_href_ = page_res->next_href;
                        size_t total_loaded = page_res->tracks.size();
                        ui_event_channel_.push(AsyncUIEvent{
                            .type = AsyncEventType::LikesTracksReady,
                            .tracks_payload = std::move(page_res->tracks)
                        });
                        screen_.PostEvent(Event::Custom);

                        // Concurrently fetch all remaining pages in batches of 200
                        while (!likes_resume_href_.empty() && is_running_ && likes_fetch_generation_.load() == current_gen) {
                            auto next_page = api_.get_user_likes_page(200, likes_resume_href_);
                            if (!next_page || likes_fetch_generation_.load() != current_gen || !is_running_) {
                                break;
                            }
                            if (next_page->tracks.empty()) {
                                likes_resume_href_.clear();
                                break;
                            }
                            likes_resume_href_ = next_page->next_href;
                            total_loaded += next_page->tracks.size();
                            ui_event_channel_.push(AsyncUIEvent{
                                .type = AsyncEventType::LikesTracksAppended,
                                .tracks_payload = std::move(next_page->tracks)
                            });
                            screen_.PostEvent(Event::Custom);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                        if (likes_fetch_generation_.load() == current_gen && is_running_) {
                            if (likes_resume_href_.empty()) {
                                likes_fully_fetched_.store(true);
                                ui_event_channel_.push(AsyncUIEvent{
                                    .type = AsyncEventType::LikesFullyLoaded
                                });
                            } else {
                                likes_fully_fetched_.store(false);
                                ui_event_channel_.push(AsyncUIEvent{
                                    .type = AsyncEventType::ErrorMessage,
                                    .text_payload = "Likes download paused at " + std::to_string(total_loaded) +
                                                    " tracks. Press F5 or Shift+R to resume."
                                });
                            }
                            screen_.PostEvent(Event::Custom);
                        }
                    } else {
                        likes_fully_fetched_.store(false);
                        likes_resume_href_.clear();
                        LOG_WARN("App", "Could not load user likes: " + page_res.error().message);
                        ui_event_channel_.push(AsyncUIEvent{
                            .type = AsyncEventType::ErrorMessage,
                            .text_payload = "Failed to load likes: " + page_res.error().message + " (Press F5 to retry)"
                        });
                        screen_.PostEvent(Event::Custom);
                    }
                }
                break;
            }
            case models::ViewMode::Playlists: {
                state_.selected_playlist.reset();
                state_.playlist_tracks.clear();
                auto res = api_.get_user_playlists(50);
                if (res) {
                    ui_event_channel_.push(AsyncUIEvent{
                        .type = AsyncEventType::PlaylistsReady,
                        .playlists_payload = std::move(*res)
                    });
                } else {
                    LOG_WARN("App", "Could not load playlists: " + res.error().message);
                    ui_event_channel_.push(AsyncUIEvent{
                        .type = AsyncEventType::ErrorMessage,
                        .text_payload = res.error().message
                    });
                }
                break;
            }
            case models::ViewMode::Queue:
                break;
        }
        if (is_running_) {
            screen_.PostEvent(Event::Custom);
        }
    });
}

void App::submit_reload_current_view(bool force_from_scratch) {
    switch (state_.current_view) {
        case models::ViewMode::Likes: {
            if (force_from_scratch) {
                state_.likes_tracks.clear();
                likes_resume_href_.clear();
                likes_fully_fetched_.store(false);
                state_.notify("Force reloading likes library from scratch...", false, 3);
            } else if (!likes_fully_fetched_.load() && !likes_resume_href_.empty()) {
                state_.notify("Resuming likes download from track #" + std::to_string(state_.likes_tracks.size() + 1) + "...", false, 3);
            } else {
                state_.notify("Checking for new likes...", false, 2);
            }
            submit_load_view(models::ViewMode::Likes);
            break;
        }
        case models::ViewMode::Playlists: {
            if (state_.selected_playlist.has_value()) {
                state_.notify("Reloading playlist: " + state_.selected_playlist->title + "...", false, 3);
                submit_load_playlist(*state_.selected_playlist);
            } else {
                if (force_from_scratch) {
                    state_.playlists.clear();
                }
                state_.notify("Refreshing playlists...", false, 2);
                submit_load_view(models::ViewMode::Playlists);
            }
            break;
        }
        case models::ViewMode::Stream: {
            if (force_from_scratch) {
                state_.stream_tracks.clear();
            }
            state_.notify("Refreshing stream...", false, 2);
            submit_load_view(models::ViewMode::Stream);
            break;
        }
        case models::ViewMode::Search: {
            if (!last_search_query_.empty()) {
                state_.notify("Refreshing search results...", false, 2);
                submit_search(last_search_query_);
            }
            break;
        }
        case models::ViewMode::Account: {
            state_.notify("Refreshing account profile...", false, 2);
            submit_load_account_profile();
            break;
        }
        case models::ViewMode::Settings: {
            if (settings_view_) {
                settings_view_->reload_from_config();
            }
            state_.notify("Settings reloaded from config", false, 2);
            break;
        }
        default:
            break;
    }
}

void App::submit_load_playlist(models::Playlist playlist) {
    state_.is_loading = true;
    state_.selected_playlist = playlist;
    state_.playlist_tracks.clear();
    state_.playlist_tracks_selected_index = 0;
    LOG_INFO("App", "Loading tracks for playlist " + std::to_string(playlist.id) + " ('" + playlist.title + "')");

    thread_pool_.enqueue([this, playlist]() {
        if (!is_running_) return;
        auto res = api_.get_playlist_tracks(playlist.id);
        if (!is_running_) return;
        if (res) {
            size_t loaded_count = res->size();
            LOG_INFO("App", "Loaded " + std::to_string(loaded_count) + " tracks for playlist " + playlist.title);
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::PlaylistTracksReady,
                .tracks_payload = std::move(*res)
            });
            if (playlist.track_count > 0 && loaded_count < playlist.track_count) {
                size_t missing = playlist.track_count - loaded_count;
                ui_event_channel_.push(AsyncUIEvent{
                    .type = AsyncEventType::InfoMessage,
                    .text_payload = "Loaded " + std::to_string(loaded_count) + "/" + std::to_string(playlist.track_count) +
                                    " tracks (" + std::to_string(missing) + " unavailable or removed). Press F5 to reload."
                });
            } else {
                ui_event_channel_.push(AsyncUIEvent{
                    .type = AsyncEventType::InfoMessage,
                    .text_payload = "Loaded playlist: " + playlist.title + " (" + std::to_string(loaded_count) + " tracks)"
                });
            }
        } else {
            LOG_ERROR("App", "Failed to load playlist tracks: " + res.error().message);
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::ErrorMessage,
                .text_payload = "Playlist error: " + res.error().message + " (Press F5 to retry)"
            });
        }
        screen_.PostEvent(Event::Custom);
    });
}

void App::submit_play_track(models::Track track) {
    if (track.is_drm_protected() && !state_.drm_engine_enabled) {
        state_.notify("[DRM Protected] '" + core::truncate_string(track.title, 25) + "' is DRM-protected: skipping (enable DRM in settings)", true, 4);
        LOG_WARN("App", "Track " + std::to_string(track.id) + " is DRM-protected: auto-skipping");
        play_next_track();
        return;
    }

    if (track.is_go_plus() || track.is_snipped) {
        state_.notify("[SoundCloud Go+] Track requires Go+ (30s preview snippet)", false, 4);
    }

    state_.active_track = track;
    state_.is_loading = true;
    if (mpris_server_) {
        mpris_server_->update_track(track);
    }
    if (discord_rpc_) {
        discord_rpc_->update_presence(track, audio::PlaybackState::Playing, 0, track.duration_ms);
    }

    LOG_INFO("App", "Play track requested: " + std::to_string(track.id) + " ('" + track.title + "') by " + track.artist);

    // If queue doesn't contain track, add it
    bool in_queue = false;
    for (size_t i = 0; i < state_.queue.tracks.size(); ++i) {
        if (state_.queue.tracks[i].id == track.id) {
            state_.queue.set_current_index(static_cast<int>(i));
            in_queue = true;
            break;
        }
    }
    if (!in_queue) {
        state_.queue.tracks.push_back(track);
        state_.queue.set_current_index(static_cast<int>(state_.queue.tracks.size() - 1));
    }

    // Asynchronously fetch cover art & extract dynamic accent color
    uint64_t track_id = track.id;
    std::string artwork_url = track.artwork_url;

    if (!artwork_url.empty()) {
        thread_pool_.enqueue([this, track_id, artwork_url]() {
            if (!is_running_) return;
            std::string high_res_url = core::CoverArtProcessor::upgrade_artwork_url(artwork_url);
            net::HttpClient client;
            client.set_timeout_seconds(5);

            LOG_INFO("App", "Fetching artwork for track " + std::to_string(track_id) + " from: " + high_res_url);
            auto res = client.get(high_res_url);
            if (!res || !res->is_2xx()) {
                if (high_res_url != artwork_url) {
                    LOG_INFO("App", "Upgraded artwork 404, trying original: " + artwork_url);
                    res = client.get(artwork_url);
                }
            }

            if (!is_running_) return;

            if (res && res->is_2xx()) {
                auto decoded = core::CoverArtProcessor::decode_and_resize(
                    reinterpret_cast<const uint8_t*>(res->body.data()),
                    res->body.size(),
                    18, 18
                );
                if (decoded && is_running_) {
                    auto dominant = core::CoverArtProcessor::extract_dominant_color(*decoded);
                    LOG_INFO("App", "Artwork processed successfully for track " + std::to_string(track_id) +
                                    ". Dominant accent color: RGB(" + std::to_string(dominant.r) + ", " +
                                    std::to_string(dominant.g) + ", " + std::to_string(dominant.b) + ")");
                    ui_event_channel_.push(AsyncUIEvent{
                        .type = AsyncEventType::CoverArtReady,
                        .numeric_payload = track_id,
                        .cover_art_payload = std::move(*decoded),
                        .accent_color_payload = dominant
                    });
                    screen_.PostEvent(Event::Custom);
                    return;
                }
            }
            LOG_WARN("App", "Failed to fetch or decode artwork for track " + std::to_string(track_id));
        });
    } else {
        state_.current_cover_art.reset();
        state_.accent_color = {255, 85, 0};
    }

    thread_pool_.enqueue([this, track]() {
        if (!is_running_) return;
        auto res = api_.resolve_stream_url(track, state_.drm_engine_enabled);
        if (!is_running_) return;
        if (res) {
            audio_.play(*res, track.id, track.duration_ms, track.is_drm_protected());
            if (cache_manager_) {
                cache_manager_->register_track(track);
            }
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::StreamResolvedAndPlay,
                .text_payload = *res,
                .numeric_payload = track.id
            });
        } else {
            LOG_ERROR("App", "Stream resolution failed for track " + std::to_string(track.id) + " ('" + track.title + "'): " + res.error().message);
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::ErrorMessage,
                .text_payload = "Playback error: " + res.error().message
            });
        }
        screen_.PostEvent(Event::Custom);
    });
}

void App::submit_toggle_like(models::Track track) {
    thread_pool_.enqueue([this, track]() {
        if (!is_running_) return;
        auto res = api_.toggle_like_track(track.id, track.is_liked);
        if (!is_running_) return;
        bool now_liked = res ? *res : !track.is_liked;
        if (cache_manager_) {
            cache_manager_->update_track_liked(track.id, now_liked);
        }
        if (res) {
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::InfoMessage,
                .text_payload = (*res ? "Added to Likes: " : "Removed from Likes: ") + core::truncate_string(track.title, 25)
            });
        } else {
            // Local like toggle fallback
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::InfoMessage,
                .text_payload = "Saved to local Likes: " + core::truncate_string(track.title, 25)
            });
        }
        screen_.PostEvent(Event::Custom);
    });
}

void App::submit_load_account_profile() {
    state_.is_loading = true;
    thread_pool_.enqueue([this]() {
        if (!is_running_) return;
        auto me_res = api_.get_me();
        if (!is_running_) return;
        if (!me_res) {
            LOG_WARN("App", "Could not fetch user profile (/me): " + me_res.error().message);
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::ErrorMessage,
                .text_payload = "Account info unavailable: " + me_res.error().message
            });
            screen_.PostEvent(Event::Custom);
            return;
        }

        models::UserProfile profile = *me_res;

        if (!profile.avatar_url.empty()) {
            std::string high_res_avatar = core::CoverArtProcessor::upgrade_artwork_url(profile.avatar_url);
            net::HttpClient client;
            client.set_timeout_seconds(5);
            auto img_res = client.get(high_res_avatar);
            if (!img_res || !img_res->is_2xx()) {
                img_res = client.get(profile.avatar_url);
            }
            if (img_res && img_res->is_2xx()) {
                auto decoded = core::CoverArtProcessor::decode_and_resize(
                    reinterpret_cast<const uint8_t*>(img_res->body.data()),
                    img_res->body.size(),
                    20, 20
                );
                if (decoded) {
                    profile.avatar_image = std::move(*decoded);
                }
            }
        }

        if (!is_running_) return;

        ui_event_channel_.push(AsyncUIEvent{
            .type = AsyncEventType::UserProfileReady,
            .profile_payload = profile
        });
        screen_.PostEvent(Event::Custom);

        auto tracks_res = api_.get_user_uploaded_tracks(profile.id, 100);
        if (!is_running_) return;
        if (tracks_res) {
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::UserTracksReady,
                .tracks_payload = std::move(*tracks_res)
            });
        }
        screen_.PostEvent(Event::Custom);
    });
}

void App::cancel_download_all_likes() {
    is_downloading_likes_ = false;
    if (likes_download_thread_.joinable()) {
        likes_download_thread_.request_stop();
        likes_download_thread_.join();
    }
}

void App::trigger_download_all_likes() {
    if (is_downloading_likes_) {
        cancel_download_all_likes();
        state_.is_downloading_likes = false;
        state_.notify("Likes downloading paused/cancelled.");
        return;
    }

    if (state_.likes_tracks.empty()) {
        state_.notify("No likes loaded yet. Open Likes tab first.", true);
        return;
    }

    is_downloading_likes_ = true;
    state_.is_downloading_likes = true;
    state_.likes_download_total = state_.likes_tracks.size();
    state_.likes_download_current = 0;
    state_.notify("Started downloading likes to cache (" + std::to_string(state_.likes_tracks.size()) + " tracks)...");

    std::vector<models::Track> to_dl = state_.likes_tracks;

    likes_download_thread_ = std::jthread([this, to_dl = std::move(to_dl)](std::stop_token stop_tok) {
        size_t done = 0;
        for (const auto& t : to_dl) {
            if (stop_tok.stop_requested() || !is_running_ || !is_downloading_likes_) {
                break;
            }

            if (cache_manager_) {
                // Ensure complete metadata and high-res cover art are always saved
                cache_manager_->register_track(t);

                // Skip Go+ tracks
                if (!t.is_go_plus()) {
                    bool allow_drm = state_.drm_engine_enabled && state_.download_drm_tracks_in_likes;
                    if (!t.is_drm_protected() || allow_drm) {
                        std::filesystem::path dest = cache_manager_->get_track_cache_path(t.id);
                        std::error_code ec;
                        if (!std::filesystem::exists(dest, ec) || std::filesystem::file_size(dest, ec) <= 1024) {
                            auto stream_res = api_.resolve_stream_url(t, allow_drm);
                            if (stream_res && !stream_res->empty() && is_running_ && is_downloading_likes_) {
                                std::filesystem::path tmp_path = dest.string() + ".download.tmp";
                                bool is_hls = (stream_res->find(".m3u8") != std::string::npos ||
                                               stream_res->find("/hls") != std::string::npos ||
                                               stream_res->find("/aac_") != std::string::npos ||
                                               stream_res->find("media-streaming.soundcloud.cloud") != std::string::npos);

                                bool download_ok = false;
                                if (is_hls) {
                                    std::string cmd = "ffmpeg -y -hide_banner -loglevel error -i \"" + *stream_res +
                                                      "\" -c:a libmp3lame -b:a 192k \"" + tmp_path.string() + "\" >/dev/null 2>&1";
                                    int rc = std::system(cmd.c_str());
                                    download_ok = (rc == 0);
                                } else {
                                    std::ofstream out_file(tmp_path, std::ios::binary);
                                    if (out_file.is_open()) {
                                        net::HttpClient dl_client;
                                        auto dl_res = dl_client.get_stream(*stream_res, [&out_file](const char* data, size_t size) {
                                            out_file.write(data, size);
                                            return true;
                                        });
                                        out_file.close();
                                        download_ok = (dl_res && dl_res->is_2xx());
                                    }
                                }

                                if (download_ok && std::filesystem::exists(tmp_path) && std::filesystem::file_size(tmp_path) > 1024) {
                                    std::filesystem::rename(tmp_path, dest, ec);
                                    cache_manager_->register_track(t);
                                } else {
                                    std::filesystem::remove(tmp_path, ec);
                                }
                            }
                        }
                    }
                }
            }

            done++;
            ui_event_channel_.push(AsyncUIEvent{
                .type = AsyncEventType::LikesDownloadProgress,
                .numeric_payload = done
            });
            screen_.PostEvent(Event::Custom);
        }

        is_downloading_likes_ = false;
        ui_event_channel_.push(AsyncUIEvent{
            .type = AsyncEventType::LikesDownloadFinished,
            .numeric_payload = done
        });
        screen_.PostEvent(Event::Custom);
    });
}

void App::play_next_track() {
    if (state_.queue.tracks.empty()) {
        const auto& current_list = track_list_view_->get_current_track_list();
        if (!current_list.empty()) {
            state_.queue.tracks = current_list;
            state_.queue.current_index = 0;
            if (state_.queue.shuffle) {
                state_.queue.rebuild_shuffle();
            }
        } else {
            state_.notify("Queue is empty");
            return;
        }
    }

    size_t skipped_drm = 0;
    const size_t total_tracks = state_.queue.tracks.size();
    std::optional<models::Track> next_playable;

    for (size_t attempt = 0; attempt < total_tracks; ++attempt) {
        state_.queue.advance_next();
        auto candidate = state_.queue.current_track();
        if (!candidate.has_value()) {
            break; // Reached end of queue without repeat
        }
        if (candidate->is_drm_protected() && !state_.drm_engine_enabled) {
            skipped_drm++;
            LOG_INFO("App", "Skipping DRM-protected track in queue: " + std::to_string(candidate->id) + " ('" + candidate->title + "')");
            continue;
        }
        next_playable = candidate;
        break;
    }

    if (next_playable) {
        if (skipped_drm > 0) {
            state_.notify("[DRM Protected] Skipped " + std::to_string(skipped_drm) + " DRM track(s)", true, 3);
        }
        submit_play_track(*next_playable);
    } else {
        if (skipped_drm > 0) {
            state_.notify("[DRM Protected] Reached end of queue (all remaining tracks DRM-protected)", true, 4);
        } else {
            state_.notify("Reached end of queue");
        }
    }
}

void App::play_previous_track() {
    if (state_.queue.tracks.empty() && !state_.active_track) {
        const auto& current_list = track_list_view_->get_current_track_list();
        if (!current_list.empty()) {
            state_.queue.tracks = current_list;
            state_.queue.current_index = 0;
            if (state_.queue.shuffle) {
                state_.queue.rebuild_shuffle();
            }
        } else {
            state_.notify("Queue is empty");
            return;
        }
    }

    // Classic music player UX:
    // If track is past the first 3 seconds (> 3000ms), restart from 00:00
    // If pressed within first 3 seconds (<= 3000ms), play previous track!
    uint64_t pos_ms = audio_.get_position_ms();
    if (pos_ms > 3000) {
        audio_.seek_absolute(0);
        state_.position_ms = 0;
        state_.notify("Restarted track (00:00)");
        return;
    }

    size_t skipped_drm = 0;
    const size_t total_tracks = state_.queue.tracks.size();
    std::optional<models::Track> prev_playable;

    for (size_t attempt = 0; attempt < total_tracks; ++attempt) {
        state_.queue.advance_prev();
        auto candidate = state_.queue.current_track();
        if (!candidate.has_value()) {
            break;
        }
        if (candidate->is_drm_protected() && !state_.drm_engine_enabled) {
            skipped_drm++;
            LOG_INFO("App", "Skipping DRM-protected track backward in queue: " + std::to_string(candidate->id) + " ('" + candidate->title + "')");
            continue;
        }
        prev_playable = candidate;
        break;
    }

    if (prev_playable) {
        if (skipped_drm > 0) {
            state_.notify("[DRM Protected] Skipped " + std::to_string(skipped_drm) + " DRM track(s)", true, 3);
        }
        submit_play_track(*prev_playable);
    } else {
        audio_.seek_absolute(0);
        state_.position_ms = 0;
        state_.notify("Restarted track (00:00)");
    }
}

void App::process_async_events() {
    while (auto opt_event = ui_event_channel_.try_pop()) {
        auto& evt = *opt_event;
        state_.is_loading = false;

        switch (evt.type) {
            case AsyncEventType::SearchResultsReady:
                state_.search_results = std::move(evt.tracks_payload);
                state_.search_selected_index = 0;
                break;
            case AsyncEventType::StreamTracksReady:
                state_.stream_tracks = std::move(evt.tracks_payload);
                state_.stream_selected_index = 0;
                break;
            case AsyncEventType::LikesTracksReady:
                state_.likes_tracks = std::move(evt.tracks_payload);
                state_.likes_selected_index = 0;
                if (cache_manager_) {
                    std::vector<uint64_t> liked_ids;
                    liked_ids.reserve(state_.likes_tracks.size());
                    for (const auto& t : state_.likes_tracks) {
                        liked_ids.push_back(t.id);
                    }
                    cache_manager_->sync_liked_tracks(liked_ids);
                }
                break;
            case AsyncEventType::LikesTracksAppended:
                if (cache_manager_) {
                    std::vector<uint64_t> liked_ids;
                    liked_ids.reserve(evt.tracks_payload.size());
                    for (const auto& t : evt.tracks_payload) {
                        liked_ids.push_back(t.id);
                    }
                    cache_manager_->sync_liked_tracks(liked_ids);
                }
                if (queue_source_is_likes_) {
                    state_.queue.append_tracks_and_update_shuffle(evt.tracks_payload);
                }
                state_.likes_tracks.insert(
                    state_.likes_tracks.end(),
                    std::make_move_iterator(evt.tracks_payload.begin()),
                    std::make_move_iterator(evt.tracks_payload.end())
                );
                break;
            case AsyncEventType::LikesTracksDiffPrepended:
                if (cache_manager_) {
                    std::vector<uint64_t> liked_ids;
                    liked_ids.reserve(evt.tracks_payload.size());
                    for (const auto& t : evt.tracks_payload) {
                        liked_ids.push_back(t.id);
                    }
                    cache_manager_->sync_liked_tracks(liked_ids);
                }
                if (queue_source_is_likes_) {
                    state_.queue.append_tracks_and_update_shuffle(evt.tracks_payload);
                }
                state_.likes_tracks.insert(
                    state_.likes_tracks.begin(),
                    std::make_move_iterator(evt.tracks_payload.begin()),
                    std::make_move_iterator(evt.tracks_payload.end())
                );
                state_.notify("Likes updated: +" + std::to_string(evt.tracks_payload.size()) + " new track(s)", false, 3);
                break;
            case AsyncEventType::LikesFullyLoaded:
                state_.is_loading = false;
                if (queue_source_is_likes_) {
                    std::string msg = "Likes library completely loaded: " + std::to_string(state_.queue.tracks.size()) + " tracks queued";
                    if (state_.queue.shuffle) {
                        msg += " & shuffled";
                    }
                    state_.notify(msg, false, 4);
                }
                break;
            case AsyncEventType::LikesDownloadProgress:
                state_.likes_download_current = static_cast<size_t>(evt.numeric_payload);
                break;
            case AsyncEventType::LikesDownloadFinished:
                state_.is_downloading_likes = false;
                state_.notify("All likes downloaded to cache (" + std::to_string(evt.numeric_payload) + " tracks)!", false, 5);
                break;
            case AsyncEventType::PlaylistsReady:
                state_.playlists = std::move(evt.playlists_payload);
                state_.playlist_selected_index = 0;
                break;
            case AsyncEventType::PlaylistTracksReady:
                state_.playlist_tracks = std::move(evt.tracks_payload);
                state_.playlist_tracks_selected_index = 0;
                break;
            case AsyncEventType::StreamResolvedAndPlay:
                state_.notify("Now playing: " + core::truncate_string(state_.active_track ? state_.active_track->title : "", 30));
                break;
            case AsyncEventType::TrackFinished:
                handle_track_finished(evt.numeric_payload);
                break;
            case AsyncEventType::WaveformReady:
                if (state_.active_track) {
                    state_.active_track->waveform_samples = std::move(evt.waveform_payload);
                }
                break;
            case AsyncEventType::CoverArtReady:
                if (state_.active_track && state_.active_track->id == evt.numeric_payload) {
                    state_.current_cover_art = std::move(evt.cover_art_payload);
                    state_.accent_color = evt.accent_color_payload;
                }
                break;
            case AsyncEventType::ErrorMessage:
                LOG_ERROR("UI", "Notification error: " + evt.text_payload);
                state_.notify(evt.text_payload, true, 6);
                break;
            case AsyncEventType::InfoMessage:
                state_.notify(evt.text_payload, false, 3);
                break;
            case AsyncEventType::ClientIdResolved:
                state_.notify("SoundCloud Client ID updated: " + evt.text_payload.substr(0, 8) + "...", false, 3);
                break;
            case AsyncEventType::MprisNext:
                play_next_track();
                break;
            case AsyncEventType::MprisPrevious:
                play_previous_track();
                break;
            case AsyncEventType::MprisPlayPause:
                audio_.toggle_playback();
                break;
            case AsyncEventType::MprisStop:
                audio_.stop();
                break;
            case AsyncEventType::UserProfileReady:
                state_.user_profile = std::move(evt.profile_payload);
                break;
            case AsyncEventType::UserTracksReady:
                state_.user_tracks = std::move(evt.tracks_payload);
                state_.user_tracks_selected_index = 0;
                break;
        }
    }
}

void App::handle_track_finished(uint64_t track_id) {
    if (state_.active_track && state_.active_track->id != track_id) {
        return; // Stale event
    }

    play_next_track();
}

void App::apply_network_settings() {
    std::string doh_url = config_.get_effective_doh_url();
    std::string dns_servers = (config_.dns_mode == core::DnsMode::Custom) ? config_.custom_dns_server : "";
    std::string proxy = config_.proxy_url;

    api_.configure_network(doh_url, dns_servers, proxy);
    LOG_INFO("App", "Applied network settings: DoH=" + (doh_url.empty() ? "disabled" : doh_url) +
                    ", DNS=" + (dns_servers.empty() ? "system" : dns_servers) +
                    ", Proxy=" + (proxy.empty() ? "none" : proxy));
}

void App::trigger_auto_extract_cookies() {
    auto res = core::CookieExtractor::extract_soundcloud_token();
    if (res.success && res.cookie) {
        config_.auth_token = res.cookie->token;
        config_.extracted_browser = res.cookie->browser_name;
        api_.set_auth_token(config_.auth_token);
        (void)config_.save();
        state_.notify("Auto-logged in via " + res.cookie->browser_name + "!", false, 5);
        if (settings_view_) {
            settings_view_->reload_from_config();
        }
    } else {
        std::string browsers_str;
        for (size_t i = 0; i < res.detected_browsers.size(); ++i) {
            if (i > 0) browsers_str += ", ";
            browsers_str += res.detected_browsers[i];
        }
        if (browsers_str.empty()) browsers_str = "No browsers detected";
        state_.notify("No login cookie found in " + browsers_str + ". Visit: https://soundcloud.com/signin", true, 6);
    }
}

bool App::handle_mouse_event(ftxui::Event event) {
    if (!config_.mouse_control || !event.is_mouse()) {
        return false;
    }

    auto& mouse = event.mouse();

    // 0. Fullscreen mode mouse handling
    if (state_.is_fullscreen) {
        int w = state_.terminal_width;
        int h = state_.terminal_height;
        int vis_h = (h >= 32) ? 3 : 2;

        if (mouse.button == ftxui::Mouse::WheelUp) {
            state_.volume_percent = std::clamp(state_.volume_percent + 5.0f, 0.0f, 150.0f);
            audio_.set_volume(state_.volume_percent);
            state_.is_muted = false;
            return true;
        }
        if (mouse.button == ftxui::Mouse::WheelDown) {
            state_.volume_percent = std::clamp(state_.volume_percent - 5.0f, 0.0f, 150.0f);
            audio_.set_volume(state_.volume_percent);
            state_.is_muted = false;
            return true;
        }

        if (mouse.button == ftxui::Mouse::Left &&
            (mouse.motion == ftxui::Mouse::Pressed || mouse.motion == ftxui::Mouse::Moved)) {
            int x = mouse.x;
            int y = mouse.y;

            // Top row exit
            if (y == 0 && x >= w - 18) {
                state_.is_fullscreen = false;
                core::CoverArtProcessor::clear_terminal_image(3);
                fullscreen_cover_drawn_ = false;
                return true;
            }

            // Timeline row: h - vis_h - 4
            if (y == h - vis_h - 4) {
                int pbar_w = std::clamp(w * 3 / 5, 24, std::max(24, w - 24));
                int pbar_x = (w - pbar_w) / 2;
                if (x >= pbar_x && x <= pbar_x + pbar_w && state_.duration_ms > 0) {
                    float ratio = static_cast<float>(x - pbar_x) / static_cast<float>(pbar_w);
                    ratio = std::clamp(ratio, 0.0f, 1.0f);
                    uint64_t target_ms = static_cast<uint64_t>(ratio * static_cast<float>(state_.duration_ms));
                    audio_.seek_absolute(target_ms);
                    state_.position_ms = target_ms;
                    return true;
                }
            }

            // Discrete clicks
            if (mouse.motion == ftxui::Mouse::Pressed) {
                // Upper body: click on cover toggles playback
                if (y >= 1 && y <= h - vis_h - 8) {
                    audio_.toggle_playback();
                    return true;
                }

                // Controls row: h - vis_h - 2
                if (y == h - vis_h - 2) {
                    bool compact_ctrls = (w < 90);
                    int w_prev = compact_ctrls ? 5 : 10;
                    int w_play = compact_ctrls ? 5 : 11;
                    int w_next = compact_ctrls ? 5 : 10;
                    int w_shuf = compact_ctrls ? 5 : 11;
                    int w_rep  = compact_ctrls ? 5 : 10;
                    int w_vol  = compact_ctrls ? 6 : 13;
                    int w_exit = compact_ctrls ? 5 : 10;
                    int gap    = compact_ctrls ? 1 : 2;

                    int total_w = w_prev + gap + w_play + gap + w_next + gap +
                                  w_shuf + gap + w_rep + gap +
                                  w_vol + gap + w_exit;
                    int start_x = (w - total_w) / 2;
                    int cur_x = start_x;

                    // Prev
                    if (x >= cur_x && x < cur_x + w_prev) {
                        play_previous_track();
                        return true;
                    }
                    cur_x += w_prev + gap;

                    // Play/Pause
                    if (x >= cur_x && x < cur_x + w_play) {
                        audio_.toggle_playback();
                        return true;
                    }
                    cur_x += w_play + gap;

                    // Next
                    if (x >= cur_x && x < cur_x + w_next) {
                        play_next_track();
                        return true;
                    }
                    cur_x += w_next + gap;

                    // Shuffle
                    if (x >= cur_x && x < cur_x + w_shuf) {
                        state_.queue.toggle_shuffle();
                        return true;
                    }
                    cur_x += w_shuf + gap;

                    // Repeat
                    if (x >= cur_x && x < cur_x + w_rep) {
                        int next_r = (static_cast<int>(state_.queue.repeat_mode) + 1) % 3;
                        state_.queue.repeat_mode = static_cast<core::RepeatMode>(next_r);
                        if (mpris_server_) mpris_server_->update_repeat_mode(state_.queue.repeat_mode);
                        return true;
                    }
                    cur_x += w_rep + gap;

                    // Volume mute/unmute
                    if (x >= cur_x && x < cur_x + w_vol) {
                        state_.is_muted = !state_.is_muted;
                        if (state_.is_muted) {
                            state_.pre_mute_volume = state_.volume_percent;
                            audio_.set_volume(0.0f);
                        } else {
                            state_.volume_percent = state_.pre_mute_volume;
                            audio_.set_volume(state_.volume_percent);
                        }
                        return true;
                    }
                    cur_x += w_vol + gap;

                    // Exit Fullscreen
                    if (x >= cur_x && x < cur_x + w_exit) {
                        state_.is_fullscreen = false;
                        core::CoverArtProcessor::clear_terminal_image(3);
                        fullscreen_cover_drawn_ = false;
                        return true;
                    }
                }
            }
        }
        return true;
    }

    // 1. Mouse wheel scrolling
    if (mouse.button == ftxui::Mouse::WheelDown) {
        if (state_.current_view == models::ViewMode::Queue) {
            queue_view_->scroll_down();
        } else if (state_.current_view == models::ViewMode::Account) {
            account_view_->scroll_down();
        } else if (state_.current_view == models::ViewMode::Settings) {
            settings_view_->get_component()->OnEvent(ftxui::Event::ArrowDown);
        } else {
            track_list_view_->scroll_down();
        }
        return true;
    }

    if (mouse.button == ftxui::Mouse::WheelUp) {
        if (state_.current_view == models::ViewMode::Queue) {
            queue_view_->scroll_up();
        } else if (state_.current_view == models::ViewMode::Account) {
            account_view_->scroll_up();
        } else if (state_.current_view == models::ViewMode::Settings) {
            settings_view_->get_component()->OnEvent(ftxui::Event::ArrowUp);
        } else {
            track_list_view_->scroll_up();
        }
        return true;
    }

    // 2. Mouse click handling
    if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed) {
        int x = mouse.x;
        int y = mouse.y;
        int w = state_.terminal_width;
        int h = state_.terminal_height;

        // Player controls bar at the bottom: 6 rows in normal mode, 5 in compact
        int player_h = state_.is_compact() ? 5 : 6;
        if (y >= h - player_h) {
            state_.focused_panel = models::FocusPanel::Player;

            // Seek bar row: h - 3
            if (y == h - 3 && x >= 8 && x <= w - 8 && state_.active_track.has_value() && state_.active_track->duration_ms > 0) {
                float ratio = static_cast<float>(x - 8) / static_cast<float>(std::max(1, w - 16));
                ratio = std::clamp(ratio, 0.0f, 1.0f);
                uint64_t target_ms = static_cast<uint64_t>(ratio * state_.active_track->duration_ms);
                audio_.seek_absolute(target_ms);
                return true;
            }

            // Controls row: h - 2
            if (y == h - 2) {
                // Play / Pause button
                if (x >= 1 && x <= 16) {
                    audio_.toggle_playback();
                    return true;
                }
                // Shuffle toggle
                if (x >= 17 && x <= 26) {
                    state_.queue.toggle_shuffle();
                    return true;
                }
                // Repeat toggle
                if (x >= 27 && x <= 38) {
                    int next_r = (static_cast<int>(state_.queue.repeat_mode) + 1) % 3;
                    state_.queue.repeat_mode = static_cast<core::RepeatMode>(next_r);
                    if (mpris_server_) mpris_server_->update_repeat_mode(state_.queue.repeat_mode);
                    return true;
                }
                // Fullscreen toggle button
                if (x >= 39 && x <= 50) {
                    state_.is_fullscreen = true;
                    core::CoverArtProcessor::clear_terminal_image(1);
                    return true;
                }
                // In compact mode, volume gauge is on the right of controls
                if (state_.is_compact() && x >= w - 24) {
                    float ratio = static_cast<float>(x - (w - 20)) / 12.0f;
                    ratio = std::clamp(ratio, 0.0f, 1.0f);
                    state_.volume_percent = ratio * 150.0f;
                    audio_.set_volume(state_.volume_percent);
                    state_.is_muted = false;
                    return true;
                }
            }

            // Volume row in normal mode: h - 5
            if (!state_.is_compact() && y == h - 5) {
                // Volume slider / gauge on the right
                if (x >= w - 24) {
                    float ratio = static_cast<float>(x - (w - 20)) / 12.0f;
                    ratio = std::clamp(ratio, 0.0f, 1.0f);
                    state_.volume_percent = ratio * 150.0f;
                    audio_.set_volume(state_.volume_percent);
                    state_.is_muted = false;
                    return true;
                }
                if (x >= w - 28 && x < w - 24) {
                    state_.is_muted = !state_.is_muted;
                    if (state_.is_muted) {
                        state_.pre_mute_volume = state_.volume_percent;
                        audio_.set_volume(0.0f);
                    } else {
                        state_.volume_percent = state_.pre_mute_volume;
                        audio_.set_volume(state_.volume_percent);
                    }
                    return true;
                }
            }

            return true;
        }

        // Header search box (top 3 rows: 0, 1, 2)
        if (y <= 2) {
            if (y == 1) {
                if (x >= 18 && x <= w - 20) {
                    state_.focused_panel = models::FocusPanel::SearchInput;
                    header_view_->focus_input();
                    return true;
                }
                if (x > w - 10) {
                    request_exit();
                    return true;
                }
                if (x > w - 20) {
                    state_.focused_panel = models::FocusPanel::HelpModal;
                    return true;
                }
            }
            return false;
        }

        // Sidebar tabs on the left
        int sidebar_width = state_.is_compact() ? 8 : 22;
        if (x <= sidebar_width) {
            int row_in_sidebar = y - 6;
            if (row_in_sidebar >= 0 && row_in_sidebar < 7) {
                models::ViewMode target_mode = static_cast<models::ViewMode>(row_in_sidebar);
                submit_load_view(target_mode);
                if (target_mode == models::ViewMode::Settings) {
                    state_.focused_panel = models::FocusPanel::SettingsPanel;
                } else if (target_mode == models::ViewMode::Account) {
                    state_.focused_panel = models::FocusPanel::AccountPanel;
                } else if (target_mode == models::ViewMode::Queue) {
                    state_.focused_panel = models::FocusPanel::QueueList;
                } else {
                    state_.focused_panel = models::FocusPanel::TrackList;
                }
                return true;
            }
            if (y >= 14 && state_.active_track.has_value()) {
                audio_.toggle_playback();
                return true;
            }
            return true;
        }

        // Main content area
        if (state_.current_view == models::ViewMode::Settings) {
            state_.focused_panel = models::FocusPanel::SettingsPanel;
            settings_view_->handle_mouse_click(y, x);
            return true;
        }

        if (state_.current_view == models::ViewMode::Account) {
            state_.focused_panel = models::FocusPanel::AccountPanel;
            if (config_.auth_token.empty()) {
                if (y >= 15 && y <= 18) {
                    trigger_auto_extract_cookies();
                } else if (y >= 12 && y <= 14) {
                    int ret = std::system("xdg-open https://soundcloud.com/signin >/dev/null 2>&1 &");
                    (void)ret;
                    state_.notify("Opened https://soundcloud.com/signin in default browser", false, 4);
                }
            } else {
                int row_idx = y - 17;
                if (row_idx >= 0) {
                    account_view_->select_row(row_idx);
                }
            }
            return true;
        }

        if (state_.current_view == models::ViewMode::Queue) {
            state_.focused_panel = models::FocusPanel::QueueList;
            int row_idx = y - 7;
            if (row_idx >= 0) {
                queue_view_->select_row(row_idx);
            }
            return true;
        }

        // Default track list view (Search, Stream, Likes, Playlists)
        state_.focused_panel = models::FocusPanel::TrackList;
        int row_idx = y - 8;
        if (row_idx >= 0) {
            track_list_view_->select_row(row_idx);
        }
        return true;
    }

    return false;
}

Element App::render_fullscreen_playback() {
    int w = state_.terminal_width;
    int h = state_.terminal_height;
    if (w <= 0 || h <= 0) return text("");

    int vis_h = (h >= 32) ? 3 : 2;
    Color accent = state_.get_accent_color();
    Color bg = Color::RGB(5, 5, 6);
    const auto& sym = ThemeSymbols::get(state_.icon_style);

    // Top Bar
    bool has_notif = state_.notification && std::chrono::steady_clock::now() < state_.notification->expires_at;
    Element notif_elem = has_notif
        ? (text(core::truncate_display_width(state_.notification->message, std::max(16, w / 2))) |
           color(state_.notification->is_error ? Color::RedLight : Color::Yellow) | bold)
        : (text("FULLSCREEN PLAYBACK") | color(Color::GrayDark) | bold);

    auto top_bar = hbox({
        text(" SCTUI ") | bold | color(accent),
        filler(),
        notif_elem,
        filler(),
        text("[ESC / F] Exit  ") | color(Color::GrayLight)
    });

    // 1. Visualizer stretched along the entire bottom edge (all columns 0 to w - 1)
    static const char* block_levels[] = {" ", " ", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    int total_levels = vis_h * 8;

    std::vector<int> col_levels(static_cast<size_t>(w), 0);
    for (int x = 0; x < w; ++x) {
        float norm_x = (w > 1) ? (static_cast<float>(x) / static_cast<float>(w - 1)) : 0.0f;
        float band_idx = norm_x * static_cast<float>(audio::VisualizerBandsCount - 1);
        int b0 = static_cast<int>(band_idx);
        int b1 = std::min(b0 + 1, static_cast<int>(audio::VisualizerBandsCount - 1));
        float frac = band_idx - static_cast<float>(b0);
        float val = (1.0f - frac) * state_.visualizer_bars[static_cast<size_t>(b0)] +
                    frac * state_.visualizer_bars[static_cast<size_t>(b1)];
        val = std::clamp(val, 0.0f, 1.0f);
        col_levels[static_cast<size_t>(x)] = static_cast<int>(std::round(val * static_cast<float>(total_levels)));
    }

    Elements vis_rows;
    vis_rows.reserve(static_cast<size_t>(vis_h));

    for (int r = 0; r < vis_h; ++r) {
        int from_bottom = (vis_h - 1) - r;
        int base = from_bottom * 8;
        Elements row_cells;
        row_cells.reserve(static_cast<size_t>(w));

        for (int x = 0; x < w; ++x) {
            int sub = col_levels[static_cast<size_t>(x)] - base;
            sub = std::clamp(sub, 0, 8);
            const char* ch = block_levels[sub];

            Color cell_col;
            if (r == 0 && sub >= 6) {
                cell_col = Color::RGB(255, 60, 0); // Peak
            } else if (r == 0) {
                cell_col = Color::RGB(255, 140, 20); // Warm
            } else if (r == 1 && vis_h > 2) {
                cell_col = accent; // Mid
            } else {
                cell_col = Color::RGB(
                    state_.accent_color.r * 3 / 4,
                    state_.accent_color.g * 3 / 4,
                    state_.accent_color.b * 3 / 4
                );
            }
            row_cells.push_back(text(ch) | color(cell_col) | bold);
        }
        vis_rows.push_back(hbox(std::move(row_cells)) | size(WIDTH, EQUAL, w));
    }
    auto vis_elem = vbox(std::move(vis_rows)) | size(WIDTH, EQUAL, w);

    // 2. Timeline
    std::string pos_str = core::format_duration_ms(state_.position_ms);
    std::string dur_str = core::format_duration_ms(state_.duration_ms);
    float progress = 0.0f;
    if (state_.duration_ms > 0) {
        progress = static_cast<float>(state_.position_ms) / static_cast<float>(state_.duration_ms);
    }
    progress = std::clamp(progress, 0.0f, 1.0f);

    int pbar_w = std::clamp(w * 3 / 5, 24, std::max(24, w - 24));
    auto pbar = gauge(progress) | color(accent) | size(WIDTH, EQUAL, pbar_w);
    auto timeline_elem = hbox({
        text(pos_str) | color(Color::White) | bold,
        text("  "),
        pbar,
        text("  "),
        text(dur_str) | color(Color::GrayLight)
    }) | center;

    // 3. Controls Row
    bool compact_ctrls = (w < 90);
    int vol_pct = static_cast<int>(std::round(state_.volume_percent));

    Element prev_btn = compact_ctrls
        ? (text("[ < ]") | color(Color::White))
        : (text("[ ⏮ Prev ]") | color(Color::White));

    std::string play_label;
    if (state_.playback_state == audio::PlaybackState::Playing) {
        play_label = compact_ctrls ? "[ || ]" : ("[ " + sym.pause + " Pause ]");
    } else {
        play_label = compact_ctrls ? "[ > ]" : ("[ " + sym.play + " Play ]");
    }
    Element play_btn = text(play_label) | color(state_.playback_state == audio::PlaybackState::Playing ? Color::Yellow : accent) | bold;

    Element next_btn = compact_ctrls
        ? (text("[ > ]") | color(Color::White))
        : (text("[ Next ⏭ ]") | color(Color::White));

    std::string shuf_label;
    if (compact_ctrls) {
        shuf_label = state_.queue.shuffle ? "[~ON]" : "[~OFF]";
    } else {
        shuf_label = state_.queue.shuffle ? ("[ " + sym.shuffle + " Shuf ]") : ("[ " + sym.shuffle + " Shuf ]");
    }
    Element shuf_btn = text(shuf_label) | color(state_.queue.shuffle ? accent : Color::GrayLight);

    std::string rep_label;
    if (compact_ctrls) {
        rep_label = (state_.queue.repeat_mode == core::RepeatMode::Off)
            ? "[R:OFF]"
            : ((state_.queue.repeat_mode == core::RepeatMode::One) ? "[R:ONE]" : "[R:ALL]");
    } else {
        rep_label = (state_.queue.repeat_mode == core::RepeatMode::Off)
            ? ("[ " + sym.repeat_off + " ]")
            : ((state_.queue.repeat_mode == core::RepeatMode::One)
                ? ("[ " + sym.repeat_one + " ]")
                : ("[ " + sym.repeat_all + " ]"));
    }
    Element rep_btn = text(rep_label) | color(state_.queue.repeat_mode != core::RepeatMode::Off ? accent : Color::GrayLight);

    std::string vol_label;
    if (compact_ctrls) {
        vol_label = state_.is_muted ? "[MUTED]" : ("[" + std::to_string(vol_pct) + "%]");
    } else {
        vol_label = state_.is_muted
            ? ("[ " + sym.muted + " Muted ]")
            : ("[ " + sym.volume + " " + std::to_string(vol_pct) + "% ]");
    }
    Element vol_btn = text(vol_label) | color(state_.is_muted ? Color::RedLight : Color::White);

    Element exit_btn = compact_ctrls
        ? (text("[ ⛶ ]") | color(Color::GrayLight))
        : (text("[ ⛶ Exit ]") | color(Color::GrayLight));

    std::string gap = compact_ctrls ? " " : "  ";

    auto controls_elem = hbox({
        prev_btn,
        text(gap),
        play_btn,
        text(gap),
        next_btn,
        text(gap),
        shuf_btn,
        text(gap),
        rep_btn,
        text(gap),
        vol_btn,
        text(gap),
        exit_btn
    }) | center;

    // 4. Track info & Cover Art
    std::string title = state_.active_track ? state_.active_track->title : "No Track Playing";
    std::string artist = (state_.active_track && !state_.active_track->artist.empty())
        ? state_.active_track->artist
        : "";

    int max_text_w = std::max(20, w - 8);
    title = core::truncate_display_width(title, max_text_w);
    artist = core::truncate_display_width(artist, max_text_w);

    // Cover art sizing
    int avail_upper_h = std::max(6, h - vis_h - 9);
    int max_cover_h_by_w = std::max(6, (w - 8) / 2);
    int cover_h = std::min(avail_upper_h - 2, max_cover_h_by_w);
    cover_h = std::clamp(cover_h, 6, 26);
    int cover_w = cover_h * 2;

    Element cover_elem;
    if (state_.current_cover_art.has_value()) {
        cover_elem = core::CoverArtProcessor::render_cover_element(
            *state_.current_cover_art,
            config_.image_protocol,
            3, // Fullscreen image id
            cover_w,
            cover_h
        );
    } else {
        cover_elem = core::CoverArtProcessor::render_placeholder(
            cover_w,
            cover_h,
            state_.accent_color
        );
    }

    auto upper_section = vbox({
        filler(),
        cover_elem | center,
        filler()
    }) | yflex;

    auto bottom_section = vbox({
        text(" "),
        text(title) | bold | color(Color::White) | center,
        artist.empty() ? text(" ") : (text(artist) | bold | color(accent) | center),
        text(" "),
        timeline_elem,
        text(" "),
        controls_elem,
        text(" "),
        vis_elem
    });

    return vbox({
        top_bar,
        upper_section,
        bottom_section
    }) | size(WIDTH, EQUAL, w) | size(HEIGHT, EQUAL, h) | bgcolor(bg);
}

} // namespace sctui::ui
