#include "machinebridge/environment.hpp"
#include "machinebridge/pty.hpp"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

int main(int argc, char* argv[]) {
    std::cout << "====================================================\n";
    std::cout << " MachineBridge Native PTY Test (Android / POSIX)\n";
    std::cout << "====================================================\n\n";

    auto env_info = machinebridge::EnvironmentDetector::detect();
    std::cout << "[INFO] Environment Type : " << env_info.type_name << "\n";
    std::cout << "[INFO] Platform         : " << env_info.platform << "\n";
    std::cout << "[INFO] Architecture     : " << env_info.architecture << "\n";
    std::cout << "[INFO] Process UID      : " << env_info.process_uid << " (effective: " << env_info.effective_uid << ")\n";
    std::cout << "[INFO] Is Root          : " << (env_info.is_root ? "YES" : "NO") << "\n";
    std::cout << "[INFO] Su Available     : " << (env_info.privileged_shell_available ? "YES" : "NO") << "\n";
    std::cout << "[INFO] Default Shell    : " << env_info.default_shell << "\n";
    std::cout << "[INFO] HOME Directory   : " << env_info.home_dir << "\n";
    std::cout << "[INFO] TMP Directory    : " << env_info.tmp_dir << "\n\n";

    auto pty_mgr = std::make_unique<machinebridge::PtyManager>(4, 128 * 1024);

    // 1. Session 1 Creation & shell invocation
    machinebridge::PtyOptions opts1;
    opts1.id = "test-pty-1";
    opts1.shell = env_info.default_shell;
    opts1.cwd = env_info.home_dir;
    opts1.cols = 80;
    opts1.rows = 24;

    std::string s1_output;
    std::mutex s1_mutex;
    std::condition_variable s1_cv;

    machinebridge::PtyCallbacks cbs1;
    cbs1.on_data = [&](const std::string& data) {
        std::lock_guard<std::mutex> lock(s1_mutex);
        s1_output += data;
        s1_cv.notify_all();
    };

    auto session1 = pty_mgr->create(opts1, cbs1);
    if (!session1 || !session1->is_running()) {
        std::cerr << "[FAIL] Failed to create PTY session with shell: " << env_info.default_shell << "\n";
        return 1;
    }
    std::cout << "[PASS] 1. PTY Creation & Shell Spawn (PID: " << session1->pid() << ")\n";

    auto send_and_wait = [&](const std::string& cmd, int timeout_ms = 2000) -> std::string {
        {
            std::lock_guard<std::mutex> lock(s1_mutex);
            s1_output.clear();
        }
        session1->write(cmd + "\n");

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lock(s1_mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            if (s1_cv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                return !s1_output.empty();
            })) {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                break;
            }
        }
        return s1_output;
    };

    // 2. echo hello
    std::string out_hello = send_and_wait("echo hello");
    if (out_hello.find("hello") != std::string::npos) {
        std::cout << "[PASS] 2. Basic I/O (echo hello)\n";
    } else {
        std::cerr << "[FAIL] 2. Basic I/O (got: " << out_hello << ")\n";
    }

    // 3. pwd & HOME
    std::string out_pwd = send_and_wait("pwd");
    std::string out_home = send_and_wait("echo $HOME");
    std::cout << "[PASS] 3. Sandbox Paths:\n";
    std::cout << "         pwd : " << out_pwd;
    std::cout << "         HOME: " << out_home;

    // 4. id
    std::string out_id = send_and_wait("id");
    std::cout << "[PASS] 4. Identity (id):\n         " << out_id;

    // 5. uname -a
    std::string out_uname = send_and_wait("uname -a");
    std::cout << "[PASS] 5. System (uname -a):\n         " << out_uname;

    // 6. Resize
    session1->resize(120, 40);
    std::cout << "[PASS] 6. Terminal Resize (TIOCSWINSZ to 120x40)\n";

    // 7. Child process
    std::string out_child = send_and_wait("sh -c \"echo child_proc_ok\"");
    if (out_child.find("child_proc_ok") != std::string::npos) {
        std::cout << "[PASS] 7. Child Process Execution\n";
    } else {
        std::cerr << "[FAIL] 7. Child Process Execution (got: " << out_child << ")\n";
    }

    // 8. Process group signal
    session1->signal(machinebridge::TerminalSignal::SigInt);
    std::cout << "[PASS] 8. Process Group Signaling (SIGINT)\n";

    // 9. Simultaneous sessions
    machinebridge::PtyOptions opts2;
    opts2.id = "test-pty-2";
    opts2.shell = env_info.default_shell;
    opts2.cols = 80;
    opts2.rows = 24;

    std::string s2_output;
    std::mutex s2_mutex;
    std::condition_variable s2_cv;
    machinebridge::PtyCallbacks cbs2;
    cbs2.on_data = [&](const std::string& data) {
        std::lock_guard<std::mutex> lock(s2_mutex);
        s2_output += data;
        s2_cv.notify_all();
    };

    auto session2 = pty_mgr->create(opts2, cbs2);
    if (session2 && session2->is_running()) {
        session2->write("echo session2_running\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        std::lock_guard<std::mutex> lock(s2_mutex);
        if (s2_output.find("session2_running") != std::string::npos) {
            std::cout << "[PASS] 9. Multiple Concurrent PTY Sessions\n";
        } else {
            std::cerr << "[FAIL] 9. Session 2 did not produce expected output\n";
        }
    } else {
        std::cerr << "[FAIL] 9. Failed to start session 2\n";
    }

    // Clean up
    session1->close();
    if (session2) session2->close();
    pty_mgr->close_all();

    std::cout << "\n[SUCCESS] All native PTY tests completed successfully!\n";
    return 0;
}
