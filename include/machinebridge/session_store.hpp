#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace machinebridge {

struct SessionRecord {
    std::string id;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point expires_at;
    std::string status = "created"; // "created", "running", "closed"
    std::optional<std::string> shell;
    std::string shell_type = "powershell";
    std::string output_buffer;
};

class InMemorySessionStore {
public:
    explicit InMemorySessionStore(size_t max_output_buffer = 4 * 1024 * 1024);

    SessionRecord create(
        uint32_t ttl_seconds = 3600,
        const std::optional<std::string>& shell = std::nullopt,
        const std::string& shell_type = "powershell"
    );

    std::optional<SessionRecord> get(const std::string& id) const;
    void set_status(const std::string& id, const std::string& status);
    void append_output(const std::string& id, const std::string& chunk);
    std::string get_output(const std::string& id) const;
    void remove(const std::string& id);
    void expire_old();
    size_t active_count() const;
    std::vector<SessionRecord> list_all() const;

private:
    size_t m_max_output_buffer;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, SessionRecord> m_sessions;
};

} // namespace machinebridge
