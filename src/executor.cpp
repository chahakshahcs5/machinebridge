#include "machinebridge/executor.hpp"
#include "machinebridge/crypto.hpp"
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

namespace machinebridge {

std::string clean_terminal_output(std::string_view raw) {
    if (raw.empty()) return "";

    std::string s(raw);

    // 1. Strip ANSI OSC escape sequences (Operating System Commands: window title, cwd, shell integration \x1b]633;...)
    static const std::regex ANSI_OSC(R"(\x1b\][^\x07\x1b]*(?:\x07|\x1b\\))");
    s = std::regex_replace(s, ANSI_OSC, "");

    // 2. Strip ANSI DCS, APC, PM, SOS sequences (Device Control String & privacy messages)
    static const std::regex ANSI_DCS(R"(\x1b[P_^\x58][^\x07\x1b]*(?:\x07|\x1b\\))");
    s = std::regex_replace(s, ANSI_DCS, "");

    // 3. Strip standard ECMA-48 / ANSI CSI escape sequences:
    // ESC [ followed by parameters (0x30-0x3F), intermediates (0x20-0x2F), and final byte (0x40-0x7E)
    // Matches: colors, cursor movements, erase, screen clear, bracketed paste (\x1b[200~, \x1b[201~), cursor shape (\x1b[0 q), etc.
    static const std::regex ANSI_CSI(R"(\x1b\[[0-9:;<=>?]*[ -/]*[@-~])");
    s = std::regex_replace(s, ANSI_CSI, "");

    // 4. Strip ANSI character sets (\x1b(B, \x1b)0, etc.)
#ifdef ANSI_CHARSET
#undef ANSI_CHARSET
#endif
    static const std::regex ANSI_CHARSET_REGEX(R"(\x1b[()*+-][A-Za-z0-9])");
    s = std::regex_replace(s, ANSI_CHARSET_REGEX, "");

    // 5. Strip 2-character escape codes (save/restore cursor \x1b7 / \x1b8, keypad modes \x1b= / \x1b>, reverse index \x1bM, reset \x1bc, etc.)
    static const std::regex ANSI_TWO_CHAR(R"(\x1b[78<=>cDEHMNO])");
    s = std::regex_replace(s, ANSI_TWO_CHAR, "");

    // 6. Strip any unclosed or truncated escape sequences at end of string
    static const std::regex ANSI_TRAILING(R"(\x1b(?:\[[0-9:;<=>?]*|\][^\x07\x1b]*|[P_^\x58][^\x07\x1b]*)?$)");
    s = std::regex_replace(s, ANSI_TRAILING, "");

    // 7. Normalize newlines and collapse multi-carriage returns (\r\r\n, \r\n, \r -> \n)
    std::string norm;
    norm.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            while (i + 1 < s.size() && s[i + 1] == '\r') {
                i++;
            }
            if (i + 1 < s.size() && s[i + 1] == '\n') {
                i++;
            }
            norm.push_back('\n');
        } else {
            norm.push_back(s[i]);
        }
    }

    // 8. Strip control characters except \n (10) and \t (9)
    std::string clean;
    clean.reserve(norm.size());
    for (char c : norm) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc == '\n' || uc == '\t' || (uc >= 32 && uc != 127)) {
            clean.push_back(c);
        }
    }

    // 9. Trim leading and trailing whitespace
    size_t start = clean.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = clean.find_last_not_of(" \t\r\n");
    return clean.substr(start, end - start + 1);
}

ShellType detect_shell_type(const std::optional<std::string>& shell_path) {
    if (!shell_path || shell_path->empty()) {
#if defined(_WIN32)
        const char* def = std::getenv("MACHINEBRIDGE_DEFAULT_SHELL");
        if (def && std::regex_search(def, std::regex("cmd(\\.exe)?$", std::regex_constants::icase))) {
            return ShellType::Cmd;
        }
        return ShellType::PowerShell;
#else
        return ShellType::Sh;
#endif
    }

    std::string sp = *shell_path;
    if (std::regex_search(sp, std::regex(R"((powershell|pwsh)(\.exe)?$)", std::regex_constants::icase))) {
        return ShellType::PowerShell;
    }
    if (std::regex_search(sp, std::regex(R"(cmd(\.exe)?$)", std::regex_constants::icase))) {
        return ShellType::Cmd;
    }
    return ShellType::Sh;
}

