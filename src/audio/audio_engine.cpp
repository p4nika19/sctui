#include "sctui/audio/audio_engine.hpp"
#include "sctui/audio/drm_daemon.hpp"
#include "sctui/net/http_client.hpp"
#include "sctui/core/logger.hpp"
#include "sctui/core/track_cache.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#pragma clang diagnostic pop

#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

namespace sctui::audio {

struct AudioEngine::Impl {
    ma_device device;
    ma_decoder decoder;
    bool device_initialized{false};
    bool decoder_initialized{false};

    std::atomic<PlaybackState> state{PlaybackState::Stopped};
    std::atomic<uint64_t> current_track_id{0};
    std::atomic<uint64_t> position_ms{0};
    std::atomic<uint64_t> duration_ms{0};
    std::atomic<float> volume_percent{75.0f};
    std::atomic<float> buffer_percent{0.0f};
    std::atomic<float> rms_left{0.0f};
    std::atomic<float> rms_right{0.0f};
    std::array<std::atomic<float>, VisualizerBandsCount> visualizer_bands{};

    core::SPSCQueue<AudioCommand, 64> command_queue;
    TrackFinishedCallback finished_callback;

    std::mutex file_mutex;
    std::string current_file_path;
    std::atomic<size_t> bytes_downloaded{0};
    std::atomic<size_t> total_bytes_expected{0};
    std::atomic<bool> download_finished{false};
    std::atomic<bool> cancel_download{false};
    std::atomic<pid_t> active_ffmpeg_pid{-1};
    std::shared_ptr<core::TrackCacheManager> cache_manager;

    std::jthread download_worker;
    std::jthread command_worker;
    std::atomic<bool> stop_workers{false};

    std::unique_ptr<DrmSatelliteDaemon> drm_daemon;
    std::atomic<bool> is_playing_drm{false};

    Impl() {
        for (auto& band : visualizer_bands) {
            band.store(0.0f, std::memory_order_relaxed);
        }
    }

    ~Impl() {
        shutdown_engine();
    }

