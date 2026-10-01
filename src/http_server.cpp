#include "machinebridge/environment.hpp"
#include "machinebridge/http_server.hpp"
#include "machinebridge/crypto.hpp"
#include "machinebridge/logger.hpp"
#include "machinebridge/shared.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

namespace machinebridge {

namespace {

std::string to_lower(std::string_view s) {
    std::string res(s);
    std::transform(res.begin(), res.end(), res.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return res;
}

std::string url_decode(std::string_view str) {
    std::string out;
    out.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%') {
            if (i + 2 < str.size()) {
                int hex_val = 0;
                std::istringstream iss(std::string(str.substr(i + 1, 2)));
                if (iss >> std::hex >> hex_val) {
                    out.push_back(static_cast<char>(hex_val));
                    i += 2;
                    continue;
                }
            }
        } else if (str[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(str[i]);
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_query_string(std::string_view qs) {
    std::unordered_map<std::string, std::string> query;
    if (qs.empty()) return query;
    if (qs.front() == '?') qs.remove_prefix(1);

    std::string s(qs);
    std::istringstream stream(s);
    std::string pair;
    while (std::getline(stream, pair, '&')) {
        if (pair.empty()) continue;
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = url_decode(pair.substr(0, eq));
            std::string v = url_decode(pair.substr(eq + 1));
            query[k] = v;
        } else {
            query[url_decode(pair)] = "";
        }
    }
    return query;
}

bool send_all(uintptr_t sock, const void* data, size_t len) {
    const char* ptr = reinterpret_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        int sent = send(static_cast<SOCKET>(sock), ptr, static_cast<int>(remaining), 0);
        if (sent <= 0) return false;
        remaining -= sent;
        ptr += sent;
    }
    return true;
}

} // namespace

// ============================================================================
// HttpRequest & HttpResponse
// ============================================================================

std::string HttpRequest::header(const std::string& name, const std::string& fallback) const {
    std::string lower_name = to_lower(name);
    for (const auto& [k, v] : headers) {
        if (to_lower(k) == lower_name) return v;
    }
    return fallback;
}

std::string HttpRequest::query_param(const std::string& name, const std::string& fallback) const {
    auto it = query.find(name);
    if (it != query.end()) return it->second;
    return fallback;
}

nlohmann::json HttpRequest::json_body() const {
    if (body.empty()) return nlohmann::json::object();
    try {
        return nlohmann::json::parse(body);
    } catch (...) {
        return nlohmann::json::object();
    }
}

void HttpResponse::set_header(const std::string& name, const std::string& value) {
    headers[name] = value;
}

HttpResponse HttpResponse::json(const nlohmann::json& j, int code) {
    HttpResponse res;
    res.status_code = code;
    res.status_message = (code == 200) ? "OK" : ((code == 201) ? "Created" : "Response");
    res.body = j.dump();
    res.set_header("Content-Type", "application/json; charset=utf-8");
    res.set_header("Access-Control-Allow-Origin", "*");
    return res;
}

HttpResponse HttpResponse::text(const std::string& t, int code, const std::string& content_type) {
    HttpResponse res;
    res.status_code = code;
    res.status_message = "OK";
    res.body = t;
    res.set_header("Content-Type", content_type);
    res.set_header("Access-Control-Allow-Origin", "*");
    return res;
}

HttpResponse HttpResponse::html(const std::string& h, int code) {
    return text(h, code, "text/html; charset=utf-8");
}

HttpResponse HttpResponse::redirect(const std::string& url, int code) {
    HttpResponse res;
    res.status_code = code;
    res.status_message = "Found";
    res.set_header("Location", url);
    res.set_header("Access-Control-Allow-Origin", "*");
    return res;
}

HttpResponse HttpResponse::error(int code, const std::string& err_code, const std::string& message) {
    nlohmann::json j = {{"error", err_code}, {"message", message}};
    HttpResponse res = json(j, code);
    res.status_message = "Error";
    return res;
}

// ============================================================================
// HttpServer
// ============================================================================

HttpServer::HttpServer(
    const Config& config,
    std::shared_ptr<PtyManager> pty_manager,
    std::shared_ptr<FilesystemManager> fs_manager,
    std::shared_ptr<InMemorySessionStore> sessions,
    std::shared_ptr<CommandExecutor> executor,
    std::shared_ptr<TunnelManager> tunnel_manager,
    std::shared_ptr<OAuthStore> oauth_store
)
    : m_config(config)
    , m_pty_manager(std::move(pty_manager))
    , m_fs_manager(std::move(fs_manager))
    , m_sessions(std::move(sessions))
    , m_executor(std::move(executor))
    , m_tunnel_manager(std::move(tunnel_manager))
    , m_oauth_store(std::move(oauth_store))
    , m_port(config.edge_port)
{
    m_mcp_context.executor = m_executor;
    m_mcp_context.fs_manager = m_fs_manager;
    m_mcp_context.sessions = m_sessions;
    m_mcp_context.pty_manager = m_pty_manager;
    m_mcp_context.api_key = m_config.api_key;
    m_mcp_context.base_url = "http://localhost:" + std::to_string(m_port);
    m_mcp_context.is_session_authenticated = [this](const std::string& sid) {
        std::lock_guard<std::mutex> lock(m_sse_mutex);
        return m_authenticated_sse_session_ids.find(sid) != m_authenticated_sse_session_ids.end();
    };

    register_all_routes();
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::add_route(const std::string& method, const std::string& path_pattern, HttpHandler handler) {
    bool is_pfx = (!path_pattern.empty() && path_pattern.back() == '*');
    std::string p = is_pfx ? path_pattern.substr(0, path_pattern.size() - 1) : path_pattern;
    m_routes.push_back(RouteEntry{method, p, is_pfx, std::move(handler)});
}

bool HttpServer::is_request_authenticated(const HttpRequest& req) const {
    std::string provided_key = req.header("x-api-key");
    if (provided_key.empty()) {
        std::string auth = req.header("authorization");
        if (auth.rfind("Bearer ", 0) == 0) {
            provided_key = auth.substr(7);
        }
    }
    if (provided_key.empty()) {
        provided_key = req.query_param("apiKey", req.query_param("token"));
    }
    return verify_api_key(m_config.api_key, provided_key);
}

std::string HttpServer::get_base_url(const HttpRequest& req) const {
    std::string tunnel_url;
    if (m_tunnel_manager && m_tunnel_manager->url().has_value() && !m_tunnel_manager->url()->empty()) {
        tunnel_url = *m_tunnel_manager->url();
    }

    std::string proto = req.header("x-forwarded-proto");
    if (!proto.empty()) {
        size_t comma = proto.find(',');
        if (comma != std::string::npos) proto = proto.substr(0, comma);
        size_t s = proto.find_first_not_of(" \t");
        size_t e = proto.find_last_not_of(" \t");
        if (s != std::string::npos && e != std::string::npos) proto = proto.substr(s, e - s + 1);
    }

    std::string host = req.header("x-forwarded-host");
    if (host.empty()) {
        host = req.header("host");
    }
    if (!host.empty()) {
        size_t comma = host.find(',');
        if (comma != std::string::npos) host = host.substr(0, comma);
        size_t s = host.find_first_not_of(" \t");
        size_t e = host.find_last_not_of(" \t");
        if (s != std::string::npos && e != std::string::npos) host = host.substr(s, e - s + 1);
    }

    // Check if the request arrived via Cloudflare Tunnel or proxy
    bool is_cloudflare = !req.header("cf-ray").empty() ||
                         !req.header("cf-connecting-ip").empty() ||
                         !req.header("cf-visitor").empty() ||
                         proto == "https" ||
                         host.find("trycloudflare.com") != std::string::npos;

    if (is_cloudflare) {
        if (host.find("trycloudflare.com") != std::string::npos) {
            return "https://" + host;
        }
        if (!tunnel_url.empty()) {
            return tunnel_url;
        }
        if (!host.empty()) {
            return "https://" + host;
        }
    }

    // External domain (not localhost or 127.0.0.1)
    if (!host.empty() && host.find("localhost") == std::string::npos && host.find("127.0.0.1") == std::string::npos) {
        return (proto.empty() ? "https" : proto) + "://" + host;
    }

    if (proto.empty()) {
        proto = "http";
    }

    if (!host.empty()) {
        return proto + "://" + host;
    }

    if (!tunnel_url.empty()) {
        return tunnel_url;
    }

    return "http://" + (m_config.edge_host == "0.0.0.0" ? "localhost" : m_config.edge_host) + ":" + std::to_string(m_port);
}

void HttpServer::register_all_routes() {
    // ------------------------------------------------------------------------
    // Health & Readiness
    // ------------------------------------------------------------------------
    auto health_handler = [this](const HttpRequest&) {
        nlohmann::json j = {
            {"ok", true},
            {"service", "machinebridge-unified"},
            {"tunnelUrl", m_tunnel_manager->url().value_or("")}
        };
        return HttpResponse::json(j);
    };
    add_route("GET", "/health", health_handler);
    add_route("GET", "/ready", [this](const HttpRequest&) {
        nlohmann::json j = {
            {"ok", true},
            {"status", "ready"},
            {"tunnelUrl", m_tunnel_manager->url().value_or("")}
        };
        return HttpResponse::json(j);
    });

    add_route("GET", "/api/environment", [](const HttpRequest&) {
        auto env_info = EnvironmentDetector::detect();
        nlohmann::json j;
        j["type"] = env_info.type_name;
        j["platform"] = env_info.platform;
        j["architecture"] = env_info.architecture;
        j["os_version"] = env_info.os_version;
        j["kernel_version"] = env_info.kernel_version;
        j["server_uid"] = env_info.process_uid;
        j["server_is_root"] = env_info.is_root;
        j["privileged_shell_available"] = env_info.privileged_shell_available;
        j["default_shell"] = env_info.default_shell;
        j["home_dir"] = env_info.home_dir;
        j["tmp_dir"] = env_info.tmp_dir;
        j["workspace_dir"] = env_info.workspace_dir;
        j["host_capabilities"] = env_info.host_capabilities;
        return HttpResponse::json(j);
    });

    add_route("GET", "/api/status", [this](const HttpRequest&) {
        auto env_info = EnvironmentDetector::detect();
        nlohmann::json j;
        j["running"] = m_running.load();
        j["port"] = m_port;
        j["host"] = m_config.edge_host;
        j["active_sessions"] = m_pty_manager ? m_pty_manager->active_count() : 0;
        nlohmann::json env_j;
        env_j["type"] = env_info.type_name;
        env_j["platform"] = env_info.platform;
        env_j["architecture"] = env_info.architecture;
        env_j["server_uid"] = env_info.process_uid;
        env_j["server_is_root"] = env_info.is_root;
        env_j["privileged_shell_available"] = env_info.privileged_shell_available;
        env_j["default_shell"] = env_info.default_shell;
        env_j["home_dir"] = env_info.home_dir;
        env_j["tmp_dir"] = env_info.tmp_dir;
        env_j["workspace_dir"] = env_info.workspace_dir;
        env_j["host_capabilities"] = env_info.host_capabilities;
        j["environment"] = env_j;
        return HttpResponse::json(j);
    });


    // ------------------------------------------------------------------------
    // Tools Endpoints (MCP REST)
    // ------------------------------------------------------------------------
    auto tools_list_handler = [](const HttpRequest&) {
        return HttpResponse::json({{"tools", get_mcp_tools()}});
    };
    add_route("GET", "/tools", tools_list_handler);
    add_route("GET", "/v1/tools", tools_list_handler);

    auto tool_exec_handler = [this](const HttpRequest& req) {
        if (!is_request_authenticated(req)) {
            return HttpResponse::error(401, "UNAUTHORIZED", "Invalid API key");
        }
        std::string path = req.path;
        std::string prefix = "/tools/";
        if (path.rfind("/v1/tools/", 0) == 0) prefix = "/v1/tools/";
        std::string tool_name = path.substr(prefix.size());

        nlohmann::json args = req.json_body();
        for (const auto& [k, v] : req.query) {
            if (!args.contains(k)) args[k] = v;
        }

        auto res = execute_tool(tool_name, args, m_mcp_context);
        nlohmann::json j = res;
        return res.is_error ? HttpResponse::json(j, 400) : HttpResponse::json(j, 200);
    };
    add_route("GET", "/tools/*", tool_exec_handler);
    add_route("POST", "/tools/*", tool_exec_handler);
    add_route("GET", "/v1/tools/*", tool_exec_handler);
    add_route("POST", "/v1/tools/*", tool_exec_handler);

    // ------------------------------------------------------------------------
    // Terminal Execution Endpoints
    // ------------------------------------------------------------------------
    auto exec_handler = [this](const HttpRequest& req) {
        if (!is_request_authenticated(req)) {
            return HttpResponse::error(401, "UNAUTHORIZED", "Invalid API key");
        }
        nlohmann::json body = req.json_body();
        std::string cmd = body.value("command", req.query_param("command"));
        if (cmd.empty()) {
            return HttpResponse::error(400, "INVALID_REQUEST", "Missing command parameter");
        }

        std::optional<std::string> cwd = body.contains("cwd") ? std::make_optional(body["cwd"].get<std::string>()) : (req.query.count("cwd") ? std::make_optional(req.query_param("cwd")) : std::nullopt);
        int64_t timeout_ms = body.value("timeoutMs", req.query.count("timeoutMs") ? std::stoll(req.query_param("timeoutMs")) : 15000LL);
        std::optional<std::string> shell = body.contains("shell") ? std::make_optional(body["shell"].get<std::string>()) : (req.query.count("shell") ? std::make_optional(req.query_param("shell")) : std::nullopt);
        std::optional<std::string> session_id = body.contains("sessionId") ? std::make_optional(body["sessionId"].get<std::string>()) : (req.query.count("sessionId") ? std::make_optional(req.query_param("sessionId")) : std::nullopt);

        try {
            auto res = m_executor->execute_command(cmd, cwd, timeout_ms, shell, session_id);
            nlohmann::json j = {
                {"ok", true},
                {"command", cmd},
                {"sessionId", res.session_id},
                {"output", res.output},
                {"exitCode", res.exit_code}
            };
            return HttpResponse::json(j);
        } catch (const std::exception& ex) {
            return HttpResponse::error(500, "EXECUTION_FAILED", ex.what());
        }
    };
    add_route("GET", "/v1/terminal/execute", exec_handler);
    add_route("POST", "/v1/terminal/execute", exec_handler);

    auto exec_batch_handler = [this](const HttpRequest& req) {
        if (!is_request_authenticated(req)) {
            return HttpResponse::error(401, "UNAUTHORIZED", "Invalid API key");
        }
        nlohmann::json body = req.json_body();
        std::vector<std::string> cmds;

        if (body.contains("commands") && body["commands"].is_array()) {
            cmds = body["commands"].get<std::vector<std::string>>();
        } else if (req.query.count("commands")) {
            try {
                auto parsed = nlohmann::json::parse(req.query_param("commands"));
                if (parsed.is_array()) cmds = parsed.get<std::vector<std::string>>();
            } catch (...) {}
        }

        if (cmds.empty()) {
            return HttpResponse::error(400, "INVALID_REQUEST", "Missing commands parameter");
        }

        std::optional<std::string> cwd = body.contains("cwd") ? std::make_optional(body["cwd"].get<std::string>()) : std::nullopt;
        int64_t timeout_ms = body.value("timeoutMs", 30000LL);
        bool stop_on_err = body.value("stopOnError", true);
        std::optional<std::string> shell = body.contains("shell") ? std::make_optional(body["shell"].get<std::string>()) : std::nullopt;
        std::optional<std::string> session_id = body.contains("sessionId") ? std::make_optional(body["sessionId"].get<std::string>()) : std::nullopt;

        try {
            auto res = m_executor->execute_commands(cmds, cwd, timeout_ms, stop_on_err, shell, session_id);
            nlohmann::json j = res;
            return HttpResponse::json(j);
        } catch (const std::exception& ex) {
            return HttpResponse::error(500, "EXECUTION_FAILED", ex.what());
        }
    };
    add_route("GET", "/v1/terminal/execute-batch", exec_batch_handler);
    add_route("POST", "/v1/terminal/execute-batch", exec_batch_handler);

    // ------------------------------------------------------------------------
    // Terminal Sessions Management
    // ------------------------------------------------------------------------
    add_route("POST", "/v1/terminal/sessions", [this](const HttpRequest& req) {
        if (!is_request_authenticated(req)) {
            return HttpResponse::error(401, "UNAUTHORIZED", "Invalid API key");
        }
        if (m_sessions->active_count() >= m_config.max_sessions) {
            return HttpResponse::error(429, "MAX_SESSIONS_REACHED", "Maximum active sessions reached");
        }
        auto session = m_sessions->create(m_config.session_ttl_seconds);
        int64_t exp = std::chrono::duration_cast<std::chrono::seconds>(session.expires_at.time_since_epoch()).count();
        nlohmann::json j = {
            {"sessionId", session.id},
            {"expiresAt", iso8601_time(exp)},
            {"websocketPath", "/v1/terminal/sessions/" + session.id}
        };
        return HttpResponse::json(j);
    });

    add_route("GET", "/v1/terminal/sessions/*", [this](const HttpRequest& req) {
        if (!is_request_authenticated(req)) {
            return HttpResponse::error(401, "UNAUTHORIZED", "Invalid API key");
        }
        std::string p = req.path;
        static const std::string pfx = "/v1/terminal/sessions/";
        std::string sid = p.substr(pfx.size());
        size_t slash = sid.find('/');
        std::string sub = (slash != std::string::npos) ? sid.substr(slash + 1) : "";
        if (slash != std::string::npos) sid = sid.substr(0, slash);

        auto s = m_sessions->get(sid);
        if (!s) return HttpResponse::error(404, "SESSION_NOT_FOUND", "Session not found");

        if (sub == "output") {
            return HttpResponse::json({{"ok", true}, {"sessionId", sid}, {"output", m_sessions->get_output(sid)}});
        }

        int64_t created = std::chrono::duration_cast<std::chrono::seconds>(s->created_at.time_since_epoch()).count();
        int64_t exp = std::chrono::duration_cast<std::chrono::seconds>(s->expires_at.time_since_epoch()).count();
        return HttpResponse::json({
            {"sessionId", s->id},
            {"status", s->status},
            {"createdAt", iso8601_time(created)},
            {"expiresAt", iso8601_time(exp)}
        });
    });

    auto session_input_handler = [this](const HttpRequest& req) {
        if (!is_request_authenticated(req)) {
            return HttpResponse::error(401, "UNAUTHORIZED", "Invalid API key");
        }
        std::string p = req.path;
        static const std::string pfx = "/v1/terminal/sessions/";
        std::string sid = p.substr(pfx.size());
        size_t slash = sid.find('/');
        std::string sub = (slash != std::string::npos) ? sid.substr(slash + 1) : "";
        if (slash != std::string::npos) sid = sid.substr(0, slash);

        auto s = m_sessions->get(sid);
        if (!s) return HttpResponse::error(404, "SESSION_NOT_FOUND", "Session not found");

        if (sub == "input") {
            auto body = req.json_body();
            std::string data = body.value("data", req.query_param("data"));
            std::string sig_str = body.value("signal", req.query_param("signal"));

            auto pty_s = m_pty_manager->get(sid);
            if (!pty_s) return HttpResponse::error(404, "PTY_NOT_RUNNING", "PTY process is not running for session");

            if (!data.empty()) pty_s->write(data);
            if (!sig_str.empty()) {
                if (auto sig = string_to_terminal_signal(sig_str)) {
                    pty_s->signal(*sig);
                }
            }
            return HttpResponse::json({{"ok", true}, {"sessionId", sid}});
        }

        if (sub == "close") {
            m_pty_manager->close(sid);
            m_sessions->set_status(sid, "closed");
            return HttpResponse::json({{"ok", true}, {"sessionId", sid}, {"status", "closed"}});
        }

        return HttpResponse::error(404, "NOT_FOUND", "Endpoint not found");
    };
    add_route("POST", "/v1/terminal/sessions/*", session_input_handler);

    // ------------------------------------------------------------------------
    // Filesystem REST Endpoints
    // ------------------------------------------------------------------------
    auto make_fs_handler = [this](FsAction action) {
        return [this, action](const HttpRequest& req) {
            if (!is_request_authenticated(req)) {
                return HttpResponse::error(401, "UNAUTHORIZED", "Invalid API key");
            }
            nlohmann::json params = req.json_body();
            for (const auto& [k, v] : req.query) {
                if (!params.contains(k)) params[k] = v;
            }

            try {
                auto data = m_fs_manager->execute_action(action, params);
                return HttpResponse::json({{"ok", true}, {"data", data}});
            } catch (const std::exception& ex) {
                return HttpResponse::error(400, "FS_OPERATION_FAILED", ex.what());
            }
        };
    };

    add_route("GET", "/v1/fs/read", make_fs_handler(FsAction::Read));
    add_route("POST", "/v1/fs/read", make_fs_handler(FsAction::Read));
    add_route("POST", "/v1/fs/write", make_fs_handler(FsAction::Write));
    add_route("POST", "/v1/fs/delete", make_fs_handler(FsAction::Delete));
    add_route("DELETE", "/v1/fs", make_fs_handler(FsAction::Delete));
    add_route("GET", "/v1/fs/list", make_fs_handler(FsAction::List));
    add_route("POST", "/v1/fs/list", make_fs_handler(FsAction::List));
    add_route("POST", "/v1/fs/mkdir", make_fs_handler(FsAction::Mkdir));
    add_route("POST", "/v1/fs/move", make_fs_handler(FsAction::Move));
    add_route("POST", "/v1/fs/copy", make_fs_handler(FsAction::Copy));

    add_route("POST", "/v1/fs/batch", [this](const HttpRequest& req) {
        if (!is_request_authenticated(req)) {
            return HttpResponse::error(401, "UNAUTHORIZED", "Invalid API key");
        }
        auto body = req.json_body();
        if (!body.contains("operations") || !body["operations"].is_array() || body["operations"].empty()) {
            return HttpResponse::error(400, "INVALID_REQUEST", "Missing operations array");
        }
        std::vector<FsOperation> ops = body["operations"].get<std::vector<FsOperation>>();
        bool stop_on_err = body.value("stopOnError", true);

        try {
            auto res = m_fs_manager->execute_batch(ops, stop_on_err, [this](const std::string& cmd, const std::optional<std::string>& cwd, std::optional<int64_t> timeout_ms) {
                auto exec_res = m_executor->execute_command(cmd, cwd, timeout_ms.value_or(15000));
                return nlohmann::json{{"output", exec_res.output}, {"exitCode", exec_res.exit_code}};
            });
            nlohmann::json j = res;
            return HttpResponse::json(j);
        } catch (const std::exception& ex) {
            return HttpResponse::error(400, "FS_BATCH_FAILED", ex.what());
        }
    });

    // ------------------------------------------------------------------------
    // OAuth 2.0 PKCE Endpoints & Well-Known Metadata
    // ------------------------------------------------------------------------
    auto well_known_resource_handler = [this](const HttpRequest& req) {
        std::string base = get_base_url(req);
        return HttpResponse::json(get_protected_resource_metadata(base));
    };
    add_route("GET", "/.well-known/oauth-protected-resource", well_known_resource_handler);
    add_route("GET", "/.well-known/oauth-protected-resource/sse", well_known_resource_handler);
    add_route("GET", "/.well-known/oauth-protected-resource*", well_known_resource_handler);
    add_route("GET", "/sse/.well-known/oauth-protected-resource", well_known_resource_handler);

    auto well_known_auth_handler = [this](const HttpRequest& req) {
        std::string base = get_base_url(req);
        return HttpResponse::json(get_authorization_server_metadata(base));
    };
    add_route("GET", "/.well-known/oauth-authorization-server", well_known_auth_handler);
    add_route("GET", "/.well-known/openid-configuration", well_known_auth_handler);

    add_route("GET", "/oauth/userinfo", [](const HttpRequest&) {
        return HttpResponse::json({
            {"sub", "machinebridge-user"},
            {"name", "MachineBridge User"},
            {"email", "user@machinebridge.local"},
            {"email_verified", true}
        });
    });

    add_route("POST", "/oauth/register", [](const HttpRequest& req) {
        auto body = req.json_body();
        std::vector<std::string> redirect_uris = body.value("redirect_uris", std::vector<std::string>{"https://chatgpt.com/connector/oauth/callback"});
        std::string auth_method = body.value("token_endpoint_auth_method", "client_secret_basic");
        std::string client_id = "client_" + create_nonce().substr(0, 16);
        std::string client_secret = create_nonce();
        int64_t now = now_seconds();

        nlohmann::json j = {
            {"client_id", client_id},
            {"client_secret", client_secret},
            {"client_id_issued_at", now},
            {"client_secret_expires_at", 0},
            {"client_name", body.value("client_name", "ChatGPT Connector")},
            {"redirect_uris", redirect_uris},
            {"grant_types", nlohmann::json::array({"authorization_code"})},
            {"response_types", nlohmann::json::array({"code"})},
            {"token_endpoint_auth_method", auth_method},
            {"scope", "mcp"}
        };
        return HttpResponse::json(j, 201);
    });

    add_route("GET", "/oauth/authorize", [this](const HttpRequest& req) {
        std::string client_id = req.query_param("client_id");
        std::string redirect_uri = req.query_param("redirect_uri");
        std::string state = req.query_param("state");
        std::string code_challenge = req.query_param("code_challenge");
        std::string code_challenge_method = req.query_param("code_challenge_method", "S256");
        std::string query_key = req.query_param("apiKey");

        if (redirect_uri.empty()) {
            return HttpResponse::error(400, "invalid_request", "Missing redirect_uri");
        }

        std::string base = get_base_url(req);

        if (!query_key.empty() && verify_api_key(m_config.api_key, query_key)) {
            std::string code = m_oauth_store->create_code(client_id, redirect_uri, state, code_challenge, code_challenge_method);
            std::string target = redirect_uri + (redirect_uri.find('?') == std::string::npos ? "?" : "&");
            target += "code=" + code;
            if (!state.empty()) target += "&state=" + state;
            target += "&iss=" + base;
            return HttpResponse::redirect(target, 302);
        }

        AuthorizeHtmlParams p;
        p.client_id = client_id;
        p.redirect_uri = redirect_uri;
        p.state = state;
        p.code_challenge = code_challenge;
        p.code_challenge_method = code_challenge_method;
        return HttpResponse::html(render_authorize_html(p));
    });

    add_route("POST", "/oauth/authorize", [this](const HttpRequest& req) {
        // Parse application/x-www-form-urlencoded or JSON
        std::unordered_map<std::string, std::string> form;
        if (req.header("content-type").find("application/x-www-form-urlencoded") != std::string::npos) {
            form = parse_query_string(req.body);
        } else {
            auto j = req.json_body();
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_string()) form[it.key()] = it.value().get<std::string>();
            }
        }

        std::string redirect_uri = form["redirect_uri"];
        std::string submitted_key = form["apiKey"];
        std::string client_id = form["client_id"];
        std::string state = form["state"];
        std::string code_challenge = form["code_challenge"];
        std::string code_challenge_method = form["code_challenge_method"];

        if (redirect_uri.empty()) {
            return HttpResponse::error(400, "invalid_request", "Missing redirect_uri");
        }

        if (submitted_key.empty() || !verify_api_key(m_config.api_key, submitted_key)) {
            AuthorizeHtmlParams p;
            p.client_id = client_id;
            p.redirect_uri = redirect_uri;
            p.state = state;
            p.code_challenge = code_challenge;
            p.code_challenge_method = code_challenge_method;
            p.error = "Invalid API key. Please check your MACHINEBRIDGE_API_KEY.";
            return HttpResponse::html(render_authorize_html(p), 401);
        }

        std::string code = m_oauth_store->create_code(client_id, redirect_uri, state, code_challenge, code_challenge_method);
        std::string base = get_base_url(req);
        std::string target = redirect_uri + (redirect_uri.find('?') == std::string::npos ? "?" : "&");
        target += "code=" + code;
        if (!state.empty()) target += "&state=" + state;
        target += "&iss=" + base;
        return HttpResponse::redirect(target, 302);
    });

