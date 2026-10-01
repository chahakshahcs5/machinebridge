#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace machinebridge {

struct Identity {
    std::string machine_id;
    std::string private_key_pem;
    std::string public_key_pem;
    std::vector<uint8_t> private_key_raw; // 64 bytes (seed + pubkey or expanded key)
    std::vector<uint8_t> public_key_raw;  // 32 bytes
};

// Generates a new Ed25519 identity for a given machineId
Identity generate_identity(const std::string& machine_id);

// Canonicalizes JSON by recursively sorting all object keys alphabetically
std::string canonical_json(const nlohmann::json& j);

// Computes SHA-256 in lowercase hex
std::string sha256_hex(std::string_view data);

// Computes SHA-256 raw bytes (32 bytes)
std::vector<uint8_t> sha256_raw(std::string_view data);

// Computes SHA-256 hash of the canonical JSON representation
std::string payload_hash(const nlohmann::json& payload);

// Creates signing string: machineId.timestamp.nonce.requestId.hash
std::string make_signing_string(
    const std::string& machine_id,
    int64_t timestamp,
    const std::string& nonce,
    const std::string& request_id,
    const std::string& hash
);

// Signs value using Ed25519 private key, returns base64url string
std::string sign_request(const Identity& identity, const std::string& value);
std::string sign_request(const std::vector<uint8_t>& private_key_raw, const std::string& value);

// Verifies value signature using Ed25519 public key
bool verify_request(const std::vector<uint8_t>& public_key_raw, const std::string& value, const std::string& signature_base64url);
bool verify_request_pem(const std::string& public_key_pem, const std::string& value, const std::string& signature_base64url);

// Constant-time string equality comparison
bool secure_equal(std::string_view left, std::string_view right);

// Generates 24 cryptographically secure random bytes as base64url string
std::string create_nonce();

// Base64 and Base64URL encoding/decoding utilities
std::string base64_encode(const uint8_t* data, size_t length);
std::string base64url_encode(const uint8_t* data, size_t length);
std::vector<uint8_t> base64_decode(std::string_view text);
std::vector<uint8_t> base64url_decode(std::string_view text);

// Verifies OAuth 2.0 PKCE challenge (S256 and plain)
bool verify_pkce(const std::string& verifier, const std::string& challenge, const std::string& method = "S256");

} // namespace machinebridge