std::string create_command_with_markers(
    const std::string& command,
    const std::string& marker_id,
    ShellType shell_type
) {
    std::string trimmed = command;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s != std::string::npos) {
        size_t e = trimmed.find_last_not_of(" \t\r\n");
        trimmed = trimmed.substr(s, e - s + 1);
    }

    if (shell_type == ShellType::PowerShell) {
        return "[Console]::Out.WriteLine(\"[__MB_START_" + marker_id + "__]\"); "
               "try { & { " + trimmed + " } } "
               "catch { Write-Error $_.ToString(); if (-not $LASTEXITCODE) { $LASTEXITCODE = 1 } } "
               "finally { "
               "$__mb_ec = if ($?) { if ($LASTEXITCODE -ne $null) { $LASTEXITCODE } else { 0 } } else { if ($LASTEXITCODE -ne $null -and $LASTEXITCODE -ne 0) { $LASTEXITCODE } else { 1 } }; "
               "[Console]::Out.WriteLine(\"[__MB_COMPL_" + marker_id + "_\" + $__mb_ec + \"__]\") "
               "}\r\n";
    }

    if (shell_type == ShellType::Cmd) {
        return "@echo [__MB_START_" + marker_id + "__] & (" + trimmed + ") & @call echo [__MB_COMPL_" + marker_id + "_%ERRORLEVEL%__]\r\n";
    }

    // POSIX sh / bash
    return "printf \"[__MB_START_" + marker_id + "__]\\n\"; (" + trimmed + "); __mb_ec=$?; printf \"[__MB_COMPL_" + marker_id + "_%d__]\\n\" \"$__mb_ec\"\n";
}

ShellInvocation get_shell_and_args_for_command(
    const std::string& command,
    const std::optional<std::string>& custom_shell
) {
    if (custom_shell && !custom_shell->empty()) {
        std::string cs = *custom_shell;
        if (std::regex_search(cs, std::regex(R"((powershell|pwsh)(\.exe)?$)", std::regex_constants::icase))) {
            return {cs, {"-NoProfile", "-NonInteractive", "-Command", command}};
        }
        if (std::regex_search(cs, std::regex(R"(cmd(\.exe)?$)", std::regex_constants::icase))) {
            return {cs, {"/d", "/c", command}};
        }
        return {cs, {"-c", command}};
    }

#if defined(_WIN32)
    const char* def_shell = std::getenv("MACHINEBRIDGE_DEFAULT_SHELL");
    std::string shell = (def_shell && def_shell[0] != '\0') ? def_shell : "powershell.exe";
    if (std::regex_search(shell, std::regex(R"((powershell|pwsh)(\.exe)?$)", std::regex_constants::icase))) {
        return {shell, {"-NoProfile", "-NonInteractive", "-Command", command}};
    }
    const char* comspec = std::getenv("COMSPEC");
    std::string cmd = (comspec && comspec[0] != '\0') ? comspec : "cmd.exe";
    return {cmd, {"/d", "/c", command}};
#else
    const char* sh = std::getenv("SHELL");
    std::string user_shell;
    if (sh && sh[0] != '\0') {
        user_shell = sh;
    } else if (std::filesystem::exists("/bin/bash")) {
        user_shell = "/bin/bash";
    } else if (std::filesystem::exists("/bin/sh")) {
        user_shell = "/bin/sh";
    } else if (std::filesystem::exists("/system/bin/sh")) {
        user_shell = "/system/bin/sh";
    } else {
        user_shell = "sh";
    }
    return {user_shell, {"-c", command}};
#endif
}

CommandOutputParser::CommandOutputParser(std::string marker_id, ShellType shell_type)
    : m_marker_id(std::move(marker_id))
    , m_shell_type(shell_type)
{}