    add_route("POST", "/oauth/token", [this](const HttpRequest& req) {
        std::unordered_map<std::string, std::string> form;
        if (req.header("content-type").find("application/x-www-form-urlencoded") != std::string::npos) {
            form = parse_query_string(req.body);
        } else {
            auto j = req.json_body();
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_string()) form[it.key()] = it.value().get<std::string>();
            }
        }

        std::string grant_type = form["grant_type"];
        std::string code = form["code"];
        std::string redirect_uri = form["redirect_uri"];
        std::string code_verifier = form["code_verifier"];

        if (grant_type != "authorization_code") {
            return HttpResponse::error(400, "unsupported_grant_type", "Only authorization_code grant is supported");
        }

        if (code.empty()) {
            return HttpResponse::error(400, "invalid_request", "Missing authorization code");
        }

        auto code_data = m_oauth_store->consume_code(code);
        if (!code_data) {
            return HttpResponse::error(400, "invalid_grant", "Authorization code is invalid or expired");
        }

        if (!redirect_uri.empty() && !code_data->redirect_uri.empty() && redirect_uri != code_data->redirect_uri) {
            return HttpResponse::error(400, "invalid_grant", "Redirect URI mismatch");
        }

        if (code_data->code_challenge) {
            if (code_verifier.empty()) {
                return HttpResponse::error(400, "invalid_grant", "Missing code_verifier for PKCE");
            }
            if (!verify_pkce(code_verifier, *code_data->code_challenge, code_data->code_challenge_method)) {
                return HttpResponse::error(400, "invalid_grant", "PKCE verification failed");
            }
        }

