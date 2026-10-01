#include "machinebridge/crypto.hpp"
#include <cassert>
#include <iostream>

int main() {
    std::cout << "[test_crypto] Running crypto tests...\n";

    // 1. SHA-256
    std::string text = "hello machinebridge";
    std::string hash = machinebridge::sha256_hex(text);
    assert(hash.size() == 64);
    assert(hash == "955ca33ff17c7562098bc58b29ecdb8f01b3bebfd69a32cba9e46a78465c4ec4");
    std::cout << "  - SHA-256 test passed\n";

    // 2. Canonical JSON
    nlohmann::json unordered = {{"z", 1}, {"a", 2}, {"m", {{"y", 3}, {"b", 4}}}};
    std::string canonical = machinebridge::canonical_json(unordered);
    assert(canonical == "{\"a\":2,\"m\":{\"b\":4,\"y\":3},\"z\":1}");
    std::cout << "  - Canonical JSON test passed\n";

    // 3. Payload hash
    std::string p_hash = machinebridge::payload_hash(unordered);
    assert(p_hash.size() == 64);
    std::cout << "  - Payload hash test passed\n";

    // 4. Secure equal
    assert(machinebridge::secure_equal("secret_key_123", "secret_key_123"));
    assert(!machinebridge::secure_equal("secret_key_123", "secret_key_456"));
    assert(!machinebridge::secure_equal("short", "longer_string"));
    std::cout << "  - Secure equal test passed\n";

    // 5. Nonce creation
    std::string nonce1 = machinebridge::create_nonce();
    std::string nonce2 = machinebridge::create_nonce();
    assert(!nonce1.empty());
    assert(!nonce2.empty());
    assert(nonce1 != nonce2);
    std::cout << "  - Nonce creation test passed\n";

    // 6. Ed25519 key generation, sign & verify
    auto id = machinebridge::generate_identity("machine-test-1");
    assert(!id.private_key_pem.empty());
    assert(!id.public_key_pem.empty());
    assert(id.public_key_raw.size() == 32);

    std::string msg = "MachineBridge Agent Request 12345";
    std::string sig = machinebridge::sign_request(id, msg);
    assert(!sig.empty());

    bool ok = machinebridge::verify_request(id.public_key_raw, msg, sig);
    assert(ok);

    bool bad = machinebridge::verify_request(id.public_key_raw, msg + "_corrupted", sig);
    assert(!bad);

    bool pem_ok = machinebridge::verify_request_pem(id.public_key_pem, msg, sig);
    assert(pem_ok);
    std::cout << "  - Ed25519 sign & verify test passed\n";

    // 7. PKCE verification
    std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    std::string challenge = "E9Melhoa2OwvFrGMTJguCH5rtG6470-ZALx822NKQDU";
    assert(machinebridge::verify_pkce(verifier, challenge, "S256"));
    assert(!machinebridge::verify_pkce("wrong_verifier", challenge, "S256"));
    assert(machinebridge::verify_pkce("plain_secret", "plain_secret", "plain"));
    assert(!machinebridge::verify_pkce("plain_secret", "wrong_plain", "plain"));
    std::cout << "  - PKCE verification test passed\n";

    std::cout << "[test_crypto] All crypto tests passed!\n";
    return 0;
}
