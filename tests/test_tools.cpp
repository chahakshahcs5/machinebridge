#include "machinebridge/executor.hpp"
#include "machinebridge/fs.hpp"
#include "machinebridge/mcp.hpp"
#include "machinebridge/pty.hpp"
#include "machinebridge/session_store.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <unordered_set>

int main() {
    std::cout << "[test_tools] Running MCP tools tests...\n";

    // 1. Check all 15 tools in schemas
    auto tools = machinebridge::get_mcp_tools();
    assert(tools.size() == 15);

    std::unordered_set<std::string> tool_names;
    for (const auto& t : tools) {
        tool_names.insert(t["name"].get<std::string>());
    }

    std::vector<std::string> expected_names = {
        "execute_command", "execute_commands",
        "read_file", "write_file", "list_directory", "delete_file",
        "make_directory", "move_file", "copy_file", "stat_file", "batch_fs",
        "create_session", "send_input", "read_output", "close_session"
    };
    for (const auto& name : expected_names) {
        assert(tool_names.find(name) != tool_names.end());
    }
    std::cout << "  - All 15 MCP tool schemas verified\n";

    // Setup context
    std::filesystem::path test_dir = std::filesystem::current_path() / "test_mcp_tools";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);

    auto pty_mgr = std::make_shared<machinebridge::PtyManager>(4, 1024 * 1024);
    auto fs_mgr = std::make_shared<machinebridge::FilesystemManager>(test_dir.string());
    auto sessions = std::make_shared<machinebridge::InMemorySessionStore>();
    auto executor = std::make_shared<machinebridge::CommandExecutor>(pty_mgr);

    machinebridge::McpContext ctx;
    ctx.executor = executor;
    ctx.fs_manager = fs_mgr;
    ctx.sessions = sessions;
    ctx.pty_manager = pty_mgr;

    // 2. Test execute_command tool
#if defined(_WIN32)
    auto cmd_res = machinebridge::execute_tool("execute_command", {{"command", "cmd.exe /c echo ToolEcho"}}, ctx);
#else
    auto cmd_res = machinebridge::execute_tool("execute_command", {{"command", "echo ToolEcho"}}, ctx);
#endif
    assert(!cmd_res.is_error);
    assert(cmd_res.content[0]["text"].get<std::string>().find("ToolEcho") != std::string::npos);
    std::cout << "  - execute_command tool passed\n";

    // 3. Test write_file and read_file tools
    auto write_res = machinebridge::execute_tool("write_file", {{"path", "tool_test.txt"}, {"content", "tool file content"}}, ctx);
    assert(!write_res.is_error);

    auto read_res = machinebridge::execute_tool("read_file", {{"path", "tool_test.txt"}}, ctx);
    assert(!read_res.is_error);
    assert(read_res.content[0]["text"].get<std::string>().find("tool file content") != std::string::npos);
    std::cout << "  - write_file and read_file tools passed\n";

    // 4. Test stat_file tool
    auto stat_res = machinebridge::execute_tool("stat_file", {{"path", "tool_test.txt"}}, ctx);
    assert(!stat_res.is_error);
    std::cout << "  - stat_file tool passed\n";

    // 5. Test copy_file, list_directory, move_file, delete_file, make_directory
    auto copy_res = machinebridge::execute_tool("copy_file", {{"source", "tool_test.txt"}, {"destination", "tool_test_copy.txt"}}, ctx);
    assert(!copy_res.is_error);
    std::cout << "  - copy_file tool passed\n";

    auto mkdir_res = machinebridge::execute_tool("make_directory", {{"path", "subdir"}}, ctx);
    assert(!mkdir_res.is_error);
    std::cout << "  - make_directory tool passed\n";

    auto list_res = machinebridge::execute_tool("list_directory", {{"path", "."}}, ctx);
    assert(!list_res.is_error);
    std::cout << "  - list_directory tool passed\n";

    auto move_res = machinebridge::execute_tool("move_file", {{"source", "tool_test_copy.txt"}, {"destination", "subdir/tool_moved.txt"}}, ctx);
    assert(!move_res.is_error);
    std::cout << "  - move_file tool passed\n";

    auto del_res = machinebridge::execute_tool("delete_file", {{"path", "subdir/tool_moved.txt"}}, ctx);
    assert(!del_res.is_error);
    std::cout << "  - delete_file tool passed\n";

    // 6. Test batch_fs tool
    nlohmann::json batch_ops = nlohmann::json::array({
        {{"action", "write"}, {"path", "batch_item.txt"}, {"content", "batch_content_123"}},
        {{"action", "read"}, {"path", "batch_item.txt"}},
        {{"action", "stat"}, {"path", "batch_item.txt"}},
        {{"action", "delete"}, {"path", "batch_item.txt"}}
    });
    auto batch_fs_res = machinebridge::execute_tool("batch_fs", {{"operations", batch_ops}}, ctx);
    assert(!batch_fs_res.is_error);
    std::cout << "  - batch_fs tool passed\n";

    // 7. Test execute_commands tool
    nlohmann::json batch_cmds = nlohmann::json::array({
#if defined(_WIN32)
        {{"command", "cmd.exe /c echo BatchOne"}},
        {{"command", "cmd.exe /c echo BatchTwo"}}
#else
        {{"command", "echo BatchOne"}},
        {{"command", "echo BatchTwo"}}
#endif
    });
    auto batch_cmd_res = machinebridge::execute_tool("execute_commands", {{"commands", batch_cmds}}, ctx);
    assert(!batch_cmd_res.is_error);
    std::cout << "  - execute_commands tool passed\n";

    // 8. Test session tools (create_session, send_input, read_output, close_session)
    auto create_s = machinebridge::execute_tool("create_session", {{"ttlSeconds", 60}}, ctx);
    assert(!create_s.is_error);
    auto created_json = nlohmann::json::parse(create_s.content[0]["text"].get<std::string>());
    std::string sid = created_json["sessionId"];
    assert(!sid.empty());

    auto send_res = machinebridge::execute_tool("send_input", {{"sessionId", sid}, {"data", "echo hello\r\n"}}, ctx);
    assert(!send_res.is_error);

    auto read_out_res = machinebridge::execute_tool("read_output", {{"sessionId", sid}}, ctx);
    assert(!read_out_res.is_error);

    auto close_res = machinebridge::execute_tool("close_session", {{"sessionId", sid}}, ctx);
    assert(!close_res.is_error);
    std::cout << "  - Interactive session tools passed\n";

    std::filesystem::remove_all(test_dir);
    std::cout << "[test_tools] All 15 MCP tools successfully tested and passed!\n";
    return 0;
}
