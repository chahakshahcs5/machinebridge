#include "machinebridge/executor.hpp"
#include "machinebridge/fs.hpp"
#include "machinebridge/pty.hpp"
#include "machinebridge/shared.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
    std::cout << "[test_batch] Running batch tests...\n";

    auto pty_mgr = std::make_shared<machinebridge::PtyManager>(4, 1024 * 1024);
    machinebridge::CommandExecutor executor(pty_mgr);

    // 1. Successful Batch
#if defined(_WIN32)
    std::vector<std::string> cmds = {
        "cmd.exe /c echo First",
        "cmd.exe /c echo Second"
    };
#else
    std::vector<std::string> cmds = {
        "echo First",
        "echo Second"
    };
#endif
    auto batch_res = executor.execute_commands(cmds);
    assert(batch_res.ok);
    assert(batch_res.summary.total == 2);
    assert(batch_res.summary.passed == 2);
    assert(batch_res.summary.failed == 0);
    assert(batch_res.results.size() == 2);

    std::string md = machinebridge::format_batch_markdown(batch_res);
    assert(md.find("Batch Execution: 2/2 Succeeded") != std::string::npos);
    std::cout << "  - Command batch execution test passed\n";

    // 2. Batch FS operations
    std::filesystem::path test_dir = std::filesystem::current_path() / "test_batch_fs";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);

    machinebridge::FilesystemManager fs_mgr(test_dir.string());

    std::vector<machinebridge::FsOperation> ops = {
        {"write", "file1.txt", "content one", "utf8", false, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        {"read", "file1.txt", std::nullopt, "utf8", std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        {"copy", std::nullopt, std::nullopt, std::nullopt, std::nullopt, "file2.txt", std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt}
    };
    ops[2].path = "file1.txt";

    auto fs_batch_res = fs_mgr.execute_batch(ops, true);
    assert(fs_batch_res.ok);
    assert(fs_batch_res.total == 3);
    assert(fs_batch_res.passed == 3);

    std::string fs_md = machinebridge::format_fs_batch_markdown(fs_batch_res);
    assert(fs_md.find("Batch Operations: 3/3 Succeeded") != std::string::npos);
    std::cout << "  - FS batch execution test passed\n";

    std::filesystem::remove_all(test_dir);
    std::cout << "[test_batch] All batch tests passed!\n";
    return 0;
}
