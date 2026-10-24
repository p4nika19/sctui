#include <iostream>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <termios.h>
#include <unistd.h>
#include "sctui/core/config.hpp"
#include "sctui/core/logger.hpp"
#include "sctui/ui/app.hpp"

namespace {
    std::atomic<bool> g_shutdown_requested{false};
    sctui::ui::App* g_app_instance{nullptr};
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
        if (sig == SIGINT || sig == SIGTERM) {
            g_shutdown_requested = true;
            if (g_app_instance) {
                g_app_instance->request_exit();
            }
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
    setup_signal_handlers();

    sctui::core::Logger::instance().init();
    LOG_INFO("Main", "sctui session initialized. Log file: " + sctui::core::Logger::get_default_log_path().string());

    auto config_res = sctui::core::Config::load();
    sctui::core::Config config;
    if (config_res) {
        config = *config_res;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            std::cout << "sctui v" << sctui::core::kVersion << " (C++23, Clang, mold, FTXUI, miniaudio)\n";
            return 0;
        } else if ((arg == "-s" || arg == "--search") && i + 1 < argc) {
            // Can pass initial search
            ++i;
        } else if ((arg == "-c" || arg == "--client-id") && i + 1 < argc) {
            config.client_id = argv[++i];
        } else if ((arg == "-t" || arg == "--token") && i + 1 < argc) {
            config.auth_token = argv[++i];
        }
    }

    try {
        sctui::ui::App app(std::move(config));
        g_app_instance = &app;

        auto run_res = app.run();
        g_app_instance = nullptr;

        restore_terminal_modes();

        if (!run_res) {
            std::cerr << "Fatal error: " << run_res.error().message << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        restore_terminal_modes();
        LOG_FATAL("Main", "Unhandled standard exception: " + std::string(e.what()));
        std::cerr << "Unhandled standard exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        restore_terminal_modes();
        LOG_FATAL("Main", "Unhandled unknown exception occurred");
        std::cerr << "Unhandled unknown exception occurred." << std::endl;
        return 1;
    }

    return 0;
}
