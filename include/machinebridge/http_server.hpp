#pragma once

#include "machinebridge/config.hpp"
#include "machinebridge/executor.hpp"
#include "machinebridge/fs.hpp"
#include "machinebridge/mcp.hpp"
#include "machinebridge/oauth.hpp"
#include "machinebridge/pty.hpp"
#include "machinebridge/session_store.hpp"
#include "machinebridge/tunnel.hpp"
#include "machinebridge/websocket.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

namespace machinebridge {

struct HttpRequest {
    std::string method; // GET, POST, DELETE, etc.
    std::string path;
    std::string query_string;
    std::unordered_map<std::string, std::string> query;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string remote_addr;

    std::string header(const std::string& name, const std::string& fallback = "") const;
    std::string query_param(const std::string& name, const std::string& fallback = "") const;
    nlohmann::json json_body() const;
};

struct HttpResponse {
    int status_code = 200;
    std::string status_message = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    void set_header(const std::string& name, const std::string& value);

    static HttpResponse json(const nlohmann::json& j, int code = 200);
    static HttpResponse text(const std::string& t, int code = 200, const std::string& content_type = "text/plain; charset=utf-8");
    static HttpResponse html(const std::string& h, int code = 200);
    static HttpResponse redirect(const std::string& url, int code = 302);
    static HttpResponse error(int code, const std::string& err_code, const std::string& message);
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

struct SseClientConnection {
    uintptr_t socket_fd = 0;
    std::string session_id;
};

class HttpServer {
public:
    HttpServer(
        const Config& config,
        std::shared_ptr<PtyManager> pty_manager,
        std::shared_ptr<FilesystemManager> fs_manager,
        std::shared_ptr<InMemorySessionStore> sessions,
        std::shared_ptr<CommandExecutor> executor,
        std::shared_ptr<TunnelManager> tunnel_manager,
        std::shared_ptr<OAuthStore> oauth_store
    );
    ~HttpServer();

    void add_route(const std::string& method, const std::string& path_pattern, HttpHandler handler);

    bool start();
    void stop();
    bool is_running() const { return m_running.load(); }
    uint16_t port() const { return m_port; }

private:
    void register_all_routes();
    void worker_loop();
    void handle_client(uintptr_t client_sock, const std::string& remote_addr);
    void handle_websocket_terminal(uintptr_t client_sock, const HttpRequest& req, const std::string& session_id);
    void handle_sse_stream(uintptr_t client_sock, const HttpRequest& req);

    bool is_request_authenticated(const HttpRequest& req) const;
    std::string get_base_url(const HttpRequest& req) const;

    Config m_config;
    std::shared_ptr<PtyManager> m_pty_manager;
    std::shared_ptr<FilesystemManager> m_fs_manager;
    std::shared_ptr<InMemorySessionStore> m_sessions;
    std::shared_ptr<CommandExecutor> m_executor;
    std::shared_ptr<TunnelManager> m_tunnel_manager;
    std::shared_ptr<OAuthStore> m_oauth_store;
    McpContext m_mcp_context;

    std::atomic<bool> m_running{false};
    uintptr_t m_listen_sock = 0;
    uint16_t m_port = 8080;

    struct RouteEntry {
        std::string method;
        std::string path;
        bool is_prefix = false;
        HttpHandler handler;
    };
    std::vector<RouteEntry> m_routes;

    mutable std::mutex m_sse_mutex;
    std::unordered_map<std::string, std::shared_ptr<SseClientConnection>> m_sse_clients;
    std::unordered_set<std::string> m_authenticated_sse_session_ids;
};

} // namespace machinebridge