bool CommandOutputParser::append_chunk(std::string_view chunk) {
    if (m_completed) return true;
    m_raw_buffer.append(chunk.data(), chunk.size());

    // Scan recent window for completion marker
    size_t window_sz = std::max<size_t>(4096, chunk.size() + 256);
    std::string scan_window = (m_raw_buffer.size() > window_sz)
        ? m_raw_buffer.substr(m_raw_buffer.size() - window_sz)
        : m_raw_buffer;
    std::string stripped_window = clean_terminal_output(scan_window);

    std::regex compl_regex("\\[__MB_COMPL_" + m_marker_id + "_(-?\\d+)__\\]");
    std::smatch match;

    if (std::regex_search(stripped_window, match, compl_regex)) {
        m_completed = true;
        std::string full_stripped = clean_terminal_output(m_raw_buffer);
        std::smatch full_match;

        if (std::regex_search(full_stripped, full_match, compl_regex)) {
            int exit_code = 0;
            try {
                exit_code = std::stoi(full_match[1].str());
            } catch (...) {}

            std::string start_marker = "[__MB_START_" + m_marker_id + "__]";
            size_t compl_pos = static_cast<size_t>(full_match.position());
            std::string prefix = full_stripped.substr(0, compl_pos);

            // 1. First attempt: match the start marker when printed on its own line in the output
            // (distinguishes executed output from echoed command invocation text)
            std::regex start_line_regex(R"((?:\n|^)\[__MB_START_)" + m_marker_id + R"(__\](?:\n|$))");
            auto words_begin = std::sregex_iterator(prefix.begin(), prefix.end(), start_line_regex);
            auto words_end = std::sregex_iterator();
            bool found_line_match = false;
            std::sregex_iterator last_it;
            for (auto it = words_begin; it != words_end; ++it) {
                last_it = it;
                found_line_match = true;
            }

            std::string cmd_out;
            if (found_line_match) {
                size_t after_start = last_it->position() + last_it->length();
                if (after_start <= prefix.size()) {
                    cmd_out = prefix.substr(after_start);
                }
            } else {
                // 2. Fallback: search backwards from completion marker for the last occurrence of start_marker
                // This ensures we always pick the executed marker rather than an echoed command earlier in the stream
                size_t last_start = prefix.rfind(start_marker);
                if (last_start != std::string::npos) {
                    size_t after_start = last_start + start_marker.size();
                    if (after_start < prefix.size() && (prefix[after_start] == '\n' || prefix[after_start] == '\r')) {
                        after_start++;
                        if (after_start < prefix.size() && prefix[after_start] == '\n') {
                            after_start++;
                        }
                    }
                    if (after_start <= prefix.size()) {
                        cmd_out = prefix.substr(after_start);
                    }
                } else {
                    cmd_out = prefix;
                }
            }

            m_result = CommandResult{clean_terminal_output(cmd_out), exit_code};
        } else {
            m_result = CommandResult{full_stripped, 0};
        }
        return true;
    }

    return false;
}

CommandResult CommandOutputParser::complete_on_process_exit(int exit_code) {
    if (m_result) return *m_result;
    m_completed = true;
    std::string full_stripped = clean_terminal_output(m_raw_buffer);
    std::string start_marker = "[__MB_START_" + m_marker_id + "__]";

    std::regex start_line_regex(R"((?:\n|^)\[__MB_START_)" + m_marker_id + R"(__\](?:\n|$))");
    auto words_begin = std::sregex_iterator(full_stripped.begin(), full_stripped.end(), start_line_regex);
    auto words_end = std::sregex_iterator();
    bool found_line_match = false;
    std::sregex_iterator last_it;
    for (auto it = words_begin; it != words_end; ++it) {
        last_it = it;
        found_line_match = true;
    }

    std::string cmd_out;
    if (found_line_match) {
        size_t after_start = last_it->position() + last_it->length();
        if (after_start <= full_stripped.size()) {
            cmd_out = full_stripped.substr(after_start);
        }
    } else {
        size_t last_start = full_stripped.rfind(start_marker);
        if (last_start != std::string::npos) {
            size_t after_start = last_start + start_marker.size();
            if (after_start < full_stripped.size() && (full_stripped[after_start] == '\n' || full_stripped[after_start] == '\r')) {
                after_start++;
                if (after_start < full_stripped.size() && full_stripped[after_start] == '\n') {
                    after_start++;
                }
            }
            if (after_start <= full_stripped.size()) {
                cmd_out = full_stripped.substr(after_start);
            }
        } else {
            cmd_out = full_stripped;
        }
    }

    m_result = CommandResult{clean_terminal_output(cmd_out), exit_code};
    return *m_result;
}