        nlohmann::json j = {
            {"access_token", m_config.api_key},
            {"token_type", "Bearer"},
            {"expires_in", 31536000},
            {"scope", "mcp"}
        };
        return HttpResponse::json(j, 200);
    });

    // ------------------------------------------------------------------------
    // Direct MCP JSON-RPC POST on /sse, /messages, or /mcp
    // ------------------------------------------------------------------------
    auto jsonrpc_post_handler = [this](const HttpRequest& req) {
        nlohmann::json body = req.json_body();
        bool is_jsonrpc = (body.is_object() && (body.contains("jsonrpc") || body.contains("method"))) || body.is_array();

        if (is_jsonrpc) {
            bool authed = is_request_authenticated(req);
            McpContext ctx = m_mcp_context;
            ctx.base_url = get_base_url(req);
            if (body.is_array()) {
                nlohmann::json results = nlohmann::json::array();
                for (const auto& item : body) {
                    auto resp = compute_jsonrpc_response(item, ctx, authed);
                    if (resp) results.push_back(*resp);
                }
                return HttpResponse::json(results);
            } else {
                auto resp = compute_jsonrpc_response(body, ctx, authed);
                return HttpResponse::json(resp.value_or(nlohmann::json::object()));
            }
        }

        return HttpResponse::json({{"ok", true}, {"transport", "sse"}, {"endpoint", "/messages"}});
    };

    add_route("POST", "/sse", jsonrpc_post_handler);
    add_route("POST", "/messages", jsonrpc_post_handler);
    add_route("POST", "/mcp", jsonrpc_post_handler);
    add_route("GET", "/mcp", [](const HttpRequest&) {
        return HttpResponse::json({{"ok", true}, {"service", "machinebridge-mcp"}});
    });
}

