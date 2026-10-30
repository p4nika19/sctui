#include <iostream>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <thread>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include "sctui/core/config.hpp"
#include "sctui/core/logger.hpp"
#include "sctui/ui/app.hpp"

namespace {
    int g_signal_write_fd = -1;
    struct termios g_orig_termios;
    bool g_termios_saved{false};

    void restore_terminal_modes() {
        if (g_termios_saved) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        }
        // Restore cursor visibility and normal screen buffer
        std::cout << "\033[?25h\033[?1049l" << std::flush;
    }

    void signal_handler(int sig) {
        if ((sig == SIGINT || sig == SIGTERM) && g_signal_write_fd >= 0) {
            const char byte = 1;
            (void)write(g_signal_write_fd, &byte, 1);
        }
    }

    void setup_signal_handlers() {
        if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
            g_termios_saved = true;
        }

        std::atexit(restore_terminal_modes);

        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);

        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    void print_usage(const char* prog_name) {
        std::cout << "SoundCloud TUI (sctui) v" << sctui::core::kVersion << "\n"
                  << "High-performance Terminal User Interface Client for SoundCloud\n\n"
                  << "Usage: " << prog_name << " [OPTIONS]\n\n"
                  << "Options:\n"
                  << "  -s, --search <query>    Initial search query\n"
                  << "  -c, --client-id <id>    Override SoundCloud client_id\n"
                  << "  -t, --token <token>     Override user OAuth token\n"
                  << "  -h, --help              Show this help message\n"
                  << "  -v, --version           Display version info\n";
    }
}

int main(int argc, char** argv) {
    sctui::core::Logger::instance().init();
    LOG_INFO("Main", "sctui session initialized. Log file: " + sctui::core::Logger::get_default_log_path().string());

    auto config_res = sctui::core::Config::load();
    sctui::core::Config config;
    if (config_res) {
        config = *config_res;
    }

    std::string initial_search_query;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "-v" || arg == "--version") {
            std::cout << "sctui v" << sctui::core::kVersion << " (C++23, Clang, mold, FTXUI, miniaudio)\n";
            return 0;
        }

        const bool is_search = arg == "-s" || arg == "--search";
        const bool is_client_id = arg == "-c" || arg == "--client-id";
        const bool is_token = arg == "-t" || arg == "--token";
        if (!is_search && !is_client_id && !is_token) {
            std::cerr << "Unknown option: " << arg << "\n\n";
            print_usage(argv[0]);
            return 2;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for option: " << arg << "\n\n";
            print_usage(argv[0]);
            return 2;
        }

        std::string value = argv[++i];
        if (is_search) initial_search_query = std::move(value);
        else if (is_client_id) config.client_id = std::move(value);
        else config.auth_token = std::move(value);
    }

    int signal_pipe[2]{-1, -1};
    auto close_signal_pipe = [&]() {
        if (signal_pipe[0] >= 0) close(signal_pipe[0]);
        if (signal_pipe[1] >= 0) close(signal_pipe[1]);
        signal_pipe[0] = signal_pipe[1] = -1;
        g_signal_write_fd = -1;
    };

    try {
        if (pipe(signal_pipe) != 0) {
            std::cerr << "Failed to create signal wakeup pipe: " << std::strerror(errno) << std::endl;
            return 1;
        }
        g_signal_write_fd = signal_pipe[1];
        setup_signal_handlers();

        sctui::ui::App app(std::move(config), std::move(initial_search_query));
        std::jthread signal_watcher([&](std::stop_token stop_token) {
            while (!stop_token.stop_requested()) {
                pollfd pfd{signal_pipe[0], POLLIN, 0};
                int ready = poll(&pfd, 1, 200);
                if (ready > 0 && (pfd.revents & POLLIN)) {
                    char byte;
                    if (read(signal_pipe[0], &byte, 1) == 1) {
                        app.request_exit();
                        return;
                    }
                } else if (ready > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
                    return;
                } else if (ready < 0 && errno != EINTR) {
                    return;
                }
            }
        });

        auto run_res = app.run();
        signal_watcher.request_stop();
        if (signal_pipe[1] >= 0) {
            close(signal_pipe[1]);
            signal_pipe[1] = -1;
            g_signal_write_fd = -1;
        }
        signal_watcher.join();
        if (signal_pipe[0] >= 0) {
            close(signal_pipe[0]);
            signal_pipe[0] = -1;
        }

        restore_terminal_modes();
        if (!run_res) {
            std::cerr << "Fatal error: " << run_res.error().message << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        close_signal_pipe();
        restore_terminal_modes();
        LOG_ERROR("Main", "Unhandled standard exception: " + std::string(e.what()));
        std::cerr << "Unhandled standard exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        close_signal_pipe();
        restore_terminal_modes();
        LOG_ERROR("Main", "Unhandled unknown exception occurred");
        std::cerr << "Unhandled unknown exception occurred." << std::endl;
        return 1;
    }

    return 0;
}
