#include "machinebridge/crypto.hpp"
#include "machinebridge/shared.hpp"
#include "machinebridge/websocket.hpp"
#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <conio.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
using SOCKET = int;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

namespace {

struct ParsedServerUrl {
    std::string host = "localhost";
    uint16_t port = 8080;
};

ParsedServerUrl parse_server_url(const std::string& url_str) {
    ParsedServerUrl res;
    std::string s = url_str;
    if (s.rfind("http://", 0) == 0) s = s.substr(7);
    else if (s.rfind("https://", 0) == 0) s = s.substr(8);
    else if (s.rfind("ws://", 0) == 0) s = s.substr(5);
    else if (s.rfind("wss://", 0) == 0) s = s.substr(6);

    size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);

    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        res.host = s.substr(0, colon);
        res.port = static_cast<uint16_t>(std::stoi(s.substr(colon + 1)));
    } else {
        res.host = s;
        res.port = 80;
    }
    return res;
}

uintptr_t connect_tcp(const std::string& host, uint16_t port) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    addrinfo hints{}, *servinfo = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &servinfo) != 0) {
        return 0;
    }

    SOCKET sock = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(servinfo);
        return 0;
    }

    if (connect(sock, servinfo->ai_addr, static_cast<int>(servinfo->ai_addrlen)) != 0) {
        closesocket(sock);
        freeaddrinfo(servinfo);
        return 0;
    }

    freeaddrinfo(servinfo);
    return static_cast<uintptr_t>(sock);
}

bool send_all_data(uintptr_t sock, const void* data, size_t len) {
    const char* ptr = reinterpret_cast<const char*>(data);
    size_t rem = len;
    while (rem > 0) {
        int sent = send(static_cast<SOCKET>(sock), ptr, static_cast<int>(rem), 0);
        if (sent <= 0) return false;
        rem -= sent;
        ptr += sent;
    }
    return true;
}

} // namespace

#if defined(_WIN32)
static DWORD g_orig_in_mode = 0;
static DWORD g_orig_out_mode = 0;

void enable_raw_mode() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(hIn, &g_orig_in_mode);
    GetConsoleMode(hOut, &g_orig_out_mode);

    DWORD raw_in = g_orig_in_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    raw_in |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(hIn, raw_in);

    DWORD raw_out = g_orig_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    SetConsoleMode(hOut, raw_out);
}

void restore_console_mode() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(hIn, g_orig_in_mode);
    SetConsoleMode(hOut, g_orig_out_mode);
}
#else
static termios g_orig_termios;

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    termios raw = g_orig_termios;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void restore_console_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
}
#endif

static std::atomic<bool> g_cli_running{true};
static uintptr_t g_cli_sock = 0;
static std::string g_active_session_id;

void cli_signal_handler(int) {
    if (g_cli_sock && !g_active_session_id.empty()) {
        nlohmann::json sig_msg = {
            {"type", "terminal.signal"},
            {"sessionId", g_active_session_id},
            {"signal", "SIGINT"}
        };
        auto frame = machinebridge::encode_ws_frame_client(sig_msg.dump());
        send_all_data(g_cli_sock, frame.data(), frame.size());
    } else {
        g_cli_running.store(false);
    }
}