// ============================================================================
// Server Socket & Dispatcher
// ============================================================================

bool HttpServer::start() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) return false;

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#if defined(SO_REUSEPORT)
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    inet_pton(AF_INET, m_config.edge_host.c_str(), &addr.sin_addr);

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(listen_sock);
        return false;
    }

    if (listen(listen_sock, 128) != 0) {
        closesocket(listen_sock);
        return false;
    }

    m_listen_sock = static_cast<uintptr_t>(listen_sock);
    m_running.store(true);

    std::thread([this]() { worker_loop(); }).detach();
    return true;
}

void HttpServer::stop() {
    if (m_running.exchange(false)) {
        if (m_listen_sock != 0) {
            closesocket(static_cast<SOCKET>(m_listen_sock));
            m_listen_sock = 0;
        }

        std::lock_guard<std::mutex> lock(m_sse_mutex);
        for (auto& [_, conn] : m_sse_clients) {
            if (conn->socket_fd != 0) {
                closesocket(static_cast<SOCKET>(conn->socket_fd));
            }
        }
        m_sse_clients.clear();
    }
}

void HttpServer::worker_loop() {
    while (m_running.load()) {
        sockaddr_in client_addr{};
#if defined(_WIN32)
        int client_len = sizeof(client_addr);
#else
        socklen_t client_len = sizeof(client_addr);
#endif
        SOCKET client_sock = accept(static_cast<SOCKET>(m_listen_sock), reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if (client_sock == INVALID_SOCKET) {
            if (!m_running.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
        std::string remote_addr(ip_buf);
        int client_port = ntohs(client_addr.sin_port);
        std::string remote_endpoint = remote_addr + ":" + std::to_string(client_port);

        std::thread([this, client_sock, remote_endpoint]() {
            handle_client(static_cast<uintptr_t>(client_sock), remote_endpoint);
        }).detach();
    }
}

void HttpServer::handle_client(uintptr_t sock, const std::string& remote_addr) {
    auto start_time = std::chrono::steady_clock::now();
    std::vector<char> raw_buf(16384);
    int received = recv(static_cast<SOCKET>(sock), raw_buf.data(), static_cast<int>(raw_buf.size() - 1), 0);
    if (received <= 0) {
        closesocket(static_cast<SOCKET>(sock));
        return;
    }
    raw_buf[received] = '\0';
    std::string req_text(raw_buf.data(), received);

    size_t header_end = req_text.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        closesocket(static_cast<SOCKET>(sock));
        return;
    }

    std::string headers_str = req_text.substr(0, header_end);
    std::string body = req_text.substr(header_end + 4);

    std::istringstream stream(headers_str);
    std::string req_line;
    if (!std::getline(stream, req_line)) {
        closesocket(static_cast<SOCKET>(sock));
        return;
    }
    if (!req_line.empty() && req_line.back() == '\r') req_line.pop_back();

    std::istringstream rl_stream(req_line);
    HttpRequest req;
    rl_stream >> req.method >> req.path;
    req.remote_addr = remote_addr;

    size_t qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        req.query_string = req.path.substr(qpos + 1);
        req.query = parse_query_string(req.query_string);
        req.path = req.path.substr(0, qpos);
    }

    std::string hline;
    while (std::getline(stream, hline)) {
        if (!hline.empty() && hline.back() == '\r') hline.pop_back();
        size_t colon = hline.find(':');
        if (colon != std::string::npos) {
            std::string k = hline.substr(0, colon);
            std::string v = hline.substr(colon + 1);
            size_t vs = v.find_first_not_of(" \t");
            if (vs != std::string::npos) v = v.substr(vs);
            req.headers[k] = v;
        }
    }

    // Read remaining body if Content-Length exceeds what was received
    std::string cl_str = req.header("content-length");
    if (!cl_str.empty()) {
        try {
            size_t cl = std::stoull(cl_str);
            while (body.size() < cl) {
                int r = recv(static_cast<SOCKET>(sock), raw_buf.data(), static_cast<int>(std::min<size_t>(raw_buf.size(), cl - body.size())), 0);
                if (r <= 0) break;
                body.append(raw_buf.data(), r);
            }
        } catch (...) {}
    }
    req.body = std::move(body);

    // Check for WebSocket Upgrade
    std::string upgrade = to_lower(req.header("upgrade"));
    if (upgrade == "websocket") {
        static const std::string ws_pfx = "/v1/terminal/sessions/";
        if (req.path.rfind(ws_pfx, 0) == 0) {
            std::string session_id = req.path.substr(ws_pfx.size());
            log_info("WS", remote_addr, " - UPGRADE ", req.path);
            handle_websocket_terminal(sock, req, session_id);
            return;
        }
    }

    // Check for SSE Stream
    if (req.path == "/sse" && req.method == "GET") {
        log_info("SSE", remote_addr, " - CONNECT /sse");
        handle_sse_stream(sock, req);
        return;
    }

    // Handle OPTIONS Preflight CORS
    if (req.method == "OPTIONS") {
        std::string cors_resp = "HTTP/1.1 204 No Content\r\n"
                                "Access-Control-Allow-Origin: *\r\n"
                                "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
                                "Access-Control-Allow-Headers: Content-Type, Authorization, X-API-Key, mcp-protocol-version\r\n"
                                "Access-Control-Max-Age: 86400\r\n\r\n";
        send_all(sock, cors_resp.data(), cors_resp.size());
        auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time).count();
        log_info("HTTP", remote_addr, " - OPTIONS ", req.path, " -> 204 No Content (", std::fixed, std::setprecision(2), elapsed, "ms)");
        closesocket(static_cast<SOCKET>(sock));
        return;
    }

    // Match route
    HttpResponse resp = HttpResponse::error(404, "NOT_FOUND", "Endpoint not found");
    for (const auto& r : m_routes) {
        if (r.method != req.method && r.method != "*") continue;
        if (r.is_prefix) {
            if (req.path.rfind(r.path, 0) == 0) {
                resp = r.handler(req);
                break;
            }
        } else {
            if (req.path == r.path) {
                resp = r.handler(req);
                break;
            }
        }
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " " << resp.status_message << "\r\n";
    if (resp.headers.find("Content-Length") == resp.headers.end() && !resp.body.empty()) {
        resp.set_header("Content-Length", std::to_string(resp.body.size()));
    }
    if (resp.headers.find("Connection") == resp.headers.end()) {
        resp.set_header("Connection", "close");
    }
    for (const auto& [k, v] : resp.headers) {
        oss << k << ": " << v << "\r\n";
    }
    oss << "\r\n" << resp.body;

    std::string full_response = oss.str();
    send_all(sock, full_response.data(), full_response.size());

    auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time).count();
    std::string details;
    if ((req.path == "/mcp" || req.path == "/messages" || req.path == "/sse") && req.method == "POST") {
        try {
            auto j = nlohmann::json::parse(req.body);
            if (j.is_object() && j.contains("method") && j["method"].is_string()) {
                std::string mcp_method = j["method"].get<std::string>();
                details = " [" + mcp_method;
                if (mcp_method == "tools/call" && j.contains("params") && j["params"].contains("name")) {
                    details += ": " + j["params"]["name"].get<std::string>();
                }
                details += "]";
            }
        } catch (...) {}
    } else if (req.path == "/v1/terminal/execute") {
        try {
            auto j = nlohmann::json::parse(req.body);
            if (j.contains("command") && j["command"].is_string()) {
                std::string cmd = j["command"].get<std::string>();
                if (cmd.size() > 40) cmd = cmd.substr(0, 37) + "...";
                details = " [cmd: \"" + cmd + "\"]";
            }
        } catch (...) {}
    }

    std::string query_info = req.query_string.empty() ? "" : ("?" + req.query_string);
    log_info("HTTP", remote_addr, " - ", req.method, " ", req.path, query_info, details,
             " -> ", resp.status_code, " ", resp.status_message,
             " (", std::fixed, std::setprecision(2), elapsed, "ms, ", resp.body.size(), " bytes)");

    closesocket(static_cast<SOCKET>(sock));
}

