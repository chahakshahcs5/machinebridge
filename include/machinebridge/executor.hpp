#pragma once

#include "machinebridge/pty.hpp"
#include "machinebridge/shared.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace machinebridge {

std::string clean_terminal_output(std::string_view raw);

enum class ShellType {
    PowerShell,
    Cmd,
    Sh
};

ShellType detect_shell_type(const std::optional<std::string>& shell_path = std::nullopt);

std::string create_command_with_markers(
    const std::string& command,
    const std::string& marker_id,
    ShellType shell_type
);

struct ShellInvocation {
    std::string shell;
    std::vector<std::string> args;
};

ShellInvocation get_shell_and_args_for_command(
    const std::string& command,
    const std::optional<std::string>& custom_shell = std::nullopt
);

struct CommandResult {
    std::string output;
    int exit_code = 0;
};

class CommandOutputParser {
public:
    CommandOutputParser(std::string marker_id, ShellType shell_type = ShellType::PowerShell);

    bool append_chunk(std::string_view chunk);
    CommandResult complete_on_process_exit(int exit_code);

    bool is_completed() const { return m_completed; }
    std::optional<CommandResult> result() const { return m_result; }
    const std::string& raw_buffer() const { return m_raw_buffer; }

private:
    std::string m_marker_id;
    ShellType m_shell_type;
    std::string m_raw_buffer;
    bool m_completed = false;
    std::optional<CommandResult> m_result;
};

struct ExecutionResult {
    std::string session_id;
    std::string output;
    int exit_code = 0;
};

class CommandExecutor {
public:
    explicit CommandExecutor(std::shared_ptr<PtyManager> pty_manager);

    ExecutionResult execute_command(
        const std::string& command,
        const std::optional<std::string>& cwd = std::nullopt,
        int64_t timeout_ms = 15000,
        const std::optional<std::string>& shell = std::nullopt,
        const std::optional<std::string>& session_id = std::nullopt,
        std::optional<int64_t> idle_timeout_ms = std::nullopt
    );

    BatchExecuteResult execute_commands(
        const std::vector<std::string>& commands,
        const std::optional<std::string>& cwd = std::nullopt,
        int64_t timeout_ms = 30000,
        bool stop_on_error = true,
        const std::optional<std::string>& shell = std::nullopt,
        const std::optional<std::string>& session_id = std::nullopt
    );

private:
    CommandResult execute_in_session(
        std::shared_ptr<PtySession> session,
        const std::string& command,
        ShellType shell_type,
        int64_t timeout_ms,
        std::optional<int64_t> idle_timeout_ms = std::nullopt
    );

    std::shared_ptr<PtyManager> m_pty_manager;
};

} // namespace machinebridge
