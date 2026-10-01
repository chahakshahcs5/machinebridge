#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace machinebridge {

enum class EnvironmentType {
    Windows,
    Linux,
    Termux,
    AndroidApp
};

struct EnvironmentInfo {
    EnvironmentType type = EnvironmentType::Linux;
    std::string type_name = "linux";
    std::string platform = "linux";
    std::string architecture = "unknown";
    std::string os_version = "unknown";
    std::string kernel_version = "unknown";

    // Server process identity (NEVER conflated with child sessions)
    uint32_t process_uid = 0;
    uint32_t effective_uid = 0;
    bool is_root = false;

    // Host-level capabilities
    bool privileged_shell_available = false; // Verified working su/root capability
    std::string default_shell;
    std::string home_dir;
    std::string tmp_dir;
    std::string workspace_dir;
    std::string path_env;
    std::vector<std::string> host_capabilities;
};

struct SessionExecutionContext {
    std::string session_id;
    std::string shell;
    uint32_t uid = 0;
    bool is_root = false;
    std::string home_dir;
    std::string tmp_dir;
    std::string workspace_dir;
    std::string path_env;
    std::vector<std::string> capabilities;
};

class EnvironmentDetector {
public:
    static EnvironmentInfo detect(
        const std::optional<std::string>& custom_shell = std::nullopt,
        const std::optional<std::string>& custom_workspace = std::nullopt
    );

    static void set_android_paths(
        const std::string& files_dir,
        const std::string& cache_dir
    );

    static SessionExecutionContext create_session_context(
        const std::string& session_id,
        const EnvironmentInfo& host_env,
        const std::optional<std::string>& requested_shell = std::nullopt,
        const std::optional<std::string>& requested_cwd = std::nullopt
    );

    static bool verify_su_capability();
};

} // namespace machinebridge
