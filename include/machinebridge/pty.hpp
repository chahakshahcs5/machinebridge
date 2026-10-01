#pragma once

#include "machinebridge/protocol.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace machinebridge {

void kill_process_tree(int64_t pid);

struct PtyOptions {
    std::string id;
    int cols = 120;
    int rows = 40;
    std::optional<std::string> shell;
    std::vector<std::string> args;
    std::optional<std::string> cwd;
};

struct PtyCallbacks {
    std::function<void(const std::string& data)> on_data;
    std::function<void(int exit_code, int signal)> on_exit;
    std::function<void(const std::string& error)> on_error;
};

class PtySession {
public:
    virtual ~PtySession() = default;

    virtual const std::string& id() const = 0;
    virtual int64_t pid() const = 0;
    virtual bool is_running() const = 0;

    virtual void start() = 0;
    virtual void write(const std::string& data) = 0;
    virtual void resize(int cols, int rows) = 0;
    virtual void signal(TerminalSignal sig) = 0;
    virtual void close() = 0;

    virtual size_t add_data_listener(std::function<void(const std::string&)> listener) = 0;
    virtual void remove_data_listener(size_t id) = 0;

    virtual size_t add_exit_listener(std::function<void(int, int)> listener) = 0;
    virtual void remove_exit_listener(size_t id) = 0;
};

class PtyManager {
public:
    PtyManager(size_t max_sessions, size_t max_buffer_bytes, bool register_exit_hooks = true);
    ~PtyManager();

    std::shared_ptr<PtySession> create(const PtyOptions& options, const PtyCallbacks& callbacks = {});
    std::shared_ptr<PtySession> get(const std::string& id) const;
    void close(const std::string& id);
    void close_all();
    size_t active_count() const;

private:
    size_t m_max_sessions;
    size_t m_max_buffer_bytes;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<PtySession>> m_sessions;
    bool m_exit_hooks_registered = false;
};

} // namespace machinebridge
