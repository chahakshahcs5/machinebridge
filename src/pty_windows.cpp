#include "machinebridge/pty.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>

namespace machinebridge {

void kill_process_tree(int64_t pid) {
    if (pid <= 0) return;
    std::string cmd = "taskkill /F /T /PID " + std::to_string(pid) + " >nul 2>&1";
    system(cmd.c_str());
}

namespace {

std::string default_shell_windows() {
    const char* env_shell = std::getenv("MACHINEBRIDGE_DEFAULT_SHELL");
    if (env_shell && env_shell[0] != '\0') return std::string(env_shell);
    return "powershell.exe";
}

std::wstring to_wstring(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring w(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), w.data(), sz);
    return w;
}

std::string to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string s(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), s.data(), sz, nullptr, nullptr);
    return s;
}

class WindowsPtySession : public PtySession {
public:
    WindowsPtySession(
        std::string id,
        PtyOptions options,
        PtyCallbacks callbacks,
        size_t max_buffer_bytes
    )
        : m_id(std::move(id))
        , m_options(std::move(options))
        , m_callbacks(std::move(callbacks))
        , m_max_buffer_bytes(max_buffer_bytes)
    {}

    ~WindowsPtySession() override {
        close();
    }

    const std::string& id() const override { return m_id; }
    int64_t pid() const override { return m_pid; }
    bool is_running() const override { return m_running.load() && !m_closed.load(); }

    void start() override {
        if (m_started) throw std::runtime_error("PTY_ALREADY_STARTED");
        m_started = true;

        std::string shell = m_options.shell.value_or(default_shell_windows());
        std::string cmdline = shell;
        for (const auto& arg : m_options.args) {
            cmdline += " " + arg;
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hPipeInRead = INVALID_HANDLE_VALUE;
        HANDLE hPipeInWrite = INVALID_HANDLE_VALUE;
        HANDLE hPipeOutRead = INVALID_HANDLE_VALUE;
        HANDLE hPipeOutWrite = INVALID_HANDLE_VALUE;

        if (!CreatePipe(&hPipeInRead, &hPipeInWrite, &sa, 0)) {
            throw std::runtime_error("Failed to create input pipe: " + std::to_string(GetLastError()));
        }
        SetHandleInformation(hPipeInWrite, HANDLE_FLAG_INHERIT, 0);

        if (!CreatePipe(&hPipeOutRead, &hPipeOutWrite, &sa, 0)) {
            CloseHandle(hPipeInRead);
            CloseHandle(hPipeInWrite);
            throw std::runtime_error("Failed to create output pipe: " + std::to_string(GetLastError()));
        }
        SetHandleInformation(hPipeOutRead, HANDLE_FLAG_INHERIT, 0);

        m_hPipeInWrite = hPipeInWrite;
        m_hPipeOutRead = hPipeOutRead;

        COORD coord{
            static_cast<SHORT>(std::clamp(m_options.cols, 20, 500)),
            static_cast<SHORT>(std::clamp(m_options.rows, 5, 200))
        };

        HRESULT hr = CreatePseudoConsole(coord, hPipeInRead, hPipeOutWrite, 0, &m_hPC);
        if (SUCCEEDED(hr)) {
            m_using_conpty = true;
            CloseHandle(hPipeInRead);
            CloseHandle(hPipeOutWrite);

            SIZE_T attr_size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
            m_attr_list.resize(attr_size);
            auto* pAttrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_attr_list.data());
            InitializeProcThreadAttributeList(pAttrList, 1, 0, &attr_size);
            UpdateProcThreadAttribute(pAttrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, m_hPC, sizeof(HPCON), nullptr, nullptr);

            STARTUPINFOEXW siEx{};
            siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
            siEx.lpAttributeList = pAttrList;

            std::wstring wcmd = to_wstring(cmdline);
            std::vector<wchar_t> cmd_buf(wcmd.begin(), wcmd.end());
            cmd_buf.push_back(L'\0');

            std::wstring wcwd = m_options.cwd ? to_wstring(*m_options.cwd) : L"";
            const wchar_t* pCwd = wcwd.empty() ? nullptr : wcwd.c_str();

            PROCESS_INFORMATION pi{};
            BOOL ok = CreateProcessW(
                nullptr,
                cmd_buf.data(),
                nullptr,
                nullptr,
                FALSE,
                EXTENDED_STARTUPINFO_PRESENT,
                nullptr,
                pCwd,
                &siEx.StartupInfo,
                &pi
            );

            if (!ok) {
                DWORD err = GetLastError();
                cleanup_handles();
                throw std::runtime_error("CreateProcessW (ConPTY) failed: " + std::to_string(err));
            }

            m_hProcess = pi.hProcess;
            m_pid = pi.dwProcessId;
            CloseHandle(pi.hThread);
        } else {
            // Fallback to standard pipes
            m_using_conpty = false;
            STARTUPINFOW si{};
            si.cb = sizeof(STARTUPINFOW);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = hPipeInRead;
            si.hStdOutput = hPipeOutWrite;
            si.hStdError = hPipeOutWrite;

            std::wstring wcmd = to_wstring(cmdline);
            std::vector<wchar_t> cmd_buf(wcmd.begin(), wcmd.end());
            cmd_buf.push_back(L'\0');

            std::wstring wcwd = m_options.cwd ? to_wstring(*m_options.cwd) : L"";
            const wchar_t* pCwd = wcwd.empty() ? nullptr : wcwd.c_str();

            PROCESS_INFORMATION pi{};
            BOOL ok = CreateProcessW(
                nullptr,
                cmd_buf.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                pCwd,
                &si,
                &pi
            );

            CloseHandle(hPipeInRead);
            CloseHandle(hPipeOutWrite);

            if (!ok) {
                DWORD err = GetLastError();
                cleanup_handles();
                throw std::runtime_error("CreateProcessW (Pipe) failed: " + std::to_string(err));
            }

            m_hProcess = pi.hProcess;
            m_pid = pi.dwProcessId;
            CloseHandle(pi.hThread);
        }

        m_running.store(true);

        // Spawn reader thread
        m_reader_thread = std::thread([this]() {
            std::vector<char> buf(8192);
            while (m_running.load() && !m_closed.load()) {
                DWORD bytes_read = 0;
                BOOL ok = ReadFile(m_hPipeOutRead, buf.data(), static_cast<DWORD>(buf.size()), &bytes_read, nullptr);
                if (!ok || bytes_read == 0) break;

                m_buffered_bytes += bytes_read;
                if (m_buffered_bytes > m_max_buffer_bytes) {
                    if (m_callbacks.on_error) m_callbacks.on_error("PTY_OUTPUT_BUFFER_EXCEEDED");
                    close();
                    break;
                }

                std::string chunk(buf.data(), bytes_read);
                if (m_callbacks.on_data) m_callbacks.on_data(chunk);

                std::lock_guard<std::mutex> lock(m_listeners_mutex);
                for (auto& [id, listener] : m_data_listeners) {
                    if (listener) listener(chunk);
                }
            }
        });

        // Spawn exit watcher thread
        m_watcher_thread = std::thread([this]() {
            if (m_hProcess && m_hProcess != INVALID_HANDLE_VALUE) {
                WaitForSingleObject(m_hProcess, INFINITE);
                DWORD exit_code = 0;
                GetExitCodeProcess(m_hProcess, &exit_code);

                m_running.store(false);
                int ec = static_cast<int>(exit_code);

                if (m_callbacks.on_exit) m_callbacks.on_exit(ec, 0);

                std::lock_guard<std::mutex> lock(m_listeners_mutex);
                for (auto& [id, listener] : m_exit_listeners) {
                    if (listener) listener(ec, 0);
                }
            }
        });
    }