// ============================================================================
// WebSocket Terminal Streaming
// ============================================================================

void HttpServer::handle_websocket_terminal(uintptr_t sock, const HttpRequest& req, const std::string& session_id) {
    if (!is_request_authenticated(req)) {
        std::string unauth = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\nUnauthorized";
        send_all(sock, unauth.data(), unauth.size());
        closesocket(static_cast<SOCKET>(sock));
        return;
    }

    std::string key = req.header("sec-websocket-key");
    if (key.empty()) {
        std::string bad = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send_all(sock, bad.data(), bad.size());
        closesocket(static_cast<SOCKET>(sock));
        return;
    }

    std::string accept_key = compute_websocket_accept(key);
    std::string handshake = "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: " + accept_key + "\r\n\r\n";

    if (!send_all(sock, handshake.data(), handshake.size())) {
        closesocket(static_cast<SOCKET>(sock));
        return;
    }

    log_info("WS", req.remote_addr, " - CONNECTED ", req.path, " (sessionId: ", session_id, ")");

    auto pty_session = m_pty_manager->get(session_id);
    auto store_session = m_sessions->get(session_id);
    if (!store_session) {
        m_sessions->create();
    }

    std::shared_ptr<std::atomic<bool>> ws_alive = std::make_shared<std::atomic<bool>>(true);

    auto send_ws_json = [sock, ws_alive](const nlohmann::json& msg) {
        if (!ws_alive->load()) return;
        auto frame = encode_ws_frame(msg.dump(), WsOpcode::Text);
        send_all(sock, frame.data(), frame.size());
    };

    size_t data_listener_id = 0;
    size_t exit_listener_id = 0;

    if (pty_session) {
        data_listener_id = pty_session->add_data_listener([send_ws_json, session_id, this](const std::string& d) {
            m_sessions->append_output(session_id, d);
            send_ws_json({{"type", "terminal.output"}, {"sessionId", session_id}, {"data", d}});
        });
        exit_listener_id = pty_session->add_exit_listener([send_ws_json, session_id, this](int, int) {
            m_sessions->set_status(session_id, "closed");
            send_ws_json({{"type", "terminal.closed"}, {"sessionId", session_id}});
        });
    }

    // Frame reader loop
    std::vector<uint8_t> buffer;
    buffer.reserve(65536);
    uint8_t chunk[8192];

    while (ws_alive->load()) {
        int r = recv(static_cast<SOCKET>(sock), reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (r <= 0) break;
        buffer.insert(buffer.end(), chunk, chunk + r);

        while (true) {
            WsFrame frame;
            size_t consumed = 0;
            if (!parse_ws_frame(buffer.data(), buffer.size(), frame, consumed)) {
                break;
            }
            buffer.erase(buffer.begin(), buffer.begin() + consumed);

            if (frame.opcode == WsOpcode::Close) {
                ws_alive->store(false);
                break;
            }

            if (frame.opcode == WsOpcode::Ping) {
                auto pong = encode_ws_frame(frame.payload, WsOpcode::Pong);
                send_all(sock, pong.data(), pong.size());
                continue;
            }

            if (frame.opcode == WsOpcode::Text) {
                try {
                    nlohmann::json msg = nlohmann::json::parse(frame.payload);
                    std::string type = msg.value("type", "");

                    if (type == "terminal.create") {
                        if (!pty_session) {
                            PtyOptions opts;
                            opts.id = session_id;
                            opts.cols = msg.value("cols", 120);
                            opts.rows = msg.value("rows", 40);
                            if (msg.contains("shell") && !msg["shell"].is_null()) opts.shell = msg["shell"].get<std::string>();
                            if (msg.contains("cwd") && !msg["cwd"].is_null()) opts.cwd = msg["cwd"].get<std::string>();

                            PtyCallbacks cbs;
                            cbs.on_data = [send_ws_json, session_id, this](const std::string& d) {
                                m_sessions->append_output(session_id, d);
                                send_ws_json({{"type", "terminal.output"}, {"sessionId", session_id}, {"data", d}});
                            };
                            cbs.on_exit = [send_ws_json, session_id, this](int, int) {
                                m_sessions->set_status(session_id, "closed");
                                send_ws_json({{"type", "terminal.closed"}, {"sessionId", session_id}});
                            };
                            cbs.on_error = [send_ws_json](const std::string& err) {
                                send_ws_json({{"type", "error"}, {"code", "PTY_ERROR"}, {"message", err}});
                            };

                            pty_session = m_pty_manager->create(opts, cbs);
                            m_sessions->set_status(session_id, "running");

                            send_ws_json({
                                {"type", "terminal.created"},
                                {"requestId", msg.value("requestId", "")},
                                {"sessionId", session_id},
                                {"pid", pty_session->pid()}
                            });
                        }
                    } else if (type == "terminal.input") {
                        if (pty_session) pty_session->write(msg.value("data", ""));
                    } else if (type == "terminal.resize") {
                        if (pty_session) pty_session->resize(msg.value("cols", 120), msg.value("rows", 40));
                    } else if (type == "terminal.signal") {
                        if (pty_session) {
                            if (auto sig = string_to_terminal_signal(msg.value("signal", ""))) {
                                pty_session->signal(*sig);
                            }
                        }
                    } else if (type == "terminal.close") {
                        m_pty_manager->close(session_id);
                        m_sessions->set_status(session_id, "closed");
                        ws_alive->store(false);
                        break;
                    }
                } catch (...) {
                    send_ws_json({{"type", "error"}, {"code", "INVALID_MESSAGE"}, {"message", "Malformed message"}});
                }
            }
        }
    }

    ws_alive->store(false);
    if (pty_session) {
        if (data_listener_id) pty_session->remove_data_listener(data_listener_id);
        if (exit_listener_id) pty_session->remove_exit_listener(exit_listener_id);
    }
    m_pty_manager->close(session_id);
    m_sessions->set_status(session_id, "closed");
    log_info("WS", req.remote_addr, " - DISCONNECTED ", req.path, " (sessionId: ", session_id, ")");
    closesocket(static_cast<SOCKET>(sock));
}

// ============================================================================
// Server-Sent Events (SSE) Streaming
// ============================================================================

void HttpServer::handle_sse_stream(uintptr_t sock, const HttpRequest& req) {
    bool authed = is_request_authenticated(req);
    std::string session_id = create_uuid();

    std::string sse_headers = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/event-stream\r\n"
                              "Cache-Control: no-cache\r\n"
                              "Connection: keep-alive\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "X-Accel-Buffering: no\r\n\r\n";

    if (!send_all(sock, sse_headers.data(), sse_headers.size())) {
        closesocket(static_cast<SOCKET>(sock));
        return;
    }

    // Send initial ping and endpoint info
    std::string init_msg = ": ping\n\n";
    std::string endpoint = "/messages?sessionId=" + session_id;
    if (authed) endpoint += "&apiKey=" + m_config.api_key;
    init_msg += "event: endpoint\ndata: " + endpoint + "\n\n";
    send_all(sock, init_msg.data(), init_msg.size());

    auto client = std::make_shared<SseClientConnection>();
    client->socket_fd = sock;
    client->session_id = session_id;

    {
        std::lock_guard<std::mutex> lock(m_sse_mutex);
        m_sse_clients[session_id] = client;
        if (authed) {
            m_authenticated_sse_session_ids.insert(session_id);
        }
    }

    log_info("SSE", req.remote_addr, " - CONNECTED /sse (sessionId: ", session_id, ", authed: ", (authed ? "true" : "false"), ")");

    // Keep connection alive with periodic pings every 15s
    while (m_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        std::string ping = ": keep-alive\n\n";
        if (!send_all(sock, ping.data(), ping.size())) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_sse_mutex);
        m_sse_clients.erase(session_id);
        m_authenticated_sse_session_ids.erase(session_id);
    }
    log_info("SSE", req.remote_addr, " - DISCONNECTED /sse (sessionId: ", session_id, ")");
    closesocket(static_cast<SOCKET>(sock));
}

} // namespace machinebridge