    void stop_active_ffmpeg() {
        pid_t pid = active_ffmpeg_pid.exchange(-1);
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, WNOHANG);
        }
    }

    void shutdown_engine() {
        stop_workers = true;
        cancel_download = true;
        stop_active_ffmpeg();

        if (drm_daemon) {
            drm_daemon->stop_playback();
            drm_daemon->stop();
        }

        if (download_worker.joinable()) {
            download_worker.request_stop();
            download_worker.join();
        }
        if (command_worker.joinable()) {
            command_worker.request_stop();
            command_worker.join();
        }

        if (device_initialized) {
            ma_device_uninit(&device);
            device_initialized = false;
        }

        if (decoder_initialized) {
            ma_decoder_uninit(&decoder);
            decoder_initialized = false;
        }

        state.store(PlaybackState::Stopped, std::memory_order_relaxed);
    }

    static void audio_data_callback(ma_device* pDevice, void* pOutput, const void* /*pInput*/, ma_uint32 frameCount) {
        auto* self = static_cast<Impl*>(pDevice->pUserData);
        if (!self) {
            std::memset(pOutput, 0, frameCount * sizeof(float) * 2);
            return;
        }

        if (self->is_playing_drm.load(std::memory_order_relaxed)) {
            std::memset(pOutput, 0, frameCount * sizeof(float) * 2);
            if (self->state.load(std::memory_order_relaxed) == PlaybackState::Playing) {
                float vol = self->volume_percent.load(std::memory_order_relaxed);
                float norm_vol = std::clamp(vol / 100.0f, 0.0f, 1.5f);
                float vis_scale = (norm_vol <= 0.001f) ? 0.0f : (norm_vol <= 1.0f ? std::pow(std::sin(norm_vol * 1.57079632679f), 1.35f) : 1.0f);
                uint64_t pos = self->position_ms.load(std::memory_order_relaxed);
                for (size_t b = 0; b < VisualizerBandsCount; ++b) {
                    float phase = static_cast<float>(pos % 3000) / 3000.0f * 6.2831853f + static_cast<float>(b) * 0.45f;
                    float val = (0.35f + 0.35f * std::sin(phase)) * vis_scale;
                    float old = self->visualizer_bands[b].load(std::memory_order_relaxed);
                    self->visualizer_bands[b].store(std::max(val, old * 0.82f), std::memory_order_relaxed);
                }
                self->rms_left.store(0.3f * norm_vol, std::memory_order_relaxed);
                self->rms_right.store(0.3f * norm_vol, std::memory_order_relaxed);
            } else {
                for (size_t b = 0; b < VisualizerBandsCount; ++b) {
                    float old = self->visualizer_bands[b].load(std::memory_order_relaxed);
                    self->visualizer_bands[b].store(old * 0.8f, std::memory_order_relaxed);
                }
            }
            return;
        }

        if (!self->decoder_initialized) {
            std::memset(pOutput, 0, frameCount * sizeof(float) * 2);
            return;
        }

        if (self->state.load(std::memory_order_relaxed) != PlaybackState::Playing) {
            std::memset(pOutput, 0, frameCount * sizeof(float) * 2);
            return;
        }

        float* out_buffer = static_cast<float*>(pOutput);
        ma_uint64 frames_read = 0;

        ma_result result = ma_decoder_read_pcm_frames(&self->decoder, out_buffer, frameCount, &frames_read);

        if (result != MA_SUCCESS && result != MA_AT_END) {
            std::memset(pOutput, 0, frameCount * sizeof(float) * 2);
            return;
        }

        // Check if playback needs buffering
        if (frames_read < frameCount) {
            if (!self->download_finished.load(std::memory_order_relaxed)) {
                // Reached end of currently downloaded buffer, need to buffer
                self->state.store(PlaybackState::Buffering, std::memory_order_relaxed);
            } else {
                // Natural end of track reached!
                self->state.store(PlaybackState::Finished, std::memory_order_relaxed);
                if (self->finished_callback) {
                    self->finished_callback(self->current_track_id.load(std::memory_order_relaxed));
                }
            }
            // Zero out remaining frames
            std::memset(out_buffer + frames_read * 2, 0, (frameCount - frames_read) * sizeof(float) * 2);
        }

        // Perceptual volume logarithmic curve: gain = (vol / 100)^2.2 (supports up to 150% volume)
        float vol = self->volume_percent.load(std::memory_order_relaxed);
        float norm_vol = std::clamp(vol / 100.0f, 0.0f, 1.5f);
        float gain = std::pow(norm_vol, 2.2f);

        // Visualizer sensitivity curve with diminishing returns relative to the 100% mark (norm_vol = 1.0f):
        // 1. As volume decreases from 100%, sensitivity initially shifts to keep the visualizer appearance nearly
        //    identical to 100%. As volume drops further, sensitivity changes progressively less so that at ~10% volume
        //    it remains clearly apparent that the sound is very quiet.
        // 2. As volume increases beyond 100%, visualizer sensitivity gradually decreases with diminishing returns.
        float vis_scale = 1.0f;
        if (norm_vol <= 0.001f) {
            vis_scale = 0.0f;
        } else if (norm_vol <= 1.0f) {
            vis_scale = std::pow(std::sin(norm_vol * 1.57079632679f), 1.35f);
        } else {
            vis_scale = 1.0f + std::tanh((norm_vol - 1.0f) * 0.8f) * 0.22f;
        }

        // Analysis variables
        float sum_sq_left = 0.0f;
        float sum_sq_right = 0.0f;
        std::array<float, VisualizerBandsCount> temp_bands{};
        size_t bin_size = (frames_read > 0) ? std::max<size_t>(1, frames_read / VisualizerBandsCount) : 1;

        for (ma_uint64 i = 0; i < frames_read; ++i) {
            float raw_left = out_buffer[i * 2];
            float raw_right = out_buffer[i * 2 + 1];

            float left = raw_left * gain;
            float right = raw_right * gain;
            // Soft-clipping limiter when gain > 1.0 to prevent digital distortion
            if (gain > 1.0f) {
                left = std::tanh(left);
                right = std::tanh(right);
            }

            out_buffer[i * 2] = left;
            out_buffer[i * 2 + 1] = right;

            sum_sq_left += left * left;
            sum_sq_right += right * right;

            size_t band_idx = std::min(static_cast<size_t>(i / bin_size), VisualizerBandsCount - 1);
            float raw_mag = (std::abs(raw_left) + std::abs(raw_right)) * 0.5f;
            float vis_mag = raw_mag * vis_scale;
            temp_bands[band_idx] = std::max(temp_bands[band_idx], vis_mag);
        }

        // Update RMS levels
        if (frames_read > 0) {
            self->rms_left.store(std::sqrt(sum_sq_left / static_cast<float>(frames_read)), std::memory_order_relaxed);
            self->rms_right.store(std::sqrt(sum_sq_right / static_cast<float>(frames_read)), std::memory_order_relaxed);
        }

        // Decay-smoothed visualizer spectrum update
        for (size_t b = 0; b < VisualizerBandsCount; ++b) {
            float old_val = self->visualizer_bands[b].load(std::memory_order_relaxed);
            float new_val = temp_bands[b];
            float smoothed = std::max(new_val, old_val * 0.80f);
            self->visualizer_bands[b].store(smoothed, std::memory_order_relaxed);
        }

        // Update elapsed position
        ma_uint64 cursor_frames = 0;
        if (ma_decoder_get_cursor_in_pcm_frames(&self->decoder, &cursor_frames) == MA_SUCCESS) {
            uint64_t ms = (cursor_frames * 1000) / pDevice->sampleRate;
            self->position_ms.store(ms, std::memory_order_relaxed);
        }
    }

    void handle_play_command(const AudioCommand& cmd) {
        cancel_download = true;
        stop_active_ffmpeg();
        if (download_worker.joinable()) {
            download_worker.join();
        }

        if (decoder_initialized) {
            ma_decoder_uninit(&decoder);
            decoder_initialized = false;
        }

        if (cmd.is_drm) {
            if (!drm_daemon) {
                drm_daemon = std::make_unique<DrmSatelliteDaemon>();
                drm_daemon->set_on_state([this](PlaybackState s, uint64_t pos, uint64_t dur) {
                    state.store(s, std::memory_order_relaxed);
                    if (dur > 0) duration_ms.store(dur, std::memory_order_relaxed);
                });
                drm_daemon->set_on_time_update([this](uint64_t pos, uint64_t dur) {
                    position_ms.store(pos, std::memory_order_relaxed);
                    if (dur > 0) duration_ms.store(dur, std::memory_order_relaxed);
                });
                drm_daemon->set_on_finished([this](uint64_t id) {
                    state.store(PlaybackState::Finished, std::memory_order_relaxed);
                    if (finished_callback) {
                        finished_callback(id);
                    }
                });
                drm_daemon->set_on_error([this](const std::string& err) {
                    LOG_ERROR("AudioEngine", "DRM Daemon error: " + err);
                    state.store(PlaybackState::Error, std::memory_order_relaxed);
                });
            }

            if (!drm_daemon->is_running()) {
                if (!drm_daemon->start()) {
                    LOG_ERROR("AudioEngine", "Failed to start CastLabs DRM satellite daemon");
                    state.store(PlaybackState::Error, std::memory_order_relaxed);
                    return;
                }
            }

            is_playing_drm.store(true, std::memory_order_relaxed);
            current_track_id.store(cmd.track_id, std::memory_order_relaxed);
            duration_ms.store(cmd.duration_ms, std::memory_order_relaxed);
            position_ms.store(0, std::memory_order_relaxed);
            buffer_percent.store(100.0f, std::memory_order_relaxed);
            state.store(PlaybackState::Loading, std::memory_order_relaxed);

            drm_daemon->set_volume(volume_percent.load(std::memory_order_relaxed));
            drm_daemon->play(cmd.track_id, cmd.stream_url);
            return;
        }

        if (is_playing_drm.load(std::memory_order_relaxed)) {
            if (drm_daemon) {
                drm_daemon->stop_playback();
            }
            is_playing_drm.store(false, std::memory_order_relaxed);
        }

        current_track_id.store(cmd.track_id, std::memory_order_relaxed);
        duration_ms.store(cmd.duration_ms, std::memory_order_relaxed);
        position_ms.store(0, std::memory_order_relaxed);
        buffer_percent.store(0.0f, std::memory_order_relaxed);
        state.store(PlaybackState::Loading, std::memory_order_relaxed);
        bytes_downloaded.store(0, std::memory_order_relaxed);
        total_bytes_expected.store(0, std::memory_order_relaxed);
        download_finished.store(false, std::memory_order_relaxed);
        cancel_download.store(false, std::memory_order_relaxed);

        std::string stream_url = cmd.stream_url;

        // Check if URL is local file
        if (stream_url.find("http://") != 0 && stream_url.find("https://") != 0) {
            current_file_path = stream_url;
            download_finished = true;
            buffer_percent.store(100.0f, std::memory_order_relaxed);

            ma_decoder_config decoder_cfg = ma_decoder_config_init(ma_format_f32, 2, 48000);
            if (ma_decoder_init_file(current_file_path.c_str(), &decoder_cfg, &decoder) == MA_SUCCESS) {
                decoder_initialized = true;
                ma_uint64 length_frames = 0;
                if (ma_decoder_get_length_in_pcm_frames(&decoder, &length_frames) == MA_SUCCESS && length_frames > 0) {
                    duration_ms.store((length_frames * 1000) / 48000, std::memory_order_relaxed);
                }
                state.store(PlaybackState::Playing, std::memory_order_relaxed);
                LOG_INFO("AudioEngine", "Local file playing: " + current_file_path);
            } else {
                LOG_ERROR("AudioEngine", "Failed to open local audio file: " + current_file_path);
                state.store(PlaybackState::Error, std::memory_order_relaxed);
            }
            return;
        }

        // Cache path determination
        if (cache_manager) {
            current_file_path = cache_manager->get_track_cache_path(cmd.track_id).string();
            auto cached_opt = cache_manager->get_cached_track(cmd.track_id);
            if (cached_opt.has_value() && std::filesystem::exists(*cached_opt)) {
                auto sz = std::filesystem::file_size(*cached_opt);
                if (sz > 1024) {
                    LOG_INFO("AudioEngine", "Track " + std::to_string(cmd.track_id) + " playing from cache (" +
                             std::to_string(sz) + " bytes). Playing immediately.");
                    download_finished = true;
                    buffer_percent.store(100.0f, std::memory_order_relaxed);

                    ma_decoder_config decoder_cfg = ma_decoder_config_init(ma_format_f32, 2, 48000);
                    if (ma_decoder_init_file(cached_opt->c_str(), &decoder_cfg, &decoder) == MA_SUCCESS) {
                        decoder_initialized = true;
                        ma_uint64 length_frames = 0;
                        if (ma_decoder_get_length_in_pcm_frames(&decoder, &length_frames) == MA_SUCCESS && length_frames > 0) {
                            duration_ms.store((length_frames * 1000) / 48000, std::memory_order_relaxed);
                        }
                        state.store(PlaybackState::Playing, std::memory_order_relaxed);
                        return;
                    } else {
                        LOG_WARN("AudioEngine", "Corrupt cached file detected, will re-download: " + current_file_path);
                        std::filesystem::remove(*cached_opt);
                    }
                }
            }
        } else {
            auto temp_dir = std::filesystem::temp_directory_path() / "soundcloud_tui";
            std::filesystem::create_directories(temp_dir);
            current_file_path = (temp_dir / ("track_" + std::to_string(cmd.track_id) + ".mp3")).string();

            // Check if full track already exists in disk cache
            if (std::filesystem::exists(current_file_path) && std::filesystem::file_size(current_file_path) > 100000) {
                LOG_INFO("AudioEngine", "Track " + std::to_string(cmd.track_id) + " found in local cache (" +
                         std::to_string(std::filesystem::file_size(current_file_path)) + " bytes). Playing immediately.");
                download_finished = true;
                buffer_percent.store(100.0f, std::memory_order_relaxed);

                ma_decoder_config decoder_cfg = ma_decoder_config_init(ma_format_f32, 2, 48000);
                if (ma_decoder_init_file(current_file_path.c_str(), &decoder_cfg, &decoder) == MA_SUCCESS) {
                    decoder_initialized = true;
                    ma_uint64 length_frames = 0;
                    if (ma_decoder_get_length_in_pcm_frames(&decoder, &length_frames) == MA_SUCCESS && length_frames > 0) {
                        duration_ms.store((length_frames * 1000) / 48000, std::memory_order_relaxed);
                    }
                    state.store(PlaybackState::Playing, std::memory_order_relaxed);
                    return;
                } else {
                    LOG_WARN("AudioEngine", "Corrupt cached file detected, will re-download: " + current_file_path);
                    std::filesystem::remove(current_file_path);
                }
            }
        }

        // Detect HLS stream vs direct progressive stream
        bool is_hls = (stream_url.find(".m3u8") != std::string::npos ||
                       stream_url.find("/hls") != std::string::npos ||
                       stream_url.find("/aac_") != std::string::npos ||
                       stream_url.find("media-streaming.soundcloud.cloud") != std::string::npos);

        if (is_hls) {
            download_worker = std::jthread([this, stream_url, track_id = cmd.track_id](std::stop_token stop_tok) {
                LOG_INFO("AudioEngine", "Streaming HLS track " + std::to_string(track_id) + " via ffmpeg transcoder...");

                pid_t pid = fork();
                if (pid == 0) {
                    int dev_null = open("/dev/null", O_RDWR);
                    if (dev_null >= 0) {
                        dup2(dev_null, STDIN_FILENO);
                        dup2(dev_null, STDOUT_FILENO);
                        dup2(dev_null, STDERR_FILENO);
                        close(dev_null);
                    }
                    std::vector<const char*> args = {
                        "ffmpeg",
                        "-y",
                        "-hide_banner",
                        "-loglevel", "error",
                        "-i", stream_url.c_str(),
                        "-c:a", "libmp3lame",
                        "-b:a", "192k",
                        current_file_path.c_str(),
                        nullptr
                    };
                    execvp("ffmpeg", const_cast<char* const*>(args.data()));
                    _exit(127);
                }

                if (pid < 0) {
                    LOG_ERROR("AudioEngine", "Failed to fork ffmpeg transcoder process");
                    state.store(PlaybackState::Error, std::memory_order_relaxed);
                    return;
                }

                active_ffmpeg_pid.store(pid);

                while (!stop_tok.stop_requested() && !cancel_download.load(std::memory_order_relaxed)) {
                    int status = 0;
                    pid_t res = waitpid(pid, &status, WNOHANG);
                    if (res == pid) {
                        active_ffmpeg_pid.store(-1);
                        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                            LOG_INFO("AudioEngine", "ffmpeg finished transcoding track " + std::to_string(track_id));
                            download_finished.store(true, std::memory_order_relaxed);
                            buffer_percent.store(100.0f, std::memory_order_relaxed);
                            if (cache_manager) {
                                cache_manager->register_track(track_id);
                            }
                        } else {
                            LOG_ERROR("AudioEngine", "ffmpeg exited with status code " + std::to_string(WEXITSTATUS(status)));
                            state.store(PlaybackState::Error, std::memory_order_relaxed);
                        }
                        break;
                    }

                    if (std::filesystem::exists(current_file_path)) {
                        auto sz = std::filesystem::file_size(current_file_path);
                        bytes_downloaded.store(sz, std::memory_order_relaxed);
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                if (cancel_download.load(std::memory_order_relaxed)) {
                    stop_active_ffmpeg();
                    return;
                }

                // Initialize decoder on the complete, fully-transcoded file
                if (download_finished.load(std::memory_order_relaxed) && std::filesystem::exists(current_file_path) && std::filesystem::file_size(current_file_path) > 1024) {
                    ma_decoder_config decoder_cfg = ma_decoder_config_init(ma_format_f32, 2, 48000);
                    if (ma_decoder_init_file(current_file_path.c_str(), &decoder_cfg, &decoder) == MA_SUCCESS) {
                        decoder_initialized = true;
                        ma_uint64 length_frames = 0;
                        if (ma_decoder_get_length_in_pcm_frames(&decoder, &length_frames) == MA_SUCCESS && length_frames > 0) {
                            duration_ms.store((length_frames * 1000) / 48000, std::memory_order_relaxed);
                        }
                        state.store(PlaybackState::Playing, std::memory_order_relaxed);
                        LOG_INFO("AudioEngine", "Playback started for full track " + std::to_string(track_id) + " (" + std::to_string(duration_ms.load()) + " ms, " + std::to_string(std::filesystem::file_size(current_file_path)) + " bytes)");
                    } else {
                        LOG_ERROR("AudioEngine", "ma_decoder_init_file failed on " + current_file_path);
                        state.store(PlaybackState::Error, std::memory_order_relaxed);
                    }
                }
            });
        } else {
            // Direct progressive MP3 download
            download_worker = std::jthread([this, stream_url, track_id = cmd.track_id](std::stop_token stop_tok) {
                std::ofstream out_file(current_file_path, std::ios::binary | std::ios::trunc);
                if (!out_file.is_open()) {
                    LOG_ERROR("AudioEngine", "Failed to create temp audio file: " + current_file_path);
                    state.store(PlaybackState::Error, std::memory_order_relaxed);
                    return;
                }

                net::HttpClient stream_http;
                bool decoder_started = false;

                auto on_chunk = [this, &out_file, &decoder_started, &stop_tok, track_id](const char* data, size_t size) -> bool {
                    if (stop_tok.stop_requested() || cancel_download.load(std::memory_order_relaxed)) {
                        return false;
                    }

                    out_file.write(data, static_cast<std::streamsize>(size));
                    out_file.flush();
                    bytes_downloaded.fetch_add(size, std::memory_order_relaxed);

                    size_t expected = total_bytes_expected.load(std::memory_order_relaxed);
                    if (expected > 0) {
                        float pct = (static_cast<float>(bytes_downloaded.load(std::memory_order_relaxed)) / static_cast<float>(expected)) * 100.0f;
                        buffer_percent.store(std::min(100.0f, pct), std::memory_order_relaxed);
                    }

                    if (!decoder_started && bytes_downloaded.load(std::memory_order_relaxed) >= 98304) {
                        ma_decoder_config decoder_cfg = ma_decoder_config_init(ma_format_f32, 2, 48000);
                        if (ma_decoder_init_file(current_file_path.c_str(), &decoder_cfg, &decoder) == MA_SUCCESS) {
                            decoder_initialized = true;
                            decoder_started = true;
                            state.store(PlaybackState::Playing, std::memory_order_relaxed);
                            LOG_INFO("AudioEngine", "Playback started for progressive track " + std::to_string(track_id));
                        }
                    }

                    if (decoder_started && state.load(std::memory_order_relaxed) == PlaybackState::Buffering) {
                        state.store(PlaybackState::Playing, std::memory_order_relaxed);
                    }

                    return true;
                };

                auto res = stream_http.get_stream(stream_url, on_chunk);
                out_file.close();

                if (cancel_download.load(std::memory_order_relaxed)) {
                    return;
                }

                if (res && res->headers.count("Content-Length")) {
                    try {
                        total_bytes_expected.store(std::stoul(res->headers["Content-Length"]), std::memory_order_relaxed);
                    } catch (...) {}
                }

                download_finished.store(true, std::memory_order_relaxed);
                buffer_percent.store(100.0f, std::memory_order_relaxed);
                if (cache_manager) {
                    cache_manager->register_track(track_id);
                }

                if (!decoder_started && bytes_downloaded.load(std::memory_order_relaxed) > 0) {
                    ma_decoder_config decoder_cfg = ma_decoder_config_init(ma_format_f32, 2, 48000);
                    if (ma_decoder_init_file(current_file_path.c_str(), &decoder_cfg, &decoder) == MA_SUCCESS) {
                        decoder_initialized = true;
                        state.store(PlaybackState::Playing, std::memory_order_relaxed);
                        LOG_INFO("AudioEngine", "Playback started for track " + std::to_string(track_id));
                    } else {
                        LOG_ERROR("AudioEngine", "ma_decoder_init_file failed on " + current_file_path);
                        state.store(PlaybackState::Error, std::memory_order_relaxed);
                    }
                }
            });
        }
    }

    void handle_seek(int64_t delta_ms) {
        if (!decoder_initialized) return;

        ma_uint64 cursor_frames = 0;
        if (ma_decoder_get_cursor_in_pcm_frames(&decoder, &cursor_frames) != MA_SUCCESS) return;

        int64_t current_ms = static_cast<int64_t>((cursor_frames * 1000) / 48000);
        int64_t target_ms = std::max<int64_t>(0, current_ms + delta_ms);
        int64_t total_ms = static_cast<int64_t>(duration_ms.load(std::memory_order_relaxed));
        if (total_ms > 0) {
            target_ms = std::min(target_ms, total_ms);
        }

        ma_uint64 target_frame = static_cast<ma_uint64>((target_ms * 48000) / 1000);
        ma_decoder_seek_to_pcm_frame(&decoder, target_frame);
        position_ms.store(static_cast<uint64_t>(target_ms), std::memory_order_relaxed);
    }

    void handle_seek_absolute(uint64_t target_ms) {
        if (!decoder_initialized) return;

        ma_uint64 target_frame = (target_ms * 48000) / 1000;
        ma_decoder_seek_to_pcm_frame(&decoder, target_frame);
        position_ms.store(target_ms, std::memory_order_relaxed);
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() = default;

core::Result<void> AudioEngine::init() {
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate = 48000;
    deviceConfig.dataCallback = Impl::audio_data_callback;
    deviceConfig.pUserData = impl_.get();

    if (ma_device_init(nullptr, &deviceConfig, &impl_->device) != MA_SUCCESS) {
        return core::MakeError(core::ErrorCode::AudioDeviceError, "Failed to initialize audio output device via miniaudio");
    }
    impl_->device_initialized = true;

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        return core::MakeError(core::ErrorCode::AudioDeviceError, "Failed to start miniaudio output device");
    }

    // Launch lock-free audio command processing thread
    impl_->command_worker = std::jthread([this](std::stop_token stop_tok) {
        while (!stop_tok.stop_requested() && !impl_->stop_workers) {
            auto cmd = impl_->command_queue.pop();
            if (!cmd) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            switch (cmd->type) {
                case AudioCommandType::Play:
                    impl_->handle_play_command(*cmd);
                    break;
                case AudioCommandType::Pause:
                    if (impl_->is_playing_drm.load(std::memory_order_relaxed)) {
                        if (impl_->drm_daemon) impl_->drm_daemon->pause();
                    }
                    impl_->state.store(PlaybackState::Paused, std::memory_order_relaxed);
                    break;
                case AudioCommandType::Resume:
                    if (impl_->is_playing_drm.load(std::memory_order_relaxed)) {
                        if (impl_->drm_daemon) impl_->drm_daemon->resume();
                        impl_->state.store(PlaybackState::Playing, std::memory_order_relaxed);
                    } else if (impl_->decoder_initialized) {
                        impl_->state.store(PlaybackState::Playing, std::memory_order_relaxed);
                    }
                    break;
                case AudioCommandType::TogglePause:
                    if (impl_->is_playing_drm.load(std::memory_order_relaxed)) {
                        if (impl_->state.load(std::memory_order_relaxed) == PlaybackState::Playing) {
                            if (impl_->drm_daemon) impl_->drm_daemon->pause();
                            impl_->state.store(PlaybackState::Paused, std::memory_order_relaxed);
                        } else if (impl_->state.load(std::memory_order_relaxed) == PlaybackState::Paused) {
                            if (impl_->drm_daemon) impl_->drm_daemon->resume();
                            impl_->state.store(PlaybackState::Playing, std::memory_order_relaxed);
                        }
                    } else {
                        if (impl_->state.load(std::memory_order_relaxed) == PlaybackState::Playing) {
                            impl_->state.store(PlaybackState::Paused, std::memory_order_relaxed);
                        } else if (impl_->state.load(std::memory_order_relaxed) == PlaybackState::Paused) {
                            impl_->state.store(PlaybackState::Playing, std::memory_order_relaxed);
                        }
                    }
                    break;
                case AudioCommandType::Stop:
                    if (impl_->is_playing_drm.load(std::memory_order_relaxed)) {
                        if (impl_->drm_daemon) impl_->drm_daemon->stop_playback();
                        impl_->is_playing_drm.store(false, std::memory_order_relaxed);
                    }
                    impl_->state.store(PlaybackState::Stopped, std::memory_order_relaxed);
                    impl_->cancel_download = true;
                    if (impl_->decoder_initialized) {
                        ma_decoder_seek_to_pcm_frame(&impl_->decoder, 0);
                    }
                    impl_->position_ms.store(0, std::memory_order_relaxed);
                    break;
                case AudioCommandType::SeekRelative:
                    if (impl_->is_playing_drm.load(std::memory_order_relaxed)) {
                        int64_t cur = static_cast<int64_t>(impl_->position_ms.load(std::memory_order_relaxed));
                        int64_t dur = static_cast<int64_t>(impl_->duration_ms.load(std::memory_order_relaxed));
                        int64_t target = std::clamp<int64_t>(cur + cmd->seek_delta_ms, 0, dur > 0 ? dur : 3600000);
                        if (impl_->drm_daemon) impl_->drm_daemon->seek(static_cast<uint64_t>(target));
                        impl_->position_ms.store(static_cast<uint64_t>(target), std::memory_order_relaxed);
                    } else {
                        impl_->handle_seek(cmd->seek_delta_ms);
                    }
                    break;
                case AudioCommandType::SeekAbsolute:
                    if (impl_->is_playing_drm.load(std::memory_order_relaxed)) {
                        if (impl_->drm_daemon) impl_->drm_daemon->seek(cmd->seek_target_ms);
                        impl_->position_ms.store(cmd->seek_target_ms, std::memory_order_relaxed);
                    } else {
                        impl_->handle_seek_absolute(cmd->seek_target_ms);
                    }
                    break;
                case AudioCommandType::SetVolume:
                    impl_->volume_percent.store(std::clamp(cmd->volume, 0.0f, 150.0f), std::memory_order_relaxed);
                    if (impl_->is_playing_drm.load(std::memory_order_relaxed) && impl_->drm_daemon) {
                        impl_->drm_daemon->set_volume(cmd->volume);
                    }
                    break;
                case AudioCommandType::Shutdown:
                    return;
            }
        }
    });

    return core::Ok();
}

void AudioEngine::shutdown() {
    AudioCommand cmd;
    cmd.type = AudioCommandType::Shutdown;
    impl_->command_queue.push(cmd);
    impl_->shutdown_engine();
}

void AudioEngine::play(const std::string& stream_url, uint64_t track_id, uint64_t duration_ms, bool is_drm) {
    AudioCommand cmd;
    cmd.type = AudioCommandType::Play;
    cmd.stream_url = stream_url;
    cmd.track_id = track_id;
    cmd.duration_ms = duration_ms;
    cmd.is_drm = is_drm;
    impl_->command_queue.push(cmd);
}

void AudioEngine::pause() {
    AudioCommand cmd;
    cmd.type = AudioCommandType::Pause;
    impl_->command_queue.push(cmd);
}

void AudioEngine::resume() {
    AudioCommand cmd;
    cmd.type = AudioCommandType::Resume;
    impl_->command_queue.push(cmd);
}

void AudioEngine::toggle_playback() {
    AudioCommand cmd;
    cmd.type = AudioCommandType::TogglePause;
    impl_->command_queue.push(cmd);
}

void AudioEngine::stop() {
    AudioCommand cmd;
    cmd.type = AudioCommandType::Stop;
    impl_->command_queue.push(cmd);
}

void AudioEngine::seek_relative(int64_t delta_ms) {
    AudioCommand cmd;
    cmd.type = AudioCommandType::SeekRelative;
    cmd.seek_delta_ms = delta_ms;
    impl_->command_queue.push(cmd);
}

void AudioEngine::seek_absolute(uint64_t position_ms) {
    AudioCommand cmd;
    cmd.type = AudioCommandType::SeekAbsolute;
    cmd.seek_target_ms = position_ms;
    impl_->command_queue.push(cmd);
}

void AudioEngine::set_volume(float volume_percent_0_to_150) {
    AudioCommand cmd;
    cmd.type = AudioCommandType::SetVolume;
    cmd.volume = volume_percent_0_to_150;
    impl_->command_queue.push(cmd);
}

PlaybackState AudioEngine::get_state() const noexcept {
    return impl_->state.load(std::memory_order_relaxed);
}

bool AudioEngine::is_playing() const noexcept {
    return impl_->state.load(std::memory_order_relaxed) == PlaybackState::Playing;
}

uint64_t AudioEngine::get_current_track_id() const noexcept {
    return impl_->current_track_id.load(std::memory_order_relaxed);
}

uint64_t AudioEngine::get_position_ms() const noexcept {
    return impl_->position_ms.load(std::memory_order_relaxed);
}

uint64_t AudioEngine::get_duration_ms() const noexcept {
    return impl_->duration_ms.load(std::memory_order_relaxed);
}

float AudioEngine::get_volume_linear() const noexcept {
    return impl_->volume_percent.load(std::memory_order_relaxed) / 100.0f;
}

float AudioEngine::get_volume_percent() const noexcept {
    return impl_->volume_percent.load(std::memory_order_relaxed);
}

float AudioEngine::get_buffer_percent() const noexcept {
    return impl_->buffer_percent.load(std::memory_order_relaxed);
}

std::array<float, VisualizerBandsCount> AudioEngine::get_visualizer_bands() const noexcept {
    std::array<float, VisualizerBandsCount> bands{};
    for (size_t i = 0; i < VisualizerBandsCount; ++i) {
        bands[i] = impl_->visualizer_bands[i].load(std::memory_order_relaxed);
    }
    return bands;
}

std::pair<float, float> AudioEngine::get_rms_levels() const noexcept {
    return {
        impl_->rms_left.load(std::memory_order_relaxed),
        impl_->rms_right.load(std::memory_order_relaxed)
    };
}

void AudioEngine::set_track_finished_callback(TrackFinishedCallback callback) {
    impl_->finished_callback = std::move(callback);
}

void AudioEngine::set_cache_manager(std::shared_ptr<core::TrackCacheManager> cache_mgr) {
    impl_->cache_manager = std::move(cache_mgr);
}

std::shared_ptr<core::TrackCacheManager> AudioEngine::get_cache_manager() const noexcept {
    return impl_->cache_manager;
}

} // namespace sctui::audio
