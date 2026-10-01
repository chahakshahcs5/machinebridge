#include "machinebridge/server.hpp"
#include "machinebridge/config.hpp"
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
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

static std::atomic<bool> g_server_running{true};
static std::shared_ptr<machinebridge::HttpServer> g_http_server;
static std::shared_ptr<machinebridge::TunnelManager> g_tunnel_manager;
static std::shared_ptr<machinebridge::PtyManager> g_pty_manager;

void handle_signal(int) {
    g_server_running.store(false);
    if (g_tunnel_manager) g_tunnel_manager->stop();
    if (g_http_server) g_http_server->stop();
    if (g_pty_manager) g_pty_manager->close_all();
}

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        handle_signal(0);
        return TRUE;
    }
    return FALSE;
}
#endif

int main(int argc, char* argv[]) {
    bool is_stdio = false;
    std::optional<bool> cli_tunnel;
    std::optional<std::string> cli_tunnel_token;
    std::optional<std::string> cli_tunnel_protocol;
    std::optional<uint16_t> cli_port;
    std::optional<std::string> cli_host;
    std::optional<std::string> cli_api_key;
    std::optional<std::string> cli_workspace;
    std::optional<std::string> cli_shell;
    std::optional<size_t> cli_max_sessions;
    std::optional<uint32_t> cli_session_ttl;
    std::optional<std::string> cli_env_file;
    std::optional<bool> cli_log;
    std::optional<std::string> cli_log_level;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "MachineBridge Server (C++20)\n\n"
                      << "Usage:\n"
                      << "  machinebridge-server [options]\n\n"
                      << "Options:\n"
                      << "  --port, -p <port>         Server listening port (default: 8080)\n"
                      << "  --host <host>             Host / bind address (default: 0.0.0.0)\n"
                      << "  --api-key, -k <key>       API authentication key (default: machinebridge-dev-key)\n"
                      << "  --tunnel                  Enable Cloudflare Tunnel (default: false)\n"
                      << "  --tunnel-token <token>    Named Cloudflare tunnel token (implies --tunnel)\n"
                      << "  --protocol <http2|quic>   Edge connection protocol (default: http2 - TCP firewall safe)\n"
                      << "  --workspace, -w <dir>     Restrict filesystem operations to this directory\n"
                      << "  --shell <path>            Default terminal shell executable\n"
                      << "  --max-sessions <num>      Maximum concurrent active terminal sessions (default: 8)\n"
                      << "  --session-ttl <seconds>   Session inactivity expiration in seconds (default: 3600)\n"
                      << "  --log, -v, --verbose      Enable request & response logging (default: false)\n"
                      << "  --no-log                  Disable request & response logging\n"
                      << "  --log-level <level>       Minimum log level: debug, info, warn, error (default: info)\n"
                      << "  --stdio                   Run as an MCP stdio server (reads JSON-RPC on stdin)\n"
                      << "  --env, -e <path>          Path to optional .env file\n"
                      << "  --help, -h                Show this help message\n\n"
                      << "Examples:\n"
                      << "  machinebridge-server\n"
                      << "  machinebridge-server --log\n"
                      << "  machinebridge-server --tunnel --log\n"
                      << "  machinebridge-server --tunnel --protocol http2\n"
                      << "  machinebridge-server --port 9000 --api-key my-secret --tunnel\n"
                      << "  machinebridge-server --workspace C:\\Users\\chaha\\Projects\n";
            return 0;
        }
        if (arg == "--stdio") {
            is_stdio = true;
        } else if (arg == "--tunnel") {
            cli_tunnel = true;
        } else if (arg == "--no-tunnel") {
            cli_tunnel = false;
        } else if (arg == "--tunnel-token" && i + 1 < argc) {
            cli_tunnel_token = argv[++i];
            cli_tunnel = true;
        } else if ((arg == "--protocol" || arg == "--tunnel-protocol") && i + 1 < argc) {
            cli_tunnel_protocol = argv[++i];
        } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            cli_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--host" && i + 1 < argc) {
            cli_host = argv[++i];
        } else if ((arg == "--api-key" || arg == "-k") && i + 1 < argc) {
            cli_api_key = argv[++i];
        } else if ((arg == "--workspace" || arg == "-w") && i + 1 < argc) {
            cli_workspace = argv[++i];
        } else if (arg == "--shell" && i + 1 < argc) {
            cli_shell = argv[++i];
        } else if (arg == "--max-sessions" && i + 1 < argc) {
            cli_max_sessions = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--session-ttl" && i + 1 < argc) {
            cli_session_ttl = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--log" || arg == "-v" || arg == "--verbose") {
            cli_log = true;
        } else if (arg == "--no-log") {
            cli_log = false;
        } else if (arg == "--log-level" && i + 1 < argc) {
            cli_log_level = argv[++i];
        } else if ((arg == "--env" || arg == "-e") && i + 1 < argc) {
            cli_env_file = argv[++i];
        }
    }

    auto config = machinebridge::load_config(cli_env_file);
    if (cli_tunnel.has_value()) {
        config.expose_tunnel = *cli_tunnel;
    }
    if (cli_tunnel_token.has_value()) {
        config.tunnel_token = cli_tunnel_token;
    }
    if (cli_tunnel_protocol.has_value()) {
        config.tunnel_protocol = *cli_tunnel_protocol;
    }
    if (cli_port.has_value()) {
        config.edge_port = *cli_port;
    }
    if (cli_host.has_value()) {
        config.edge_host = *cli_host;
    }
    if (cli_api_key.has_value()) {
        config.api_key = *cli_api_key;
    }
    if (cli_workspace.has_value()) {
        config.workspace_root = cli_workspace;
    }
    if (cli_shell.has_value()) {
        config.default_shell = cli_shell;
    }
    if (cli_max_sessions.has_value()) {
        config.max_sessions = *cli_max_sessions;
    }
    if (cli_session_ttl.has_value()) {
        config.session_ttl_seconds = *cli_session_ttl;
    }
    if (cli_log.has_value()) {
        config.verbose_logging = *cli_log;
    }
    if (cli_log_level.has_value()) {
        config.log_level = *cli_log_level;
    }

    // Initialize centralized logger
    auto& logger = machinebridge::Logger::instance();
    logger.set_enabled(config.verbose_logging);
    logger.set_level(machinebridge::Logger::string_to_level(config.log_level));
    if (is_stdio) {
        logger.set_use_stderr(true);
    }
    auto pty_manager = std::make_shared<machinebridge::PtyManager>(
        config.max_sessions,
        config.max_buffer_bytes
    );
    auto fs_manager = std::make_shared<machinebridge::FilesystemManager>(config.workspace_root);
    auto sessions = std::make_shared<machinebridge::InMemorySessionStore>();
    auto executor = std::make_shared<machinebridge::CommandExecutor>(pty_manager);
    auto tunnel_manager = std::make_shared<machinebridge::TunnelManager>();
    auto oauth_store = std::make_shared<machinebridge::OAuthStore>();

    machinebridge::McpContext mcp_ctx;
    mcp_ctx.executor = executor;
    mcp_ctx.fs_manager = fs_manager;
    mcp_ctx.sessions = sessions;
    mcp_ctx.pty_manager = pty_manager;
    mcp_ctx.base_url = "http://localhost:" + std::to_string(config.edge_port);
    mcp_ctx.api_key = config.api_key;
    mcp_ctx.is_session_authenticated = [](const std::string&) { return true; };

    if (is_stdio) {
        std::cerr << "[MachineBridge Server] Running in MCP stdio mode\n" << std::flush;
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        g_pty_manager = pty_manager;
        g_tunnel_manager = tunnel_manager;
        return machinebridge::run_mcp_stdio(mcp_ctx);
    }

    // HTTP / WebSocket Server mode using MachineBridgeServer
    static auto mb_server = std::make_shared<machinebridge::MachineBridgeServer>();

    std::signal(SIGINT, [](int) {
        g_server_running.store(false);
        if (mb_server) mb_server->stop();
    });
    std::signal(SIGTERM, [](int) {
        g_server_running.store(false);
        if (mb_server) mb_server->stop();
    });
