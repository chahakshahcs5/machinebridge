#include "machinebridge/fs.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
    std::cout << "[test_fs] Running filesystem tests...\n";

    std::filesystem::path test_dir = std::filesystem::current_path() / "test_fs_sandbox";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);

    machinebridge::FilesystemManager mgr(test_dir.string());

    // 1. Security check: null byte
    bool null_caught = false;
    try {
        mgr.resolve_path(std::string("file\0.txt", 9));
    } catch (const std::runtime_error& ex) {
        null_caught = true;
    }
    assert(null_caught);
    std::cout << "  - Null byte guard passed\n";

    // 2. Security check: Windows reserved names
    bool con_caught = false;
    try {
        mgr.resolve_path("sub/CON/test.txt");
    } catch (const std::runtime_error&) {
        con_caught = true;
    }
    assert(con_caught);
    std::cout << "  - Reserved device name guard passed\n";

    // 3. Security check: Path traversal outside workspace
    bool traversal_caught = false;
    try {
        mgr.resolve_path("../../../outside.txt");
    } catch (const std::runtime_error&) {
        traversal_caught = true;
    }
    assert(traversal_caught);
    std::cout << "  - Path traversal guard passed\n";

    // 4. Write & Read
    auto write_res = mgr.write_file("hello.txt", "Hello MachineBridge C++");
    assert(write_res.bytes_written == 23);
    assert(!write_res.sha256.empty());

    auto read_res = mgr.read_file("hello.txt");
    assert(read_res.content == "Hello MachineBridge C++");
    assert(read_res.bytes_read == 23);
    assert(!read_res.has_more);
    std::cout << "  - Write & Read passed\n";

    // 5. Append
    machinebridge::WriteFileOptions append_opt;
    append_opt.append = true;
    mgr.write_file("hello.txt", " - appended", append_opt);
    auto read_appended = mgr.read_file("hello.txt");
    assert(read_appended.content == "Hello MachineBridge C++ - appended");
    std::cout << "  - Append passed\n";

    // 6. Base64 encoding
    machinebridge::WriteFileOptions b64_write;
    b64_write.encoding = "base64";
    mgr.write_file("binary.dat", "AQIDBAU=", b64_write); // bytes 1, 2, 3, 4, 5

    machinebridge::ReadFileOptions b64_read;
    b64_read.encoding = "base64";
    auto read_b64 = mgr.read_file("binary.dat", b64_read);
    assert(read_b64.content == "AQIDBAU=");
    std::cout << "  - Base64 read/write passed\n";

    // 7. Stat
    auto stat = mgr.stat_file("hello.txt");
    assert(stat.is_file);
    assert(!stat.is_directory);
    assert(stat.size == 34);
    std::cout << "  - Stat passed\n";

    // 8. Copy & Move
    mgr.copy_file("hello.txt", "copied.txt");
    assert(std::filesystem::exists(test_dir / "copied.txt"));

    mgr.move_file("copied.txt", "moved.txt");
    assert(!std::filesystem::exists(test_dir / "copied.txt"));
    assert(std::filesystem::exists(test_dir / "moved.txt"));
    std::cout << "  - Copy & Move passed\n";

    // 9. List files
    auto list = mgr.list_files(".", {true});
    assert(list.size() >= 2);
    std::cout << "  - List files passed\n";

    // 10. Delete
    mgr.delete_file("moved.txt");
    assert(!std::filesystem::exists(test_dir / "moved.txt"));
    std::cout << "  - Delete passed\n";

    // Cleanup
    std::filesystem::remove_all(test_dir);
    std::cout << "[test_fs] All filesystem tests passed!\n";
    return 0;
}
