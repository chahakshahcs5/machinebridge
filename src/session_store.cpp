#include "machinebridge/session_store.hpp"
#include "machinebridge/shared.hpp"

namespace machinebridge {

InMemorySessionStore::InMemorySessionStore(size_t max_output_buffer)
    : m_max_output_buffer(max_output_buffer)
{}

SessionRecord InMemorySessionStore::create(
    uint32_t ttl_seconds,
    const std::optional<std::string>& shell,
    const std::string& shell_type
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::system_clock::now();
    SessionRecord record;
    record.id = create_uuid();
    record.created_at = now;
    record.expires_at = now + std::chrono::seconds(ttl_seconds);
    record.status = "created";
    record.shell = shell;
    record.shell_type = shell_type;

    m_sessions[record.id] = record;
    return record;
}

std::optional<SessionRecord> InMemorySessionStore::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) {
        return it->second;
    }
    return std::nullopt;
}

void InMemorySessionStore::set_status(const std::string& id, const std::string& status) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) {
        it->second.status = status;
    }
}

void InMemorySessionStore::append_output(const std::string& id, const std::string& chunk) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) {
        auto& buf = it->second.output_buffer;
        buf.append(chunk);
        if (buf.size() > m_max_output_buffer) {
            size_t excess = buf.size() - m_max_output_buffer;
            buf.erase(0, excess);
        }
    }
}

std::string InMemorySessionStore::get_output(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) {
        return it->second.output_buffer;
    }
    return "";
}

void InMemorySessionStore::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessions.erase(id);
}

void InMemorySessionStore::expire_old() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::system_clock::now();
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (it->second.expires_at < now) {
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

size_t InMemorySessionStore::active_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& [_, s] : m_sessions) {
        if (s.status != "closed") count++;
    }
    return count;
}

std::vector<SessionRecord> InMemorySessionStore::list_all() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SessionRecord> records;
    records.reserve(m_sessions.size());
    for (const auto& [_, s] : m_sessions) {
        records.push_back(s);
    }
    return records;
}

} // namespace machinebridge
