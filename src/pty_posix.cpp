#include "machinebridge/environment.hpp"
#include "machinebridge/pty.hpp"

#if !defined(_WIN32)

#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

namespace machinebridge {

void kill_process_tree(int64_t pid) {
    if (pid <= 1) return;
    // Since child called setsid(), the process group ID is equal to pid.
    // Negative pid sends signal to entire process group.
    kill(-static_cast<pid_t>(pid), SIGTERM);
    usleep(100000); // 100ms grace period
    kill(-static_cast<pid_t>(pid), SIGKILL);
    kill(static_cast<pid_t>(pid), SIGKILL);
}

class PtySessionPosix : public PtySession, public std::enable_shared_from_this<PtySessionPosix> {
public:
    PtySessionPosix(PtyOptions options, PtyCallbacks callbacks)
        : m_options(std::move(options))
        , m_callbacks(std::move(callbacks))
    {}

    ~PtySessionPosix() override {
        close();
    }

    const std::string& id() const override { return m_options.id; }
    int64_t pid() const override { return m_pid.load(); }
    bool is_running() const override { return m_running.load(); }

    void start() override {
        int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (master_fd < 0) {
            if (m_callbacks.on_error) m_callbacks.on_error("Failed to open pseudoterminal: " + std::string(strerror(errno)));
            return;
        }

        if (grantpt(master_fd) != 0 || unlockpt(master_fd) != 0) {
            ::close(master_fd);
            if (m_callbacks.on_error) m_callbacks.on_error("Failed to grant/unlock pty: " + std::string(strerror(errno)));
            return;
        }

        char* pts_name = ptsname(master_fd);
        if (!pts_name) {
            ::close(master_fd);
            if (m_callbacks.on_error) m_callbacks.on_error("Failed to get pty name: " + std::string(strerror(errno)));
            return;
        }

        m_master_fd = master_fd;

        // Set initial window size
        struct winsize ws{};
        ws.ws_col = static_cast<unsigned short>(m_options.cols);
        ws.ws_row = static_cast<unsigned short>(m_options.rows);
        ioctl(master_fd, TIOCSWINSZ, &ws);

        pid_t child_pid = fork();
        if (child_pid < 0) {
            ::close(master_fd);
            m_master_fd = -1;
            if (m_callbacks.on_error) m_callbacks.on_error("fork() failed: " + std::string(strerror(errno)));
            return;
        }

        if (child_pid == 0) {
            // Child process
            setsid();

            int slave_fd = open(pts_name, O_RDWR);
            if (slave_fd < 0) {
                _exit(1);
            }

            ioctl(slave_fd, TIOCSCTTY, 0);

            dup2(slave_fd, STDIN_FILENO);
            dup2(slave_fd, STDOUT_FILENO);
            dup2(slave_fd, STDERR_FILENO);

            ::close(slave_fd);
            ::close(master_fd);

            if (m_options.cwd && !m_options.cwd->empty()) {
                if (chdir(m_options.cwd->c_str()) != 0) {
                    // Ignore or fallback
                }
            }

            setenv("TERM", "xterm-256color", 1);
            setenv("COLORTERM", "truecolor", 1);

            auto env_info = EnvironmentDetector::detect(m_options.shell, m_options.cwd);
            if (getenv("PATH") == nullptr || getenv("PATH")[0] == '\0') {
                setenv("PATH", env_info.path_env.c_str(), 1);
            }
            if ((getenv("HOME") == nullptr || getenv("HOME")[0] == '\0') && !env_info.home_dir.empty()) {
                setenv("HOME", env_info.home_dir.c_str(), 1);
            }
            if ((getenv("TMPDIR") == nullptr || getenv("TMPDIR")[0] == '\0') && !env_info.tmp_dir.empty()) {
                setenv("TMPDIR", env_info.tmp_dir.c_str(), 1);
            }

            std::string shell = m_options.shell.value_or("");
            if (shell.empty()) {
                shell = env_info.default_shell;
            }

            std::vector<char*> args;
            args.push_back(strdup(shell.c_str()));
            for (const auto& a : m_options.args) {
                args.push_back(strdup(a.c_str()));
            }
            args.push_back(nullptr);

            execvp(shell.c_str(), args.data());
            _exit(127);
        }

        // Parent process
        m_pid.store(child_pid);
        m_running.store(true);

        // Reader thread
        m_reader_thread = std::thread([this, master_fd]() {
            char buf[4096];
            while (m_running.load()) {
                ssize_t bytes_read = read(master_fd, buf, sizeof(buf));
                if (bytes_read <= 0) {
                    break;
                }
                std::string chunk(buf, static_cast<size_t>(bytes_read));
                if (m_callbacks.on_data) m_callbacks.on_data(chunk);

                std::lock_guard<std::mutex> lock(m_listener_mutex);
                for (const auto& [_, listener] : m_data_listeners) {
                    listener(chunk);
                }
            }
        });

        // Waiter thread
        m_wait_thread = std::thread([this, child_pid]() {
            int status = 0;
            waitpid(child_pid, &status, 0);

            int exit_code = 0;
            int term_signal = 0;
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                term_signal = WTERMSIG(status);
                exit_code = 128 + term_signal;
            }

            m_running.store(false);

            if (m_callbacks.on_exit) m_callbacks.on_exit(exit_code, term_signal);

            std::lock_guard<std::mutex> lock(m_listener_mutex);
            for (const auto& [_, listener] : m_exit_listeners) {
                listener(exit_code, term_signal);
            }
        });
    }

