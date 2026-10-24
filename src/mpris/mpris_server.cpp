#include "sctui/mpris/mpris_server.hpp"
#include <systemd/sd-bus.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <poll.h>
#include <unistd.h>
#include "sctui/core/logger.hpp"

namespace sctui::mpris {

struct MprisServer::Impl {
    MprisCallbacks callbacks;
    sd_bus* bus{nullptr};
    std::recursive_mutex bus_mutex;
    std::atomic<bool> is_running{false};
    std::string service_name;
    std::jthread worker_thread;

    // Synchronized state
    std::mutex state_mutex;
    audio::PlaybackState playback_state{audio::PlaybackState::Stopped};
    std::optional<models::Track> active_track;
    uint64_t position_ms{0};
    float volume_percent{75.0f};
    bool shuffle{false};
    core::RepeatMode repeat_mode{core::RepeatMode::Off};

    explicit Impl(MprisCallbacks cb) : callbacks(std::move(cb)) {}

    ~Impl() {
        stop();
    }

    void stop() {
        if (!is_running) return;
        is_running = false;

        if (worker_thread.joinable()) {
            worker_thread.request_stop();
            worker_thread.join();
        }

        std::lock_guard<std::recursive_mutex> lock(bus_mutex);
        if (bus) {
            sd_bus_flush_close_unref(bus);
            bus = nullptr;
        }
    }

    [[nodiscard]] const char* get_playback_status_str() const {
        switch (playback_state) {
            case audio::PlaybackState::Playing:
                return "Playing";
            case audio::PlaybackState::Paused:
            case audio::PlaybackState::Buffering:
            case audio::PlaybackState::Loading:
                return "Paused";
            case audio::PlaybackState::Finished:
            case audio::PlaybackState::Error:
            case audio::PlaybackState::Stopped:
            default:
                return "Stopped";
        }
    }

    [[nodiscard]] const char* get_loop_status_str() const {
        switch (repeat_mode) {
            case core::RepeatMode::Off:
                return "None";
            case core::RepeatMode::One:
                return "Track";
            case core::RepeatMode::All:
                return "Playlist";
        }
        return "None";
    }

    void emit_property_change(const char* prop_name) {
        if (!is_running) return;
        std::lock_guard<std::recursive_mutex> lock(bus_mutex);
        if (!bus) return;
        sd_bus_emit_properties_changed(bus, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", prop_name, nullptr);
    }
};

// C Trampolines for sd-bus callbacks

static int prop_get_identity(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "s", "SoundCloud TUI");
}

static int prop_get_bool_true(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "b", 1);
}

static int prop_get_bool_false(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "b", 0);
}

static const char* const kSupportedUriSchemes[] = {"https", "http", nullptr};
static const char* const kSupportedMimeTypes[] = {"audio/mpeg", "audio/ogg", "audio/aac", nullptr};

static int prop_get_supported_uri_schemes(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append_strv(reply, const_cast<char**>(kSupportedUriSchemes));
}

static int prop_get_supported_mime_types(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append_strv(reply, const_cast<char**>(kSupportedMimeTypes));
}

static int method_raise(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, "");
}

static int method_quit(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    if (impl && impl->callbacks.on_quit) {
        impl->callbacks.on_quit();
    }
    return sd_bus_reply_method_return(m, "");
}