#if defined(_WIN32)
    SetConsoleCtrlHandler([](DWORD ctrl) -> BOOL {
        if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT || ctrl == CTRL_CLOSE_EVENT) {
            g_server_running.store(false);
            if (mb_server) mb_server->stop();
            return TRUE;
        }
        return FALSE;
    }, TRUE);
#endif

    if (!mb_server->start(config)) {
        std::cerr << "[MachineBridge Server] Failed to bind to " << config.edge_host << ":" << config.edge_port << "\n";
        return 1;
    }

    // Set global pointers after start so signal handlers use the server's own components
    g_http_server = mb_server->http_server();
    g_pty_manager = mb_server->pty_manager();
    g_tunnel_manager = mb_server->tunnel_manager();

    std::cout << "[MachineBridge Server] Listening on http://" << config.edge_host << ":" << config.edge_port << "\n";
    std::cout << "[MachineBridge Server] API Key: " << config.api_key << "\n";
    std::cout << "[MachineBridge Server] Request Logging: " << (config.verbose_logging ? ("ENABLED (" + config.log_level + ")") : "DISABLED") << "\n";
    std::cout << "[MachineBridge Server] Environment: " << mb_server->environment().type_name << " (" << mb_server->environment().architecture << ")\n" << std::flush;

    while (g_server_running.load() && mb_server->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    machinebridge::log_info("SERVER", "Shutdown signal received, cleaning up resources...");
    std::cout << "\n[MachineBridge Server] Shutting down...\n" << std::flush;
    mb_server->stop();
    return 0;
}
