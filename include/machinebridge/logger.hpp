#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace machinebridge {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    Off = 4
};

class Logger {
public:
    static Logger& instance();

    void set_enabled(bool enabled);
    bool is_enabled() const;

    void set_level(LogLevel level);
    LogLevel level() const;

    void set_use_stderr(bool use_err);
    bool use_stderr() const;

    void log(LogLevel lvl, std::string_view tag, std::string_view message);

    void debug(std::string_view tag, std::string_view message);
    void info(std::string_view tag, std::string_view message);
    void warn(std::string_view tag, std::string_view message);
    void error(std::string_view tag, std::string_view message);

    std::string get_and_clear_recent_logs();
    std::string get_recent_logs(size_t max_lines = 100) const;

    static std::string level_to_string(LogLevel lvl);
    static LogLevel string_to_level(std::string_view str);
    static std::string current_timestamp();

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    mutable std::mutex m_mutex;
    bool m_enabled = true;
    LogLevel m_level = LogLevel::Info;
    bool m_use_stderr = false;

    static constexpr size_t MAX_RECENT_LOGS = 300;
    std::deque<std::string> m_recent_logs;
};

// Ergonomic variadic logging functions
template<typename... Args>
void log_debug(std::string_view tag, const Args&... args) {
    if (!Logger::instance().is_enabled() || Logger::instance().level() > LogLevel::Debug) return;
    std::ostringstream oss;
    (oss << ... << args);
    Logger::instance().debug(tag, oss.str());
}

template<typename... Args>
void log_info(std::string_view tag, const Args&... args) {
    if (!Logger::instance().is_enabled() || Logger::instance().level() > LogLevel::Info) return;
    std::ostringstream oss;
    (oss << ... << args);
    Logger::instance().info(tag, oss.str());
}

template<typename... Args>
void log_warn(std::string_view tag, const Args&... args) {
    if (!Logger::instance().is_enabled() || Logger::instance().level() > LogLevel::Warn) return;
    std::ostringstream oss;
    (oss << ... << args);
    Logger::instance().warn(tag, oss.str());
}

template<typename... Args>
void log_error(std::string_view tag, const Args&... args) {
    if (Logger::instance().level() > LogLevel::Error) return;
    std::ostringstream oss;
    (oss << ... << args);
    Logger::instance().error(tag, oss.str());
}

} // namespace machinebridge