// Player methods
static int method_next(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    if (impl && impl->callbacks.on_next) {
        impl->callbacks.on_next();
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_previous(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    if (impl && impl->callbacks.on_prev) {
        impl->callbacks.on_prev();
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_pause(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    if (impl && impl->callbacks.on_pause) {
        impl->callbacks.on_pause();
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_play(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    if (impl && impl->callbacks.on_play) {
        impl->callbacks.on_play();
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_play_pause(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    if (impl && impl->callbacks.on_play_pause) {
        impl->callbacks.on_play_pause();
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_stop(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    if (impl && impl->callbacks.on_stop) {
        impl->callbacks.on_stop();
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_seek(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    int64_t offset_us = 0;
    if (sd_bus_message_read(m, "x", &offset_us) >= 0) {
        if (impl && impl->callbacks.on_seek_relative) {
            impl->callbacks.on_seek_relative(offset_us / 1000);
        }
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_set_position(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    const char* track_id_path = nullptr;
    int64_t pos_us = 0;
    if (sd_bus_message_read(m, "ox", &track_id_path, &pos_us) >= 0) {
        if (impl && impl->callbacks.on_seek_absolute && pos_us >= 0) {
            impl->callbacks.on_seek_absolute(static_cast<uint64_t>(pos_us / 1000));
        }
    }
    return sd_bus_reply_method_return(m, "");
}

static int prop_get_playback_status(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    return sd_bus_message_append(reply, "s", impl->get_playback_status_str());
}

static int prop_get_loop_status(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    return sd_bus_message_append(reply, "s", impl->get_loop_status_str());
}

static int prop_set_loop_status(sd_bus*, const char*, const char*, const char*, sd_bus_message* value, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    const char* str = nullptr;
    if (sd_bus_message_read(value, "s", &str) >= 0 && str) {
        core::RepeatMode mode = core::RepeatMode::Off;
        if (std::string(str) == "Track") {
            mode = core::RepeatMode::One;
        } else if (std::string(str) == "Playlist") {
            mode = core::RepeatMode::All;
        }
        if (impl && impl->callbacks.on_set_loop_status) {
            impl->callbacks.on_set_loop_status(mode);
        }
    }
    return 0;
}

static int prop_get_rate(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "d", 1.0);
}

static int prop_set_rate(sd_bus*, const char*, const char*, const char*, sd_bus_message*, void*, sd_bus_error*) {
    return 0;
}

static int prop_get_shuffle(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    return sd_bus_message_append(reply, "b", impl->shuffle ? 1 : 0);
}

static int prop_set_shuffle(sd_bus*, const char*, const char*, const char*, sd_bus_message* value, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    int b = 0;
    if (sd_bus_message_read(value, "b", &b) >= 0) {
        if (impl && impl->callbacks.on_set_shuffle) {
            impl->callbacks.on_set_shuffle(b != 0);
        }
    }
    return 0;
}

static int prop_get_volume(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    double vol = std::clamp(static_cast<double>(impl->volume_percent) / 100.0, 0.0, 1.5);
    return sd_bus_message_append(reply, "d", vol);
}

static int prop_set_volume(sd_bus*, const char*, const char*, const char*, sd_bus_message* value, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    double v = 0.0;
    if (sd_bus_message_read(value, "d", &v) >= 0) {
        if (impl && impl->callbacks.on_set_volume) {
            float pct = static_cast<float>(std::clamp(v, 0.0, 1.5) * 100.0);
            impl->callbacks.on_set_volume(pct);
        }
    }
    return 0;
}

static int prop_get_position(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    int64_t pos_us = static_cast<int64_t>(impl->position_ms) * 1000;
    return sd_bus_message_append(reply, "x", pos_us);
}

static int prop_get_metadata(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<MprisServer::Impl*>(userdata);
    std::lock_guard<std::mutex> lock(impl->state_mutex);

    sd_bus_message_open_container(reply, 'a', "{sv}");

    if (impl->active_track) {
        const auto& track = *impl->active_track;

        // mpris:trackid (object path)
        std::string track_path = "/org/mpris/MediaPlayer2/Track/" + std::to_string(track.id);
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append_basic(reply, 's', "mpris:trackid");
        sd_bus_message_open_container(reply, 'v', "o");
        sd_bus_message_append_basic(reply, 'o', track_path.c_str());
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);

        // mpris:length (int64_t in microseconds)
        int64_t length_us = static_cast<int64_t>(track.duration_ms) * 1000;
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append_basic(reply, 's', "mpris:length");
        sd_bus_message_open_container(reply, 'v', "x");
        sd_bus_message_append_basic(reply, 'x', &length_us);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);

        // xesam:title (string)
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append_basic(reply, 's', "xesam:title");
        sd_bus_message_open_container(reply, 'v', "s");
        sd_bus_message_append_basic(reply, 's', track.title.c_str());
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);

        // xesam:artist (as - array of strings)
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append_basic(reply, 's', "xesam:artist");
        sd_bus_message_open_container(reply, 'v', "as");
        sd_bus_message_open_container(reply, 'a', "s");
        sd_bus_message_append_basic(reply, 's', track.artist.c_str());
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);

        // mpris:artUrl (string)
        if (!track.artwork_url.empty()) {
            sd_bus_message_open_container(reply, 'e', "sv");
            sd_bus_message_append_basic(reply, 's', "mpris:artUrl");
            sd_bus_message_open_container(reply, 'v', "s");
            sd_bus_message_append_basic(reply, 's', track.artwork_url.c_str());
            sd_bus_message_close_container(reply);
            sd_bus_message_close_container(reply);
        }

        // xesam:url (string)
        if (!track.permalink_url.empty()) {
            sd_bus_message_open_container(reply, 'e', "sv");
            sd_bus_message_append_basic(reply, 's', "xesam:url");
            sd_bus_message_open_container(reply, 'v', "s");
            sd_bus_message_append_basic(reply, 's', track.permalink_url.c_str());
            sd_bus_message_close_container(reply);
            sd_bus_message_close_container(reply);
        }
    } else {
        // Fallback trackid: none
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append_basic(reply, 's', "mpris:trackid");
        sd_bus_message_open_container(reply, 'v', "o");
        sd_bus_message_append_basic(reply, 'o', "/org/mpris/MediaPlayer2/Track/none");
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    return sd_bus_message_close_container(reply);
}

static const sd_bus_vtable mpris_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", method_raise, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Quit", "", "", method_quit, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("CanQuit", "b", prop_get_bool_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanRaise", "b", prop_get_bool_false, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HasTrackList", "b", prop_get_bool_false, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Identity", "s", prop_get_identity, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedUriSchemes", "as", prop_get_supported_uri_schemes, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedMimeTypes", "as", prop_get_supported_mime_types, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable player_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", method_next, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Previous", "", "", method_previous, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Pause", "", "", method_pause, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PlayPause", "", "", method_play_pause, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Stop", "", "", method_stop, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Play", "", "", method_play, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Seek", "x", "", method_seek, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetPosition", "ox", "", method_set_position, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("PlaybackStatus", "s", prop_get_playback_status, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("LoopStatus", "s", prop_get_loop_status, prop_set_loop_status, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("Rate", "d", prop_get_rate, prop_set_rate, 0, 0),
    SD_BUS_WRITABLE_PROPERTY("Shuffle", "b", prop_get_shuffle, prop_set_shuffle, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Metadata", "a{sv}", prop_get_metadata, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("Volume", "d", prop_get_volume, prop_set_volume, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Position", "x", prop_get_position, 0, 0),
    SD_BUS_PROPERTY("MinimumRate", "d", prop_get_rate, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MaximumRate", "d", prop_get_rate, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanGoNext", "b", prop_get_bool_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanGoPrevious", "b", prop_get_bool_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanPlay", "b", prop_get_bool_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanPause", "b", prop_get_bool_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanSeek", "b", prop_get_bool_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanControl", "b", prop_get_bool_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("Seeked", "x", 0),
    SD_BUS_VTABLE_END
};

MprisServer::MprisServer(MprisCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(callbacks))) {}

MprisServer::~MprisServer() = default;

bool MprisServer::start(const std::string& custom_service_name) {
    if (impl_->is_running) return true;

    int r = sd_bus_default_user(&impl_->bus);
    if (r < 0) {
        LOG_WARN("MPRIS", "Failed to connect to user D-Bus: " + std::string(strerror(-r)) + ". MPRIS disabled.");
        return false;
    }

    r = sd_bus_add_object_vtable(impl_->bus, nullptr, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2", mpris_vtable, impl_.get());
    if (r < 0) {
        LOG_ERROR("MPRIS", "Failed to register org.mpris.MediaPlayer2 vtable: " + std::string(strerror(-r)));
        sd_bus_flush_close_unref(impl_->bus);
        impl_->bus = nullptr;
        return false;
    }

    r = sd_bus_add_object_vtable(impl_->bus, nullptr, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", player_vtable, impl_.get());
    if (r < 0) {
        LOG_ERROR("MPRIS", "Failed to register org.mpris.MediaPlayer2.Player vtable: " + std::string(strerror(-r)));
        sd_bus_flush_close_unref(impl_->bus);
        impl_->bus = nullptr;
        return false;
    }

    std::string s_name = custom_service_name.empty() ? "org.mpris.MediaPlayer2.sctui" : custom_service_name;
    r = sd_bus_request_name(impl_->bus, s_name.c_str(), SD_BUS_NAME_REPLACE_EXISTING);
    if (r < 0) {
        s_name = s_name + ".instance" + std::to_string(getpid());
        r = sd_bus_request_name(impl_->bus, s_name.c_str(), 0);
        if (r < 0) {
            LOG_ERROR("MPRIS", "Failed to acquire D-Bus service name: " + std::string(strerror(-r)));
            sd_bus_flush_close_unref(impl_->bus);
            impl_->bus = nullptr;
            return false;
        }
    }

    impl_->service_name = s_name;
    impl_->is_running = true;
    LOG_INFO("MPRIS", "Service registered successfully as '" + impl_->service_name + "'");

    impl_->worker_thread = std::jthread([this](std::stop_token stop_tok) {
        while (!stop_tok.stop_requested() && impl_->is_running) {
            int res = 0;
            int fd = -1;
            {
                std::lock_guard<std::recursive_mutex> lock(impl_->bus_mutex);
                if (!impl_->bus) break;
                res = sd_bus_process(impl_->bus, nullptr);
                fd = sd_bus_get_fd(impl_->bus);
            }
            if (res < 0) {
                LOG_ERROR("MPRIS", "Bus process error: " + std::string(strerror(-res)));
                break;
            }
            if (res > 0) {
                continue;
            }
            if (fd >= 0) {
                struct pollfd pfd{fd, POLLIN, 0};
                poll(&pfd, 1, 50);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });

    return true;
}

void MprisServer::stop() {
    impl_->stop();
}

void MprisServer::update_playback_state(audio::PlaybackState state) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->playback_state != state) {
            impl_->playback_state = state;
            changed = true;
        }
    }
    if (changed) {
        impl_->emit_property_change("PlaybackStatus");
    }
}

void MprisServer::update_track(const std::optional<models::Track>& track) {
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->active_track = track;
    }
    impl_->emit_property_change("Metadata");
}

void MprisServer::update_position(uint64_t position_ms) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->position_ms = position_ms;
}

void MprisServer::update_volume(float volume_percent) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (std::abs(impl_->volume_percent - volume_percent) > 0.5f) {
            impl_->volume_percent = volume_percent;
            changed = true;
        }
    }
    if (changed) {
        impl_->emit_property_change("Volume");
    }
}

void MprisServer::update_shuffle(bool shuffle) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->shuffle != shuffle) {
            impl_->shuffle = shuffle;
            changed = true;
        }
    }
    if (changed) {
        impl_->emit_property_change("Shuffle");
    }
}

void MprisServer::update_repeat_mode(core::RepeatMode mode) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->repeat_mode != mode) {
            impl_->repeat_mode = mode;
            changed = true;
        }
    }
    if (changed) {
        impl_->emit_property_change("LoopStatus");
    }
}

void MprisServer::notify_seeked(uint64_t position_ms) {
    if (!impl_->is_running) return;
    std::lock_guard<std::recursive_mutex> lock(impl_->bus_mutex);
    if (!impl_->bus) return;
    int64_t pos_us = static_cast<int64_t>(position_ms) * 1000;
    sd_bus_emit_signal(impl_->bus, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", "Seeked", "x", pos_us);
}

const std::string& MprisServer::get_service_name() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->service_name : empty;
}

} // namespace sctui::mpris
