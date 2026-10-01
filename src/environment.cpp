#include "machinebridge/environment.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace machinebridge {

namespace {
static std::mutex g_env_mutex;
static std::string g_android_files_dir;
static std::string g_android_cache_dir;
}

void EnvironmentDetector::set_android_paths(const std::string& files_dir, const std::string& cache_dir) {
    std::lock_guard<std::mutex> lock(g_env_mutex);
    g_android_files_dir = files_dir;
    g_android_cache_dir = cache_dir;
}

bool EnvironmentDetector::verify_su_capability() {
#if defined(_WIN32)
    return false;
#else
    if (access("/system/xbin/su", X_OK) != 0 &&
        access("/system/bin/su", X_OK) != 0 &&
        access("/sbin/su", X_OK) != 0 &&
        access("/data/local/tmp/su", X_OK) != 0 &&
        access("/bin/su", X_OK) != 0 &&
        access("/usr/bin/su", X_OK) != 0) {
        // Also check PATH
        const char* path_env = getenv("PATH");
        if (!path_env) return false;
    }

    // Attempt non-blocking fork/exec of su -c "id -u"
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);

        char* const argv[] = {
            const_cast<char*>("su"),
            const_cast<char*>("-c"),
            const_cast<char*>("id -u"),
            nullptr
        };
        execvp("su", argv);
        _exit(127);
    }

    close(pipe_fd[1]);

    // Read with timeout
    char buf[64] = {0};
    ssize_t n = 0;
    int status = 0;
    
    pid_t w = waitpid(pid, &status, 0);
    if (w > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        n = read(pipe_fd[0], buf, sizeof(buf) - 1);
        close(pipe_fd[0]);
        if (n > 0) {
            buf[n] = '\0';
            std::string out(buf);
            out.erase(std::remove_if(out.begin(), out.end(), ::isspace), out.end());
            return (out == "0");
        }
    } else {
        close(pipe_fd[0]);
    }
    return false;
#endif
}

