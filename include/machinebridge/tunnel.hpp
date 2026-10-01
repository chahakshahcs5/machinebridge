#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace cloudflared {
class Tunnel;
}

namespace machinebridge {

struct TunnelStartOptions {
    uint16_t port = 8080;
    std::string host = "localhost";
    std::optional<std::string> token;
    std::optional<std::string> cache_dir;
    std::optional<std::string> protocol = "http2";
    uint32_t timeout_ms = 45000;
};

class TunnelManager {
public:
    TunnelManager();
    ~TunnelManager();

    std::string start(const TunnelStartOptions& options);
    void stop();

    std::optional<std::string> url() const;
    std::optional<int64_t> pid() const;
    std::optional<std::string> last_error() const;
    bool is_running() const;

private:
    std::shared_ptr<cloudflared::Tunnel> m_active_tunnel;
    std::optional<std::string> m_current_url;
    std::optional<int64_t> m_tunnel_pid;
    std::optional<std::string> m_last_error;
};

} // namespace machinebridge