int main(int argc, char* argv[]) {
    std::string server_url = "http://localhost:8080";
    std::string api_key = "machinebridge-dev-key";
    const char* env_key = std::getenv("MACHINEBRIDGE_API_KEY");
    if (env_key && env_key[0] != '\0') api_key = env_key;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "MachineBridge CLI Client (C++20)\n"
                      << "Usage:\n"
                      << "  machinebridge-cli [options]\n\n"
                      << "Options:\n"
                      << "  --server <url>    Server base URL (default: http://localhost:8080)\n"
                      << "  --api-key <key>   API authentication key\n"
                      << "  --help, -h        Show this help message\n";
            return 0;
        }
        if (arg == "--server" && i + 1 < argc) {
            server_url = argv[++i];
        } else if (arg == "--api-key" && i + 1 < argc) {
            api_key = argv[++i];
        }
    }

    auto parsed_url = parse_server_url(server_url);

    // 1. Send HTTP POST /v1/terminal/sessions
    uintptr_t http_sock = connect_tcp(parsed_url.host, parsed_url.port);
    if (!http_sock) {
        std::cerr << "Failed to connect to MachineBridge server at " << parsed_url.host << ":" << parsed_url.port << "\n";
        return 1;
    }

    std::string create_req = "POST /v1/terminal/sessions HTTP/1.1\r\n"
                             "Host: " + parsed_url.host + ":" + std::to_string(parsed_url.port) + "\r\n"
                             "X-API-Key: " + api_key + "\r\n"
                             "Content-Length: 0\r\n\r\n";

    if (!send_all_data(http_sock, create_req.data(), create_req.size())) {
        std::cerr << "Failed to send create session request\n";
        closesocket(static_cast<SOCKET>(http_sock));
        return 1;
    }

    std::vector<char> http_buf(4096);
    int r = recv(static_cast<SOCKET>(http_sock), http_buf.data(), static_cast<int>(http_buf.size() - 1), 0);
    closesocket(static_cast<SOCKET>(http_sock));

    if (r <= 0) {
        std::cerr << "Failed to receive session response\n";
        return 1;
    }
    http_buf[r] = '\0';
    std::string resp_str(http_buf.data(), r);
    size_t bpos = resp_str.find("\r\n\r\n");
    if (bpos == std::string::npos) {
        std::cerr << "Invalid HTTP response from server\n";
        return 1;
    }

    std::string session_id;
    try {
        auto session_json = nlohmann::json::parse(resp_str.substr(bpos + 4));
        session_id = session_json.value("sessionId", "");
    } catch (...) {}

    if (session_id.empty()) {
        std::cerr << "Server rejected session creation:\n" << resp_str << "\n";
        return 1;
    }

    g_active_session_id = session_id;

    // 2. Open WebSocket connection to /v1/terminal/sessions/<sessionId>?apiKey=<apiKey>
    uintptr_t ws_sock = connect_tcp(parsed_url.host, parsed_url.port);
    if (!ws_sock) {
        std::cerr << "Failed to open WebSocket connection\n";
        return 1;
    }
    g_cli_sock = ws_sock;

    std::string ws_key = machinebridge::create_nonce().substr(0, 22) + "==";
    std::string upgrade_req = "GET /v1/terminal/sessions/" + session_id + "?apiKey=" + api_key + " HTTP/1.1\r\n"
                              "Host: " + parsed_url.host + ":" + std::to_string(parsed_url.port) + "\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Key: " + ws_key + "\r\n"
                              "Sec-WebSocket-Version: 13\r\n\r\n";

    if (!send_all_data(ws_sock, upgrade_req.data(), upgrade_req.size())) {
        std::cerr << "Failed to send WebSocket upgrade request\n";
        closesocket(static_cast<SOCKET>(ws_sock));
        return 1;
    }

    r = recv(static_cast<SOCKET>(ws_sock), http_buf.data(), static_cast<int>(http_buf.size() - 1), 0);
    if (r <= 0 || std::string(http_buf.data(), r).find("101 Switching Protocols") == std::string::npos) {
        std::cerr << "WebSocket upgrade failed:\n" << std::string(http_buf.data(), r) << "\n";
        closesocket(static_cast<SOCKET>(ws_sock));
        return 1;
    }

    enable_raw_mode();
    std::signal(SIGINT, cli_signal_handler);

    // Send terminal.create
    int cols = 120, rows = 40;
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#endif

    nlohmann::json create_msg = {
        {"type", "terminal.create"},
        {"requestId", session_id},
        {"cols", cols},
        {"rows", rows},
        {"shell", nullptr},
        {"cwd", "."}
    };
    auto initial_frame = machinebridge::encode_ws_frame_client(create_msg.dump());
    send_all_data(ws_sock, initial_frame.data(), initial_frame.size());

    // Reader thread: reads output from server and writes to stdout
    std::thread reader([ws_sock, session_id]() {
        std::vector<uint8_t> buffer;
        buffer.reserve(65536);
        uint8_t chunk[4096];

        while (g_cli_running.load()) {
            int rec = recv(static_cast<SOCKET>(ws_sock), reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
            if (rec <= 0) break;
            buffer.insert(buffer.end(), chunk, chunk + rec);

            while (true) {
                machinebridge::WsFrame frame;
                size_t consumed = 0;
                if (!machinebridge::parse_ws_frame(buffer.data(), buffer.size(), frame, consumed)) {
                    break;
                }
                buffer.erase(buffer.begin(), buffer.begin() + consumed);

                if (frame.opcode == machinebridge::WsOpcode::Close) {
                    g_cli_running.store(false);
                    return;
                }

                if (frame.opcode == machinebridge::WsOpcode::Text) {
                    try {
                        auto msg = nlohmann::json::parse(frame.payload);
                        std::string type = msg.value("type", "");
                        if (type == "terminal.output") {
                            std::string data = msg.value("data", "");
                            std::cout.write(data.data(), data.size());
                            std::cout.flush();
                        } else if (type == "terminal.closed") {
                            g_cli_running.store(false);
                            return;
                        } else if (type == "error") {
                            std::cerr << "\n[MachineBridge] " << msg.value("code", "") << ": " << msg.value("message", "") << "\n";
                            g_cli_running.store(false);
                            return;
                        }
                    } catch (...) {}
                }
            }
        }
        g_cli_running.store(false);
    });

    // Stdin loop
#if defined(_WIN32)
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    while (g_cli_running.load()) {
        DWORD read_count = 0;
        INPUT_RECORD recs[128];
        if (ReadConsoleInputW(hStdIn, recs, 128, &read_count) && read_count > 0) {
            std::string text;
            for (DWORD i = 0; i < read_count; ++i) {
                if (recs[i].EventType == KEY_EVENT && recs[i].Event.KeyEvent.bKeyDown) {
                    WCHAR ch = recs[i].Event.KeyEvent.uChar.UnicodeChar;
                    if (ch != 0) {
                        int sz = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, nullptr, 0, nullptr, nullptr);
                        std::string utf8(sz, 0);
                        WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8.data(), sz, nullptr, nullptr);
                        text += utf8;
                    }
                }
            }
            if (!text.empty()) {
                nlohmann::json input_msg = {
                    {"type", "terminal.input"},
                    {"sessionId", session_id},
                    {"data", text}
                };
                auto frame = machinebridge::encode_ws_frame_client(input_msg.dump());
                send_all_data(ws_sock, frame.data(), frame.size());
            }
        }
    }
#else
    char buf[128];
    while (g_cli_running.load()) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) {
            std::string text(buf, n);
            nlohmann::json input_msg = {
                {"type", "terminal.input"},
                {"sessionId", session_id},
                {"data", text}
            };
            auto frame = machinebridge::encode_ws_frame_client(input_msg.dump());
            send_all_data(ws_sock, frame.data(), frame.size());
        }
    }
#endif

    restore_console_mode();
    closesocket(static_cast<SOCKET>(ws_sock));
    if (reader.joinable()) reader.join();

    std::cout << "\nConnection closed.\n";
    return 0;
}