EnvironmentInfo EnvironmentDetector::detect(
    const std::optional<std::string>& custom_shell,
    const std::optional<std::string>& custom_workspace
) {
    EnvironmentInfo info;

    // Architecture
#if defined(__aarch64__) || defined(_M_ARM64)
    info.architecture = "arm64-v8a";
#elif defined(__x86_64__) || defined(_M_X64)
    info.architecture = "x86_64";
#elif defined(__arm__) || defined(_M_ARM)
    info.architecture = "armeabi-v7a";
#elif defined(__i386__) || defined(_M_IX86)
    info.architecture = "x86";
#else
    info.architecture = "unknown";
#endif

#if defined(_WIN32)
    info.type = EnvironmentType::Windows;
    info.type_name = "windows";
    info.platform = "windows";
    info.os_version = "Windows";
    info.kernel_version = "NT";
    info.process_uid = 0;
    info.effective_uid = 0;
    info.is_root = false;
    info.privileged_shell_available = false;

    const char* up = getenv("USERPROFILE");
    info.home_dir = up ? up : "C:\\";
    const char* tmp = getenv("TEMP");
    info.tmp_dir = tmp ? tmp : "C:\\Temp";
    const char* pe = getenv("PATH");
    info.path_env = pe ? pe : "";

    if (custom_shell && !custom_shell->empty()) {
        info.default_shell = *custom_shell;
    } else {
        info.default_shell = "powershell.exe";
    }

    if (custom_workspace && !custom_workspace->empty()) {
        info.workspace_dir = *custom_workspace;
    } else {
        info.workspace_dir = info.home_dir;
    }

    info.host_capabilities = {"pty", "signals", "process_groups", "sockets"};
#else
    struct utsname uts{};
    if (uname(&uts) == 0) {
        info.os_version = uts.sysname;
        info.kernel_version = uts.release;
    }

    info.process_uid = static_cast<uint32_t>(getuid());
    info.effective_uid = static_cast<uint32_t>(geteuid());
    info.is_root = (info.effective_uid == 0);

    std::string files_dir_copy, cache_dir_copy;
    {
        std::lock_guard<std::mutex> lock(g_env_mutex);
        files_dir_copy = g_android_files_dir;
        cache_dir_copy = g_android_cache_dir;
    }

    // Determine environment type
    bool is_termux = false;
    if (getenv("TERMUX_VERSION") != nullptr) {
        is_termux = true;
    } else if (getenv("PREFIX") != nullptr && std::string(getenv("PREFIX")).find("com.termux") != std::string::npos) {
        is_termux = true;
    } else if (access("/data/data/com.termux/files/usr/bin/bash", X_OK) == 0) {
        is_termux = true;
    }

    bool is_android_app = false;
#if defined(__ANDROID__)
    if (!is_termux) {
        is_android_app = true;
    }
#else
    if (!files_dir_copy.empty() || (access("/system/bin/sh", X_OK) == 0 && access("/bin/sh", F_OK) != 0 && !is_termux)) {
        is_android_app = true;
    }
#endif

    if (is_termux) {
        info.type = EnvironmentType::Termux;
        info.type_name = "termux";
        info.platform = "android";

        const char* prefix = getenv("PREFIX");
        std::string pfx = prefix ? prefix : "/data/data/com.termux/files/usr";
        const char* h = getenv("HOME");
        info.home_dir = h ? h : (pfx + "/../home");
        const char* t = getenv("TMPDIR");
        info.tmp_dir = t ? t : (pfx + "/tmp");
        const char* p = getenv("PATH");
        info.path_env = p ? p : (pfx + "/bin:/system/bin");

        if (custom_shell && !custom_shell->empty()) {
            info.default_shell = *custom_shell;
        } else {
            const char* sh = getenv("SHELL");
            if (sh && sh[0] != '\0') info.default_shell = sh;
            else if (access((pfx + "/bin/bash").c_str(), X_OK) == 0) info.default_shell = pfx + "/bin/bash";
            else if (access((pfx + "/bin/sh").c_str(), X_OK) == 0) info.default_shell = pfx + "/bin/sh";
            else info.default_shell = "/system/bin/sh";
        }
    } else if (is_android_app) {
        info.type = EnvironmentType::AndroidApp;
        info.type_name = "android-app";
        info.platform = "android";

        const char* h = getenv("HOME");
        info.home_dir = !files_dir_copy.empty() ? files_dir_copy : (h && strcmp(h, "/") != 0 && strcmp(h, "/root") != 0 ? h : "/data/local/tmp");
        const char* t = getenv("TMPDIR");
        info.tmp_dir = !cache_dir_copy.empty() ? cache_dir_copy : (t && strcmp(t, "/") != 0 ? t : "/data/local/tmp");
        const char* p = getenv("PATH");
        info.path_env = p ? p : "/system/bin:/system/xbin:/vendor/bin";

        if (custom_shell && !custom_shell->empty()) {
            info.default_shell = *custom_shell;
        } else {
            info.default_shell = "/system/bin/sh";
        }
    } else {
        info.type = EnvironmentType::Linux;
        info.type_name = "linux";
        info.platform = "linux";

        const char* h = getenv("HOME");
        info.home_dir = h ? h : "/";
        const char* t = getenv("TMPDIR");
        info.tmp_dir = t ? t : "/tmp";
        const char* p = getenv("PATH");
        info.path_env = p ? p : "/usr/local/bin:/usr/bin:/bin";

        if (custom_shell && !custom_shell->empty()) {
            info.default_shell = *custom_shell;
        } else {
            const char* sh = getenv("SHELL");
            if (sh && sh[0] != '\0') info.default_shell = sh;
            else if (access("/bin/bash", X_OK) == 0) info.default_shell = "/bin/bash";
            else info.default_shell = "/bin/sh";
        }
    }

    if (custom_workspace && !custom_workspace->empty()) {
        info.workspace_dir = *custom_workspace;
    } else {
        info.workspace_dir = !info.home_dir.empty() ? info.home_dir : ".";
    }

    // Check optional root capability
    info.privileged_shell_available = (info.is_root || verify_su_capability());

    info.host_capabilities = {"pty", "signals", "process_groups", "sockets"};
    if (info.privileged_shell_available) {
        info.host_capabilities.push_back("privileged_shell");
    }
#endif

    return info;
}

SessionExecutionContext EnvironmentDetector::create_session_context(
    const std::string& session_id,
    const EnvironmentInfo& host_env,
    const std::optional<std::string>& requested_shell,
    const std::optional<std::string>& requested_cwd
) {
    SessionExecutionContext ctx;
    ctx.session_id = session_id;
    ctx.home_dir = host_env.home_dir;
    ctx.tmp_dir = host_env.tmp_dir;
    ctx.workspace_dir = requested_cwd.value_or(host_env.workspace_dir);
    ctx.path_env = host_env.path_env;

    std::string sh = requested_shell.value_or(host_env.default_shell);
    if (sh.empty()) sh = host_env.default_shell;

    bool is_su = (sh == "su" || sh.find("/su") != std::string::npos);
    if (is_su && host_env.privileged_shell_available) {
        ctx.shell = sh;
        ctx.uid = 0;
        ctx.is_root = true;
        ctx.capabilities = {"pty", "signals", "process_groups", "sockets", "root"};
    } else {
        ctx.shell = sh;
        ctx.uid = host_env.effective_uid;
        ctx.is_root = host_env.is_root;
        ctx.capabilities = {"pty", "signals", "process_groups", "sockets"};
    }

    return ctx;
}

} // namespace machinebridge
