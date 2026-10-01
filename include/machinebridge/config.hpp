#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace machinebridge {

struct Config {
    std::string edge_host = "0.0.0.0";
    uint16_t edge_port = 8080;
    std::string api_key = "machinebridge-dev-key";
    size_t max_sessions = 8;
    size_t max_buffer_bytes = 4 * 1024 * 1024; // 4 MiB
    size_t max_message_bytes = 1024 * 1024;     // 1 MiB
    uint32_t session_ttl_seconds = 3600;
    uint32_t clock_skew_seconds = 60;
    uint32_t rate_limit_per_minute = 120;
    bool expose_tunnel = false;
    std::optional<std::string> tunnel_token;
    std::string tunnel_protocol = "quic";
    std::optional<std::string> workspace_root;
    std::optional<std::string> default_shell;
    bool verbose_logging = true;
    std::string log_level = "info";
};

// Loads configuration by reading .env file (if present) and process environment variables.
Config load_config(const std::optional<std::string>& env_file_path = std::nullopt);

// Helper to parse .env format content into key-value map.
std::unordered_map<std::string, std::string> parse_env_file(const std::string& content);

} // namespace machinebridge
