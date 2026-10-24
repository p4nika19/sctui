#include "sctui/core/logger.hpp"
#include <iomanip>
#include <cstdlib>

namespace sctui::core {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_.is_open()) {
        log_file_.flush();
        log_file_.close();
    }
}

std::filesystem::path Logger::get_default_log_path() {
    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        return std::filesystem::path(xdg_config) / "soundcloud-tui" / "sctui.log";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home) / ".config" / "soundcloud-tui" / "sctui.log";
    }
    return std::filesystem::current_path() / "sctui.log";
}

void Logger::init(const std::filesystem::path& log_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_path_ = log_path;
    try {
        if (log_path_.has_parent_path()) {
            std::filesystem::create_directories(log_path_.parent_path());
        }
        log_file_.open(log_path_, std::ios::app);
        initialized_ = log_file_.is_open();
        if (initialized_) {
            log_file_ << "\n--- SCTUI Session Started ---\n";
            log_file_.flush();
        }
    } catch (...) {
        initialized_ = false;
    }
}

void Logger::log(LogLevel level, const std::string& tag, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        // Auto-init on first log call if not explicitly initialized
        log_path_ = get_default_log_path();
        try {
            if (log_path_.has_parent_path()) {
                std::filesystem::create_directories(log_path_.parent_path());
            }
            log_file_.open(log_path_, std::ios::app);
            initialized_ = log_file_.is_open();
        } catch (...) {
            return;
        }
    }

    if (level == LogLevel::Error) {
        last_error_ = "[" + tag + "] " + message;
    }

    if (!log_file_.is_open()) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &in_time_t);
#else
    localtime_r(&in_time_t, &tm_buf);
#endif

    const char* level_str = "INFO";
    switch (level) {
        case LogLevel::Debug: level_str = "DEBUG"; break;
        case LogLevel::Info:  level_str = "INFO "; break;
        case LogLevel::Warn:  level_str = "WARN "; break;
        case LogLevel::Error: level_str = "ERROR"; break;
    }

    std::ostringstream ss;
    ss << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
       << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
       << "[" << level_str << "] "
       << "[" << tag << "] "
       << message << "\n";

    std::string line = ss.str();
    log_file_ << line;
    if (level == LogLevel::Error || level == LogLevel::Warn) {
        log_file_.flush();
    }
}

std::string Logger::get_last_error() const {
    return last_error_;
}

} // namespace sctui::core
