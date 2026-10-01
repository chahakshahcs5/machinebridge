#include "machinebridge/executor.hpp"
#include "machinebridge/pty.hpp"
#include <cassert>
#include <iostream>

int main() {
    std::cout << "[test_executor] Running executor tests...\n";

    // 1. clean_terminal_output
    std::string raw = "\x1b[32mHello\x1b[0m\r\r\nWorld\x07!\x1b[200~pasted\x1b[201~\x1b]633;A\x07\x1b[>0;276;0c\x1b[1 q";
    std::string cleaned = machinebridge::clean_terminal_output(raw);
    assert(cleaned == "Hello\nWorld!pasted");
    std::cout << "  - clean_terminal_output test passed\n";

    // 2. CommandOutputParser with echoed command wrapper simulation
    std::string marker_id = "test-marker-123";
    machinebridge::CommandOutputParser parser(marker_id, machinebridge::ShellType::PowerShell);

    // Terminal PTY echoes the input command line first, followed by the real output and completion marker
    std::string echoed_stream =
        "PS C:\\Users>[Console]::Out.WriteLine(\"[__MB_START_" + marker_id + "__]\"); "
        "try { & { echo hi } } catch { Write-Error $_ } finally { [Console]::Out.WriteLine(\"[__MB_COMPL_" + marker_id + "_0__]\") }\r\n"
        "[__MB_START_" + marker_id + "__]\r\n"
        "Execution output line 1\r\n"
        "Execution output line 2\r\n"
        "[__MB_COMPL_" + marker_id + "_0__]\r\n"
        "PS C:\\Users>";

    assert(parser.append_chunk(echoed_stream));
    assert(parser.is_completed());

    auto res = parser.result();
    assert(res.has_value());
    assert(res->exit_code == 0);
    assert(res->output.find("Execution output line 1") != std::string::npos);
    assert(res->output.find("Execution output line 2") != std::string::npos);
    // Crucial: verify that NO wrapper text was exposed in output
    assert(res->output.find("[Console]::Out.WriteLine") == std::string::npos);
    assert(res->output.find("catch") == std::string::npos);
    assert(res->output.find("finally") == std::string::npos);
    assert(res->output.find("Write-Error") == std::string::npos);
    std::cout << "  - CommandOutputParser echoed wrapper rejection test passed\n";

    // 3. CommandExecutor execution
    auto pty_mgr = std::make_shared<machinebridge::PtyManager>(4, 1024 * 1024);
    machinebridge::CommandExecutor executor(pty_mgr);

#if defined(_WIN32)
    auto exec_res = executor.execute_command("cmd.exe /c echo HelloFromExecutor");
    assert(exec_res.exit_code == 0);
    assert(exec_res.output.find("HelloFromExecutor") != std::string::npos);
#else
    auto exec_res = executor.execute_command("echo HelloFromExecutor");
    assert(exec_res.exit_code == 0);
    assert(exec_res.output.find("HelloFromExecutor") != std::string::npos);
#endif
    std::cout << "  - Single command execution test passed\n";

    std::cout << "[test_executor] All executor tests passed!\n";
    return 0;
}
