#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace machinebridge {

enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

struct WsFrame {
    bool fin = true;
    WsOpcode opcode = WsOpcode::Text;
    bool masked = false;
    uint32_t masking_key = 0;
    std::string payload;
};

// Computes Sec-WebSocket-Accept given Sec-WebSocket-Key
std::string compute_websocket_accept(const std::string& client_key);

// Encodes a text message into an unmasked (server-to-client) WebSocket frame
std::vector<uint8_t> encode_ws_frame(const std::string& text, WsOpcode opcode = WsOpcode::Text);

// Encodes a text message into a masked (client-to-server) WebSocket frame
std::vector<uint8_t> encode_ws_frame_client(const std::string& text, WsOpcode opcode = WsOpcode::Text);

// Encodes a WebSocket close frame
std::vector<uint8_t> encode_ws_close_frame(uint16_t code = 1000, const std::string& reason = "");

// Parses raw socket buffer into WebSocket frame. Returns parsed frame and consumed byte count.
bool parse_ws_frame(const uint8_t* buffer, size_t length, WsFrame& out_frame, size_t& bytes_consumed);

} // namespace machinebridge
