#include "machinebridge/config.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <linux/limits.h>
#endif

namespace machinebridge {

namespace {

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

std::optional<std::string> get_env_var(const char* name) {
#if defined(_WIN32)
    char* buf = nullptr;
    size_t sz = 0;
    if (_dupenv_s(&buf, &sz, name) == 0 && buf != nullptr) {
        std::string res(buf);
        free(buf);
        return res;
    }
    return std::nullopt;
#else
    const char* val = std::getenv(name);
    if (val) return std::string(val);
    return std::nullopt;
#endif
}

bool parse_boolean(const std::string& val) {
    std::string lower = val;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return (lower == "true" || lower == "1" || lower == "yes");
}

} // namespace

std::unordered_map<std::string, std::string> parse_env_file(const std::string& content) {
    std::unordered_map<std::string, std::string> result;
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') continue;

        size_t eq_pos = trimmed.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = trim(trimmed.substr(0, eq_pos));
            std::string val = trim(trimmed.substr(eq_pos + 1));
            result[key] = unquote(val);
        }
    }

    return result;
}

Config load_config(const std::optional<std::string>& env_file_path) {
    Config cfg;
    std::unordered_map<std::string, std::string> file_vars;

    std::filesystem::path target_env = env_file_path.has_value() && !env_file_path->empty()
        ? std::filesystem::path(*env_file_path)
        : std::filesystem::current_path() / ".env";

    std::error_code ec;
    if (std::filesystem::exists(target_env, ec) && !std::filesystem::is_directory(target_env, ec)) {
        std::ifstream file(target_env);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            file_vars = parse_env_file(buffer.str());
            std::cout << "[MachineBridge Config] Loaded .env from current directory: " << target_env.string() << "\n" << std::flush;
        }
    } else {
        std::cout << "[MachineBridge Config] No .env found in: " << target_env.string() << "\n" << std::flush;
    }

    auto get_val = [&](const char* key, const std::string& fallback) -> std::string {
        if (auto env_val = get_env_var(key)) {
            if (!env_val->empty()) return *env_val;
        }
        auto it = file_vars.find(key);
        if (it != file_vars.end()) {
            return it->second;
        }
        return fallback;
    };

    std::string host_val = get_val("EDGE_HOST", "");
    if (host_val.empty()) host_val = get_val("HOST", cfg.edge_host);
    cfg.edge_host = host_val;

    std::string port_val = get_val("EDGE_PORT", "");
    if (port_val.empty()) port_val = get_val("PORT", std::to_string(cfg.edge_port));
    cfg.edge_port = static_cast<uint16_t>(std::stoi(port_val));

    std::string api_key_val = get_val("MACHINEBRIDGE_API_KEY", "");
    if (api_key_val.empty()) api_key_val = get_val("API_KEY", cfg.api_key);
    cfg.api_key = api_key_val;
    cfg.max_sessions = static_cast<size_t>(std::stoul(get_val("MACHINEBRIDGE_MAX_SESSIONS", std::to_string(cfg.max_sessions))));
    cfg.max_buffer_bytes = static_cast<size_t>(std::stoull(get_val("MACHINEBRIDGE_MAX_BUFFER_BYTES", std::to_string(cfg.max_buffer_bytes))));
    cfg.max_message_bytes = static_cast<size_t>(std::stoull(get_val("MACHINEBRIDGE_MAX_MESSAGE_BYTES", std::to_string(cfg.max_message_bytes))));
    cfg.session_ttl_seconds = static_cast<uint32_t>(std::stoul(get_val("MACHINEBRIDGE_SESSION_TTL_SECONDS", std::to_string(cfg.session_ttl_seconds))));
    cfg.clock_skew_seconds = static_cast<uint32_t>(std::stoul(get_val("MACHINEBRIDGE_CLOCK_SKEW_SECONDS", std::to_string(cfg.clock_skew_seconds))));
    cfg.rate_limit_per_minute = static_cast<uint32_t>(std::stoul(get_val("MACHINEBRIDGE_RATE_LIMIT_PER_MINUTE", std::to_string(cfg.rate_limit_per_minute))));

    std::string tunnel_flag = get_val("MACHINEBRIDGE_EXPOSE_TUNNEL", "");
    if (tunnel_flag.empty()) {
        tunnel_flag = get_val("TUNNEL_ENABLED", "");
    }
    if (tunnel_flag.empty()) {
        tunnel_flag = get_val("EXPOSE_TUNNEL", "false");
    }
    cfg.expose_tunnel = parse_boolean(tunnel_flag);

    std::string token_val = get_val("MACHINEBRIDGE_TUNNEL_TOKEN", "");
    if (token_val.empty()) {
        token_val = get_val("CLOUDFLARE_TUNNEL_TOKEN", "");
    }
    if (token_val.empty()) {
        token_val = get_val("TUNNEL_TOKEN", "");
    }
    if (!token_val.empty()) {
        cfg.tunnel_token = token_val;
    }

    std::string proto_val = get_val("MACHINEBRIDGE_TUNNEL_PROTOCOL", "");
    if (proto_val.empty()) proto_val = get_val("CLOUDFLARE_TUNNEL_PROTOCOL", "");
    if (proto_val.empty()) proto_val = get_val("TUNNEL_PROTOCOL", "http2");
    cfg.tunnel_protocol = proto_val;

    std::string ws_root = get_val("MACHINEBRIDGE_WORKSPACE_ROOT", "");
    if (!ws_root.empty()) {
        cfg.workspace_root = ws_root;
    }

    std::string def_shell = get_val("MACHINEBRIDGE_DEFAULT_SHELL", "");
    if (!def_shell.empty()) {
        cfg.default_shell = def_shell;
    }

    std::string log_flag = get_val("MACHINEBRIDGE_LOG", "");
    if (log_flag.empty()) log_flag = get_val("LOG_REQUESTS", "");
    if (log_flag.empty()) log_flag = get_val("VERBOSE", "false");
    cfg.verbose_logging = parse_boolean(log_flag);

    cfg.log_level = get_val("MACHINEBRIDGE_LOG_LEVEL", get_val("LOG_LEVEL", "info"));

    return cfg;
}

} // namespace machinebridge
