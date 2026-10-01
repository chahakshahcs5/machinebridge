#include "machinebridge/logger.hpp"
#include <ctime>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace machinebridge {

Logger& Logger::instance() {
    static Logger s_instance;
    return s_instance;
}

void Logger::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = enabled;
}

bool Logger::is_enabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enabled;
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level;
}

LogLevel Logger::level() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_level;
}

void Logger::set_use_stderr(bool use_err) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_use_stderr = use_err;
}

bool Logger::use_stderr() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_use_stderr;
}

std::string Logger::level_to_string(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        default:              return "OFF";
    }
}

LogLevel Logger::string_to_level(std::string_view str) {
    if (str == "debug" || str == "DEBUG") return LogLevel::Debug;
    if (str == "info"  || str == "INFO")  return LogLevel::Info;
    if (str == "warn"  || str == "WARN" || str == "warning" || str == "WARNING")  return LogLevel::Warn;
    if (str == "error" || str == "ERROR") return LogLevel::Error;
    if (str == "off"   || str == "OFF")   return LogLevel::Off;
    return LogLevel::Info;
}

std::string Logger::current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm bt{};
#if defined(_WIN32)
    localtime_s(&bt, &in_time_t);
#else
    localtime_r(&in_time_t, &bt);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  bt.tm_year + 1900, bt.tm_mon + 1, bt.tm_mday,
                  bt.tm_hour, bt.tm_min, bt.tm_sec, static_cast<int>(ms.count()));
    return std::string(buf);
}

void Logger::log(LogLevel lvl, std::string_view tag, std::string_view message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_enabled && lvl < LogLevel::Error) return;
    if (lvl < m_level) return;

    std::string ts = current_timestamp();
    std::string lvl_str = level_to_string(lvl);
    std::ostringstream oss;
    oss << "[" << ts << "] [" << std::left << std::setw(5) << lvl_str << "] ["
        << tag << "] " << message << "\n";
    std::string formatted_line = oss.str();

    std::ostream& out = (m_use_stderr || lvl >= LogLevel::Warn) ? std::cerr : std::cout;
    out << formatted_line << std::flush;

    m_recent_logs.push_back(formatted_line);
    while (m_recent_logs.size() > MAX_RECENT_LOGS) {
        m_recent_logs.pop_front();
    }

#if defined(__ANDROID__)
    int android_priority = ANDROID_LOG_INFO;
    switch (lvl) {
        case LogLevel::Debug: android_priority = ANDROID_LOG_DEBUG; break;
        case LogLevel::Info:  android_priority = ANDROID_LOG_INFO; break;
        case LogLevel::Warn:  android_priority = ANDROID_LOG_WARN; break;
        case LogLevel::Error: android_priority = ANDROID_LOG_ERROR; break;
        default: break;
    }
    std::string tag_str = "MachineBridge:" + std::string(tag);
    std::string msg_str(message);
    __android_log_print(android_priority, tag_str.c_str(), "%s", msg_str.c_str());
#endif
}

std::string Logger::get_and_clear_recent_logs() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_recent_logs.empty()) return "";
    std::string result;
    for (const auto& line : m_recent_logs) {
        result += line;
    }
    m_recent_logs.clear();
    return result;
}

std::string Logger::get_recent_logs(size_t max_lines) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_recent_logs.empty()) return "";
    size_t start = (m_recent_logs.size() > max_lines) ? (m_recent_logs.size() - max_lines) : 0;
    std::string result;
    for (size_t i = start; i < m_recent_logs.size(); ++i) {
        result += m_recent_logs[i];
    }
    return result;
}

void Logger::debug(std::string_view tag, std::string_view message) {
    log(LogLevel::Debug, tag, message);
}

void Logger::info(std::string_view tag, std::string_view message) {
    log(LogLevel::Info, tag, message);
}

void Logger::warn(std::string_view tag, std::string_view message) {
    log(LogLevel::Warn, tag, message);
}

void Logger::error(std::string_view tag, std::string_view message) {
    log(LogLevel::Error, tag, message);
}

} // namespace machinebridge