    void write(const std::string& data) override {
        if (!m_running.load() || m_closed.load() || m_hPipeInWrite == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("PTY_NOT_RUNNING");
        }
        DWORD written = 0;
        WriteFile(m_hPipeInWrite, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    }

    void resize(int cols, int rows) override {
        if (!m_running.load() || m_closed.load()) return;
        if (m_using_conpty && m_hPC) {
            COORD coord{
                static_cast<SHORT>(std::clamp(cols, 20, 500)),
                static_cast<SHORT>(std::clamp(rows, 5, 200))
            };
            ResizePseudoConsole(m_hPC, coord);
        }
    }

    void signal(TerminalSignal sig) override {
        if (!m_running.load() || m_closed.load()) return;
        if (sig == TerminalSignal::SigInt) {
            // Write Ctrl+C to terminal input
            write("\x03");
        } else if (sig == TerminalSignal::SigKill || sig == TerminalSignal::SigTerm) {
            close();
        }
    }

    void close() override {
        bool expected = false;
        if (!m_closed.compare_exchange_strong(expected, true)) return;
        m_running.store(false);

        if (m_hPipeInWrite != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hPipeInWrite);
            m_hPipeInWrite = INVALID_HANDLE_VALUE;
        }

        if (m_using_conpty && m_hPC) {
            ClosePseudoConsole(m_hPC);
            m_hPC = nullptr;
        }

        if (m_pid > 0) {
            kill_process_tree(m_pid);
        }

        if (m_hProcess && m_hProcess != INVALID_HANDLE_VALUE) {
            TerminateProcess(m_hProcess, 1);
            CloseHandle(m_hProcess);
            m_hProcess = INVALID_HANDLE_VALUE;
        }

        if (m_hPipeOutRead != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hPipeOutRead);
            m_hPipeOutRead = INVALID_HANDLE_VALUE;
        }

