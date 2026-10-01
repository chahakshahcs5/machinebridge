#include "machinebridge/server.hpp"
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>

namespace machinebridge {

MachineBridgeServer::MachineBridgeServer() = default;

MachineBridgeServer::~MachineBridgeServer() {
    stop();
}

bool MachineBridgeServer::start(const Config& config) {
    if (m_running.load()) {
        return true;
    }

    m_config = config;
    m_port = config.edge_port;

    // Detect environment dynamically
    m_env_info = EnvironmentDetector::detect(m_config.default_shell, m_config.workspace_root);
    if (!m_config.default_shell.has_value() || m_config.default_shell->empty()) {
        m_config.default_shell = m_env_info.default_shell;
    }
    if (!m_config.workspace_root.has_value() || m_config.workspace_root->empty()) {
        m_config.workspace_root = m_env_info.workspace_dir;
    }

    // Configure logger
    auto& logger = Logger::instance();
    logger.set_enabled(m_config.verbose_logging);
    logger.set_level(Logger::string_to_level(m_config.log_level));

    m_pty_manager = std::make_shared<PtyManager>(m_config.max_sessions, m_config.max_buffer_bytes);
    m_fs_manager = std::make_shared<FilesystemManager>(m_config.workspace_root);
    m_sessions = std::make_shared<InMemorySessionStore>();
    m_executor = std::make_shared<CommandExecutor>(m_pty_manager);
    m_tunnel_manager = std::make_shared<TunnelManager>();
    m_oauth_store = std::make_shared<OAuthStore>();

    m_http_server = std::make_shared<HttpServer>(
        m_config,
        m_pty_manager,
        m_fs_manager,
        m_sessions,
        m_executor,
        m_tunnel_manager,
        m_oauth_store
    );

    if (!m_http_server->start()) {
        log_error("SERVER", "Failed to bind to " + m_config.edge_host + ":" + std::to_string(m_config.edge_port));
        return false;
    }

    m_running.store(true);
    log_info("SERVER", "Listening on http://" + m_config.edge_host + ":" + std::to_string(m_config.edge_port));

    if (m_config.expose_tunnel) {
        TunnelStartOptions t_opts;
        t_opts.port = m_config.edge_port;
        t_opts.token = m_config.tunnel_token;
        t_opts.protocol = m_config.tunnel_protocol;
        if (!m_env_info.home_dir.empty() && m_env_info.home_dir != "/" && m_env_info.home_dir != "/root") {
            t_opts.cache_dir = m_env_info.home_dir;
        } else if (!m_env_info.tmp_dir.empty() && m_env_info.tmp_dir != "/") {
            t_opts.cache_dir = m_env_info.tmp_dir;
        } else {
#if defined(__ANDROID__)
            t_opts.cache_dir = "/data/local/tmp";
#endif
        }

        log_info("TUNNEL", "Requesting Cloudflare tunnel (protocol: " + t_opts.protocol.value_or("http2") + ")...");
        m_tunnel_thread = std::thread([this, t_opts]() {
            try {
                std::string public_url = m_tunnel_manager->start(t_opts);
                log_info("TUNNEL", "Cloudflare Tunnel established: " + public_url);
            } catch (const std::exception& ex) {
                log_error("TUNNEL", "Error establishing tunnel: " + std::string(ex.what()));
            }
        });
    }

    // Background session cleanup loop (polls every 100ms to allow instant exit on shutdown)
    m_cleanup_thread = std::thread([this]() {
        int counter = 0;
        while (m_running.load()) {
            for (int i = 0; i < 10 && m_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!m_running.load()) break;
            if (++counter >= 60) {
                counter = 0;
                if (m_sessions) {
                    m_sessions->expire_old();
                }
            }
        }
    });

    return true;
}

void MachineBridgeServer::stop() {
    if (m_running.exchange(false)) {
        log_info("SERVER", "Stopping MachineBridge server...");
        if (m_tunnel_manager) m_tunnel_manager->stop();
        if (m_http_server) m_http_server->stop();
        if (m_pty_manager) m_pty_manager->close_all();
        if (m_tunnel_thread.joinable()) {
            m_tunnel_thread.detach();
        }
        if (m_cleanup_thread.joinable()) {
            m_cleanup_thread.join();
        }
        log_info("SERVER", "MachineBridge server stopped");
    }
}

std::string MachineBridgeServer::get_status_json() const {
    nlohmann::json j;
    j["running"] = m_running.load();
    j["port"] = m_port;
    j["host"] = m_config.edge_host;
    j["active_sessions"] = m_pty_manager ? m_pty_manager->active_count() : 0;
    
    nlohmann::json env_j;
    env_j["type"] = m_env_info.type_name;
    env_j["platform"] = m_env_info.platform;
    env_j["architecture"] = m_env_info.architecture;
    env_j["os_version"] = m_env_info.os_version;
    env_j["kernel_version"] = m_env_info.kernel_version;
    env_j["server_uid"] = m_env_info.process_uid;
    env_j["server_is_root"] = m_env_info.is_root;
    env_j["privileged_shell_available"] = m_env_info.privileged_shell_available;
    env_j["default_shell"] = m_env_info.default_shell;
    env_j["home_dir"] = m_env_info.home_dir;
    env_j["tmp_dir"] = m_env_info.tmp_dir;
    env_j["workspace_dir"] = m_env_info.workspace_dir;
    env_j["host_capabilities"] = m_env_info.host_capabilities;
    j["environment"] = env_j;

    return j.dump(2);
}

} // namespace machinebridge
