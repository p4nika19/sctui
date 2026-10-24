#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <iostream>

namespace sctui::core {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

class Logger {
public:
    static Logger& instance();

    void init(const std::filesystem::path& log_path = get_default_log_path());
    void log(LogLevel level, const std::string& tag, const std::string& message);
    
    void debug(const std::string& tag, const std::string& message) { log(LogLevel::Debug, tag, message); }
    void info(const std::string& tag, const std::string& message)  { log(LogLevel::Info, tag, message); }
    void warn(const std::string& tag, const std::string& message)  { log(LogLevel::Warn, tag, message); }
    void error(const std::string& tag, const std::string& message) { log(LogLevel::Error, tag, message); }

    static std::filesystem::path get_default_log_path();

    std::string get_last_error() const;

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex mutex_;
    std::ofstream log_file_;
    std::filesystem::path log_path_;
    std::string last_error_;
    bool initialized_{false};
};

#define LOG_DEBUG(tag, msg) ::sctui::core::Logger::instance().debug(tag, msg)
#define LOG_INFO(tag, msg)  ::sctui::core::Logger::instance().info(tag, msg)
#define LOG_WARN(tag, msg)  ::sctui::core::Logger::instance().warn(tag, msg)
#define LOG_ERROR(tag, msg) ::sctui::core::Logger::instance().error(tag, msg)

} // namespace sctui::core
