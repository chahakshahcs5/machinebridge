#pragma once

#include "machinebridge/config.hpp"
#include "machinebridge/environment.hpp"
#include "machinebridge/executor.hpp"
#include "machinebridge/fs.hpp"
#include "machinebridge/http_server.hpp"
#include "machinebridge/mcp.hpp"
#include "machinebridge/logger.hpp"
#include "machinebridge/oauth.hpp"
#include "machinebridge/pty.hpp"
#include "machinebridge/session_store.hpp"
#include "machinebridge/tunnel.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace machinebridge {

class MachineBridgeServer {
public:
    MachineBridgeServer();
    ~MachineBridgeServer();

    bool start(const Config& config);
    void stop();

    bool is_running() const { return m_running.load(); }
    uint16_t port() const { return m_port; }
    const Config& config() const { return m_config; }
    const EnvironmentInfo& environment() const { return m_env_info; }
    std::string get_status_json() const;

    std::shared_ptr<HttpServer> http_server() const { return m_http_server; }
    std::shared_ptr<PtyManager> pty_manager() const { return m_pty_manager; }
    std::shared_ptr<FilesystemManager> fs_manager() const { return m_fs_manager; }
    std::shared_ptr<InMemorySessionStore> sessions() const { return m_sessions; }
    std::shared_ptr<CommandExecutor> executor() const { return m_executor; }
    std::shared_ptr<TunnelManager> tunnel_manager() const { return m_tunnel_manager; }
    std::shared_ptr<OAuthStore> oauth_store() const { return m_oauth_store; }

private:
    std::atomic<bool> m_running{false};
    uint16_t m_port = 8080;
    Config m_config;
    EnvironmentInfo m_env_info;

    std::shared_ptr<HttpServer> m_http_server;
    std::shared_ptr<PtyManager> m_pty_manager;
    std::shared_ptr<FilesystemManager> m_fs_manager;
    std::shared_ptr<InMemorySessionStore> m_sessions;
    std::shared_ptr<CommandExecutor> m_executor;
    std::shared_ptr<TunnelManager> m_tunnel_manager;
    std::shared_ptr<OAuthStore> m_oauth_store;

    std::thread m_cleanup_thread;
    std::thread m_tunnel_thread;
};

} // namespace machinebridge