    void write(const std::string& data) override {
        if (!m_running.load() || m_master_fd < 0) return;
        const char* ptr = data.data();
        size_t rem = data.size();
        while (rem > 0) {
            ssize_t w = ::write(m_master_fd, ptr, rem);
            if (w <= 0) break;
            rem -= static_cast<size_t>(w);
            ptr += w;
        }
    }

    void resize(int cols, int rows) override {
        if (m_master_fd < 0) return;
        struct winsize ws{};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        ioctl(m_master_fd, TIOCSWINSZ, &ws);
        if (m_pid.load() > 0) {
            kill(-static_cast<pid_t>(m_pid.load()), SIGWINCH);
        }
    }

    void signal(TerminalSignal sig) override {
        if (!m_running.load() || m_pid.load() <= 0) return;
        int s = 2; // SIGINT
        switch (sig) {
            case TerminalSignal::SigInt: s = 2; break;
            case TerminalSignal::SigTerm: s = 15; break;
            case TerminalSignal::SigHup: s = 1; break;
            case TerminalSignal::SigKill: s = 9; break;
        }
        kill(-static_cast<pid_t>(m_pid.load()), s);
    }

    void close() override {
        bool exp = true;
        if (m_running.compare_exchange_strong(exp, false)) {
            if (m_pid.load() > 0) {
                kill_process_tree(m_pid.load());
            }
        }
        if (m_master_fd >= 0) {
            ::close(m_master_fd);
            m_master_fd = -1;
        }
        if (m_reader_thread.joinable()) {
            if (m_reader_thread.get_id() != std::this_thread::get_id()) {
                m_reader_thread.join();
            } else {
                m_reader_thread.detach();
            }
        }
        if (m_wait_thread.joinable()) {
            if (m_wait_thread.get_id() != std::this_thread::get_id()) {
                m_wait_thread.join();
            } else {
                m_wait_thread.detach();
            }
        }
    }

    size_t add_data_listener(std::function<void(const std::string&)> listener) override {
        std::lock_guard<std::mutex> lock(m_listener_mutex);
        size_t id = ++m_next_listener_id;
        m_data_listeners[id] = std::move(listener);
        return id;
    }

    void remove_data_listener(size_t id) override {
        std::lock_guard<std::mutex> lock(m_listener_mutex);
        m_data_listeners.erase(id);
    }

    size_t add_exit_listener(std::function<void(int, int)> listener) override {
        std::lock_guard<std::mutex> lock(m_listener_mutex);
        size_t id = ++m_next_listener_id;
        m_exit_listeners[id] = std::move(listener);
        return id;
    }

    void remove_exit_listener(size_t id) override {
        std::lock_guard<std::mutex> lock(m_listener_mutex);
        m_exit_listeners.erase(id);
    }

private:
    PtyOptions m_options;
    PtyCallbacks m_callbacks;
    std::atomic<int64_t> m_pid{0};
    std::atomic<bool> m_running{false};
    int m_master_fd = -1;
    std::thread m_reader_thread;
    std::thread m_wait_thread;

    std::mutex m_listener_mutex;
    size_t m_next_listener_id = 0;
    std::unordered_map<size_t, std::function<void(const std::string&)>> m_data_listeners;
    std::unordered_map<size_t, std::function<void(int, int)>> m_exit_listeners;
};

// ============================================================================
// PtyManager (POSIX)
// ============================================================================

PtyManager::PtyManager(size_t max_sessions, size_t max_buffer_bytes, bool /*register_exit_hooks*/)
    : m_max_sessions(max_sessions)
    , m_max_buffer_bytes(max_buffer_bytes)
{}

PtyManager::~PtyManager() {
    close_all();
}

std::shared_ptr<PtySession> PtyManager::create(const PtyOptions& options, const PtyCallbacks& callbacks) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sessions.size() >= m_max_sessions) {
        throw std::runtime_error("MAX_SESSIONS_EXCEEDED: Reached maximum terminal sessions limit (" + std::to_string(m_max_sessions) + ")");
    }

    auto session = std::make_shared<PtySessionPosix>(options, callbacks);
    session->start();
    m_sessions[options.id] = session;
    return session;
}

std::shared_ptr<PtySession> PtyManager::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) return it->second;
    return nullptr;
}

void PtyManager::close(const std::string& id) {
    std::shared_ptr<PtySession> s;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(id);
        if (it != m_sessions.end()) {
            s = it->second;
            m_sessions.erase(it);
        }
    }
    if (s) s->close();
}

void PtyManager::close_all() {
    std::unordered_map<std::string, std::shared_ptr<PtySession>> sessions_copy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sessions_copy = std::move(m_sessions);
        m_sessions.clear();
    }
    for (auto& [_, s] : sessions_copy) {
        if (s) s->close();
    }
}

size_t PtyManager::active_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.size();
}

} // namespace machinebridge

#endif // !defined(_WIN32)
