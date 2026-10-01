#include "machinebridge/websocket.hpp"
#include "machinebridge/crypto.hpp"
#include <algorithm>
#include <cstring>
#include <random>

namespace machinebridge {

// ============================================================================
// SHA-1 implementation specifically for RFC 6455 WebSocket Handshake
// ============================================================================

namespace {

inline uint32_t rol32(uint32_t value, size_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    uint32_t block[80];

    for (size_t i = 0; i < 16; ++i) {
        block[i] = (static_cast<uint32_t>(buffer[i * 4]) << 24) |
                   (static_cast<uint32_t>(buffer[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(buffer[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(buffer[i * 4 + 3]));
    }
    for (size_t i = 16; i < 80; ++i) {
        block[i] = rol32(block[i - 3] ^ block[i - 8] ^ block[i - 14] ^ block[i - 16], 1);
    }

    for (size_t i = 0; i < 20; ++i) {
        uint32_t t = rol32(a, 5) + ((b & c) | (~b & d)) + e + 0x5a827999 + block[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    for (size_t i = 20; i < 40; ++i) {
        uint32_t t = rol32(a, 5) + (b ^ c ^ d) + e + 0x6ed9eba1 + block[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    for (size_t i = 40; i < 60; ++i) {
        uint32_t t = rol32(a, 5) + ((b & c) | (b & d) | (c & d)) + e + 0x8f1bbcdc + block[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    for (size_t i = 60; i < 80; ++i) {
        uint32_t t = rol32(a, 5) + (b ^ c ^ d) + e + 0xca62c1d6 + block[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

std::vector<uint8_t> sha1(std::string_view data) {
    uint32_t state[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    uint64_t total_bits = data.size() * 8;
    uint8_t buffer[64];
    size_t offset = 0;

    while (offset + 64 <= data.size()) {
        std::memcpy(buffer, data.data() + offset, 64);
        sha1_transform(state, buffer);
        offset += 64;
    }

    size_t rem = data.size() - offset;
    std::memcpy(buffer, data.data() + offset, rem);
    buffer[rem++] = 0x80;

    if (rem > 56) {
        std::memset(buffer + rem, 0, 64 - rem);
        sha1_transform(state, buffer);
        rem = 0;
    }

    std::memset(buffer + rem, 0, 56 - rem);
    for (int i = 0; i < 8; ++i) {
        buffer[56 + i] = static_cast<uint8_t>((total_bits >> (56 - i * 8)) & 0xFF);
    }
    sha1_transform(state, buffer);

    std::vector<uint8_t> digest(20);
    for (size_t i = 0; i < 5; ++i) {
        digest[i * 4]     = static_cast<uint8_t>((state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xFF);
    }
    return digest;
}

} // namespace

std::string compute_websocket_accept(const std::string& client_key) {
    static const std::string WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = client_key + WS_GUID;
    auto hash = sha1(concat);
    return base64_encode(hash.data(), hash.size());
}

std::vector<uint8_t> encode_ws_frame(const std::string& text, WsOpcode opcode) {
    std::vector<uint8_t> frame;
    frame.reserve(text.size() + 10);

    uint8_t b0 = 0x80 | (static_cast<uint8_t>(opcode) & 0x0F); // FIN + opcode
    frame.push_back(b0);

    size_t len = text.size();
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len)); // Unmasked
    } else if (len <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    frame.insert(frame.end(), text.begin(), text.end());
    return frame;
}

std::vector<uint8_t> encode_ws_frame_client(const std::string& text, WsOpcode opcode) {
    std::vector<uint8_t> frame;
    frame.reserve(text.size() + 14);

    uint8_t b0 = 0x80 | (static_cast<uint8_t>(opcode) & 0x0F);
    frame.push_back(b0);

    size_t len = text.size();
    if (len < 126) {
        frame.push_back(0x80 | static_cast<uint8_t>(len)); // Masked
    } else if (len <= 0xFFFF) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    // 4-byte random mask
    std::random_device rd;
    uint8_t mask[4] = {
        static_cast<uint8_t>(rd()),
        static_cast<uint8_t>(rd()),
        static_cast<uint8_t>(rd()),
        static_cast<uint8_t>(rd())
    };
    frame.insert(frame.end(), mask, mask + 4);

    for (size_t i = 0; i < len; ++i) {
        frame.push_back(static_cast<uint8_t>(text[i]) ^ mask[i % 4]);
    }

    return frame;
}

std::vector<uint8_t> encode_ws_close_frame(uint16_t code, const std::string& reason) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload += reason;
    return encode_ws_frame(payload, WsOpcode::Close);
}

bool parse_ws_frame(const uint8_t* buffer, size_t length, WsFrame& out_frame, size_t& bytes_consumed) {
    if (length < 2) return false;

    uint8_t b0 = buffer[0];
    uint8_t b1 = buffer[1];

    out_frame.fin = (b0 & 0x80) != 0;
    out_frame.opcode = static_cast<WsOpcode>(b0 & 0x0F);
    out_frame.masked = (b1 & 0x80) != 0;

    uint64_t payload_len = (b1 & 0x7F);
    size_t header_size = 2;

    if (payload_len == 126) {
        if (length < header_size + 2) return false;
        payload_len = (static_cast<uint64_t>(buffer[2]) << 8) | buffer[3];
        header_size += 2;
    } else if (payload_len == 127) {
        if (length < header_size + 8) return false;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | buffer[header_size + i];
        }
        header_size += 8;
    }

    uint8_t mask[4] = {0};
    if (out_frame.masked) {
        if (length < header_size + 4) return false;
        std::memcpy(mask, buffer + header_size, 4);
        header_size += 4;
    }

    if (length < header_size + payload_len) return false;

    out_frame.payload.resize(payload_len);
    const uint8_t* pdata = buffer + header_size;
    for (size_t i = 0; i < payload_len; ++i) {
        out_frame.payload[i] = static_cast<char>(out_frame.masked ? (pdata[i] ^ mask[i % 4]) : pdata[i]);
    }

    bytes_consumed = header_size + static_cast<size_t>(payload_len);
    return true;
}

} // namespace machinebridge
