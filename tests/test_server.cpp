#include "machinebridge/config.hpp"
#include "machinebridge/executor.hpp"
#include "machinebridge/fs.hpp"
#include "machinebridge/http_server.hpp"
#include "machinebridge/oauth.hpp"
#include "machinebridge/pty.hpp"
#include "machinebridge/session_store.hpp"
#include "machinebridge/tunnel.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

namespace {

std::string http_get(uint16_t port, const std::string& path, const std::string& api_key = "") {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(sock != INVALID_SOCKET);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int conn = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (conn != 0) {
        closesocket(sock);
        return "";
    }

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n";
    if (!api_key.empty()) {
        req += "X-API-Key: " + api_key + "\r\n";
    }
    req += "Connection: close\r\n\r\n";

    send(sock, req.data(), static_cast<int>(req.size()), 0);

    std::string resp;
    char buf[4096];
    while (true) {
        int r = recv(sock, buf, sizeof(buf), 0);
        if (r <= 0) break;
        resp.append(buf, r);
    }

    closesocket(sock);
    return resp;
}

} // namespace

int main() {
    std::cout << "[test_server] Running server HTTP tests...\n";

    machinebridge::Config config;
    config.edge_port = 18080;
    config.api_key = "test-secret-key";

    auto pty_mgr = std::make_shared<machinebridge::PtyManager>(4, 1024 * 1024);
    auto fs_mgr = std::make_shared<machinebridge::FilesystemManager>();
    auto sessions = std::make_shared<machinebridge::InMemorySessionStore>();
    auto executor = std::make_shared<machinebridge::CommandExecutor>(pty_mgr);
    auto tunnel_mgr = std::make_shared<machinebridge::TunnelManager>();
    auto oauth_store = std::make_shared<machinebridge::OAuthStore>();

    auto server = std::make_shared<machinebridge::HttpServer>(
        config,
        pty_mgr,
        fs_mgr,
        sessions,
        executor,
        tunnel_mgr,
        oauth_store
    );

    bool ok = server->start();
    assert(ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 1. Health check (unauthenticated)
    std::string health_resp = http_get(config.edge_port, "/health");
    assert(health_resp.find("200 OK") != std::string::npos);
    assert(health_resp.find("\"machinebridge-unified\"") != std::string::npos);
    std::cout << "  - GET /health passed\n";

    // 2. Ready check
    std::string ready_resp = http_get(config.edge_port, "/ready");
    assert(ready_resp.find("200 OK") != std::string::npos);
    assert(ready_resp.find("\"ready\"") != std::string::npos);
    std::cout << "  - GET /ready passed\n";

    // 3. Unauthenticated access to /tools must fail
    std::string unauth_resp = http_get(config.edge_port, "/tools/stat_file");
    assert(unauth_resp.find("401") != std::string::npos);
    std::cout << "  - Auth check on /tools passed\n";

    // 4. Authenticated access to /tools
    std::string auth_resp = http_get(config.edge_port, "/tools", config.api_key);
    assert(auth_resp.find("200 OK") != std::string::npos);
    assert(auth_resp.find("\"execute_command\"") != std::string::npos);
    std::cout << "  - GET /tools passed\n";

    server->stop();
    pty_mgr->close_all();
    std::cout << "[test_server] All server tests passed!\n";
    return 0;
}