        if (m_reader_thread.joinable() && m_reader_thread.get_id() != std::this_thread::get_id()) {
            m_reader_thread.join();
        }
        if (m_watcher_thread.joinable() && m_watcher_thread.get_id() != std::this_thread::get_id()) {
            m_watcher_thread.join();
        }
    }

    size_t add_data_listener(std::function<void(const std::string&)> listener) override {
        std::lock_guard<std::mutex> lock(m_listeners_mutex);
        size_t id = m_next_listener_id++;
        m_data_listeners[id] = std::move(listener);
        return id;
    }

    void remove_data_listener(size_t id) override {
        std::lock_guard<std::mutex> lock(m_listeners_mutex);
        m_data_listeners.erase(id);
    }

    size_t add_exit_listener(std::function<void(int, int)> listener) override {
        std::lock_guard<std::mutex> lock(m_listeners_mutex);
        size_t id = m_next_listener_id++;
        m_exit_listeners[id] = std::move(listener);
        return id;
    }

    void remove_exit_listener(size_t id) override {
        std::lock_guard<std::mutex> lock(m_listeners_mutex);
        m_exit_listeners.erase(id);
    }

private:
    void cleanup_handles() {
        if (m_hPipeInWrite != INVALID_HANDLE_VALUE) { CloseHandle(m_hPipeInWrite); m_hPipeInWrite = INVALID_HANDLE_VALUE; }
        if (m_hPipeOutRead != INVALID_HANDLE_VALUE) { CloseHandle(m_hPipeOutRead); m_hPipeOutRead = INVALID_HANDLE_VALUE; }
        if (m_hPC) { ClosePseudoConsole(m_hPC); m_hPC = nullptr; }
    }

    std::string m_id;
    PtyOptions m_options;
    PtyCallbacks m_callbacks;
    size_t m_max_buffer_bytes;

    bool m_started = false;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_closed{false};
    int64_t m_pid = 0;
    size_t m_buffered_bytes = 0;
    bool m_using_conpty = false;

    HANDLE m_hPipeInWrite = INVALID_HANDLE_VALUE;
    HANDLE m_hPipeOutRead = INVALID_HANDLE_VALUE;
    HANDLE m_hProcess = INVALID_HANDLE_VALUE;
    HPCON m_hPC = nullptr;
    std::vector<uint8_t> m_attr_list;

    std::thread m_reader_thread;
    std::thread m_watcher_thread;

    std::mutex m_listeners_mutex;
    size_t m_next_listener_id = 1;
    std::unordered_map<size_t, std::function<void(const std::string&)>> m_data_listeners;
    std::unordered_map<size_t, std::function<void(int, int)>> m_exit_listeners;
};

static PtyManager* g_pty_manager_instance = nullptr;

BOOL WINAPI console_ctrl_handler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        if (g_pty_manager_instance) {
            g_pty_manager_instance->close_all();
        }
    }
    return FALSE;
}

} // namespace

PtyManager::PtyManager(size_t max_sessions, size_t max_buffer_bytes, bool register_exit_hooks)
    : m_max_sessions(max_sessions)
    , m_max_buffer_bytes(max_buffer_bytes)
{
    if (register_exit_hooks) {
        g_pty_manager_instance = this;
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
        m_exit_hooks_registered = true;
    }
}

PtyManager::~PtyManager() {
    close_all();
    if (m_exit_hooks_registered && g_pty_manager_instance == this) {
        SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
        g_pty_manager_instance = nullptr;
    }
}

std::shared_ptr<PtySession> PtyManager::create(const PtyOptions& options, const PtyCallbacks& callbacks) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sessions.size() >= m_max_sessions) {
        throw std::runtime_error("MAX_SESSIONS");
    }
    if (m_sessions.find(options.id) != m_sessions.end()) {
        throw std::runtime_error("SESSION_EXISTS");
    }

    std::string opt_id = options.id;
    PtyCallbacks wrapped_callbacks = callbacks;
    wrapped_callbacks.on_exit = [this, opt_id, orig_on_exit = callbacks.on_exit](int code, int signal) {
        {
            std::lock_guard<std::mutex> inner_lock(m_mutex);
            m_sessions.erase(opt_id);
        }
        if (orig_on_exit) orig_on_exit(code, signal);
    };

    auto session = std::make_shared<WindowsPtySession>(opt_id, options, wrapped_callbacks, m_max_buffer_bytes);
    session->start();
    m_sessions[opt_id] = session;
    return session;
}

std::shared_ptr<PtySession> PtyManager::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) return it->second;
    return nullptr;
}

void PtyManager::close(const std::string& id) {
    std::shared_ptr<PtySession> session;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(id);
        if (it != m_sessions.end()) {
            session = it->second;
            m_sessions.erase(it);
        }
    }
    if (session) {
        session->close();
    }
}

void PtyManager::close_all() {
    std::vector<std::shared_ptr<PtySession>> sessions_copy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [_, s] : m_sessions) {
            sessions_copy.push_back(s);
        }
        m_sessions.clear();
    }
    for (auto& s : sessions_copy) {
        if (s) s->close();
    }
}

size_t PtyManager::active_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.size();
}

} // namespace machinebridge

#endif
