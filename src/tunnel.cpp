#include "machinebridge/tunnel.hpp"
#include "machinebridge/logger.hpp"
#include <iostream>
#include <stdexcept>

#ifdef MACHINEBRIDGE_ENABLE_TUNNEL
#include <cloudflared/tunnel.hpp>
#include <cloudflared/types.hpp>
#endif

namespace machinebridge {

TunnelManager::TunnelManager() = default;

TunnelManager::~TunnelManager() {
    stop();
}

std::string TunnelManager::start(const TunnelStartOptions& options) {
#ifdef MACHINEBRIDGE_ENABLE_TUNNEL
    if (m_active_tunnel && !m_active_tunnel->is_closed()) {
        if (m_current_url && !m_current_url->empty()) {
            return *m_current_url;
        }
        stop();
    }

    cloudflared::TunnelOptions opts;
    opts.port = options.port;
    if (options.token) {
        opts.token = *options.token;
    }
    if (options.cache_dir) {
        opts.cache_dir = std::filesystem::path(*options.cache_dir);
    }
    if (options.protocol) {
        opts.protocol = *options.protocol;
    }
    opts.timeout = std::chrono::milliseconds(options.timeout_ms);

    // Map server log level to tunnel library log level
    auto& logger = Logger::instance();
    auto server_level = logger.level();
    if (server_level <= LogLevel::Debug) {
        opts.log_level = cloudflared::LogLevel::Debug;
    } else if (server_level <= LogLevel::Info) {
        opts.log_level = cloudflared::LogLevel::Info;
    } else {
        opts.log_level = cloudflared::LogLevel::Info; // Always show tunnel lifecycle
    }

    // Route tunnel library logs through the server's Logger
    opts.log_callback = [](cloudflared::LogLevel level, const std::string& msg) {
        if (level == cloudflared::LogLevel::Debug) {
            log_debug("TUNNEL", msg);
        } else {
            log_info("TUNNEL", msg);
        }
    };

    try {
        m_last_error.reset();
        m_active_tunnel = cloudflared::create_tunnel(opts);
        m_current_url = m_active_tunnel->url();
        m_tunnel_pid = m_active_tunnel->pid();
        return *m_current_url;
    } catch (const std::exception& ex) {
        m_last_error = ex.what();
        m_active_tunnel.reset();
        m_current_url.reset();
        m_tunnel_pid.reset();
        throw;
    }
#else
    (void)options;
    throw std::runtime_error("Cloudflare tunnel is not supported or disabled on this platform");
#endif
}

void TunnelManager::stop() {
#ifdef MACHINEBRIDGE_ENABLE_TUNNEL
    if (m_active_tunnel) {
        try {
            m_active_tunnel->close();
        } catch (...) {}
        m_active_tunnel.reset();
    }
#endif
    m_current_url.reset();
    m_tunnel_pid.reset();
    m_last_error.reset();
}

std::optional<std::string> TunnelManager::url() const {
#ifdef MACHINEBRIDGE_ENABLE_TUNNEL
    if (m_active_tunnel && !m_active_tunnel->is_closed()) {
        return m_current_url;
    }
#endif
    return std::nullopt;
}

std::optional<int64_t> TunnelManager::pid() const {
#ifdef MACHINEBRIDGE_ENABLE_TUNNEL
    if (m_active_tunnel && !m_active_tunnel->is_closed()) {
        return m_tunnel_pid;
    }
#endif
    return std::nullopt;
}

std::optional<std::string> TunnelManager::last_error() const {
#ifdef MACHINEBRIDGE_ENABLE_TUNNEL
    return m_last_error;
#else
    return std::nullopt;
#endif
}

bool TunnelManager::is_running() const {
#ifdef MACHINEBRIDGE_ENABLE_TUNNEL
    return m_active_tunnel && !m_active_tunnel->is_closed();
#else
    return false;
#endif
}

} // namespace machinebridge