CommandExecutor::CommandExecutor(std::shared_ptr<PtyManager> pty_manager)
    : m_pty_manager(std::move(pty_manager))
{}

CommandResult CommandExecutor::execute_in_session(
    std::shared_ptr<PtySession> session,
    const std::string& command,
    ShellType shell_type,
    int64_t timeout_ms,
    std::optional<int64_t> idle_timeout_ms
) {
    std::string marker_id = create_uuid();
    auto parser = std::make_shared<CommandOutputParser>(marker_id, shell_type);

    auto result_promise = std::make_shared<std::promise<CommandResult>>();
    auto result_future = result_promise->get_future();
    auto completed = std::make_shared<std::atomic<bool>>(false);

    auto last_data_time = std::make_shared<std::atomic<int64_t>>(now_millis());

    size_t data_id = session->add_data_listener([parser, result_promise, completed, last_data_time](const std::string& chunk) {
        last_data_time->store(now_millis());
        if (parser->append_chunk(chunk)) {
            bool exp = false;
            if (completed->compare_exchange_strong(exp, true)) {
                if (auto res = parser->result()) {
                    result_promise->set_value(*res);
                } else {
                    result_promise->set_value(CommandResult{clean_terminal_output(parser->raw_buffer()), 0});
                }
            }
        }
    });

    size_t exit_id = session->add_exit_listener([parser, result_promise, completed](int exit_code, int) {
        bool exp = false;
        if (completed->compare_exchange_strong(exp, true)) {
            result_promise->set_value(parser->complete_on_process_exit(exit_code));
        }
    });

    std::string invocation = create_command_with_markers(command, marker_id, shell_type);
    session->write(invocation);

    auto start_time = std::chrono::steady_clock::now();
    bool timed_out = false;

    while (!completed->load()) {
        auto now = std::chrono::steady_clock::now();
        int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        if (elapsed_ms >= timeout_ms) {
            timed_out = true;
            break;
        }

        if (idle_timeout_ms && *idle_timeout_ms > 0) {
            int64_t idle_ms = now_millis() - last_data_time->load();
            if (idle_ms >= *idle_timeout_ms) {
                timed_out = true;
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    session->remove_data_listener(data_id);
    session->remove_exit_listener(exit_id);

    if (timed_out) {
        bool exp = false;
        if (completed->compare_exchange_strong(exp, true)) {
            session->signal(TerminalSignal::SigInt);
            std::string out = parser->result() ? parser->result()->output : clean_terminal_output(parser->raw_buffer());
            out += "\n(Command execution timed out after " + std::to_string(timeout_ms) + "ms)";
            return CommandResult{out, 124};
        }
    }

    return result_future.get();
}

ExecutionResult CommandExecutor::execute_command(
    const std::string& command,
    const std::optional<std::string>& cwd,
    int64_t timeout_ms,
    const std::optional<std::string>& shell,
    const std::optional<std::string>& session_id,
    std::optional<int64_t> idle_timeout_ms
) {
    if (session_id && !session_id->empty()) {
        auto session = m_pty_manager->get(*session_id);
        if (!session) {
            throw std::runtime_error("Session " + *session_id + " not found or not active");
        }
        ShellType st = detect_shell_type(shell);
        auto res = execute_in_session(session, command, st, timeout_ms, idle_timeout_ms);
        return ExecutionResult{*session_id, res.output, res.exit_code};
    }

    std::string sid = create_uuid();
    auto invocation = get_shell_and_args_for_command(command, shell);

    std::string output;
    std::mutex out_mutex;
    std::atomic<bool> done{false};
    std::atomic<int> exit_code{0};
    std::condition_variable cv;

    PtyOptions opts{
        sid,
        120,
        40,
        invocation.shell,
        invocation.args,
        cwd
    };

    PtyCallbacks cbs;
    cbs.on_data = [&](const std::string& data) {
        std::lock_guard<std::mutex> lock(out_mutex);
        output += data;
    };
    cbs.on_exit = [&](int ec, int) {
        exit_code.store(ec);
        done.store(true);
        cv.notify_all();
    };

    auto session = m_pty_manager->create(opts, cbs);

    std::unique_lock<std::mutex> lock(out_mutex);
    bool finished = cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
        return done.load();
    });

    if (!finished) {
        session->close();
        m_pty_manager->close(sid);
        std::string clean = clean_terminal_output(output) + "\n(Command execution timed out after " + std::to_string(timeout_ms) + "ms)";
        return ExecutionResult{sid, clean, 124};
    }

    m_pty_manager->close(sid);
    return ExecutionResult{sid, clean_terminal_output(output), exit_code.load()};
}

BatchExecuteResult CommandExecutor::execute_commands(
    const std::vector<std::string>& commands,
    const std::optional<std::string>& cwd,
    int64_t timeout_ms,
    bool stop_on_error,
    const std::optional<std::string>& shell,
    const std::optional<std::string>& session_id
) {
    if (commands.empty()) {
        return BatchExecuteResult{true, session_id.value_or(""), FsBatchSummary{0, 0, 0, 0}, {}};
    }

    std::string sid = session_id.value_or(create_uuid());
    bool is_ephemeral = (!session_id.has_value() || session_id->empty());
    ShellType st = detect_shell_type(shell);
    std::shared_ptr<PtySession> session;

    if (!is_ephemeral) {
        session = m_pty_manager->get(sid);
        if (!session) throw std::runtime_error("Session " + sid + " not found or not active");
    } else {
        std::string resolved_shell = shell.value_or(
#if defined(_WIN32)
            std::getenv("MACHINEBRIDGE_DEFAULT_SHELL") ? std::getenv("MACHINEBRIDGE_DEFAULT_SHELL") : "powershell.exe"
#else
            std::getenv("SHELL") ? std::getenv("SHELL") : "/bin/bash"
#endif
        );
        PtyOptions opts{sid, 120, 40, resolved_shell, {}, cwd};
        session = m_pty_manager->create(opts);
        std::this_thread::sleep_for(std::chrono::milliseconds(250)); // Shell prompt warm-up
    }

    BatchExecuteResult result;
    result.session_id = sid;
    auto start_time = std::chrono::steady_clock::now();

    for (size_t i = 0; i < commands.size(); ++i) {
        const auto& cmd = commands[i];
        int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        int64_t rem_time = std::max<int64_t>(1000, timeout_ms - elapsed);

        if (elapsed >= timeout_ms) {
            result.results.push_back(BatchCommandResult{cmd, 124, "failed", "(Batch execution timed out)", 0.0});
            for (size_t j = i + 1; j < commands.size(); ++j) {
                result.results.push_back(BatchCommandResult{commands[j], -1, "skipped", "", 0.0});
            }
            break;
        }

        auto cmd_start = std::chrono::steady_clock::now();
        auto res = execute_in_session(session, cmd, st, rem_time);
        auto cmd_duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cmd_start).count();
        bool is_success = (res.exit_code == 0);

        result.results.push_back(BatchCommandResult{
            cmd,
            res.exit_code,
            is_success ? "success" : "failed",
            res.output,
            static_cast<double>(cmd_duration)
        });

        if (!is_success && stop_on_error) {
            for (size_t j = i + 1; j < commands.size(); ++j) {
                result.results.push_back(BatchCommandResult{commands[j], -1, "skipped", "", 0.0});
            }
            break;
        }
    }

    if (is_ephemeral && session) {
        session->close();
        m_pty_manager->close(sid);
    }

    int passed = 0, failed = 0, skipped = 0;
    for (const auto& r : result.results) {
        if (r.status == "success") passed++;
        else if (r.status == "failed") failed++;
        else if (r.status == "skipped") skipped++;
    }

    result.summary = FsBatchSummary{static_cast<int>(commands.size()), passed, failed, skipped};
    result.ok = (failed == 0);
    return result;
}

} // namespace machinebridge
