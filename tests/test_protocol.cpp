#include "machinebridge/protocol.hpp"
#include <cassert>
#include <iostream>

int main() {
    std::cout << "[test_protocol] Running protocol tests...\n";

    // 1. Terminal signals
    assert(machinebridge::terminal_signal_to_string(machinebridge::TerminalSignal::SigInt) == "SIGINT");
    assert(machinebridge::string_to_terminal_signal("SIGKILL") == machinebridge::TerminalSignal::SigKill);
    assert(!machinebridge::string_to_terminal_signal("SIGUNKNOWN").has_value());
    std::cout << "  - Terminal signal tests passed\n";

    // 2. FsAction
    assert(machinebridge::fs_action_to_string(machinebridge::FsAction::Read) == "read");
    assert(machinebridge::string_to_fs_action("delete") == machinebridge::FsAction::Delete);
    assert(!machinebridge::string_to_fs_action("unknown").has_value());
    std::cout << "  - FsAction tests passed\n";

    // 3. Encode & decode message
    nlohmann::json hello_msg = {
        {"type", "device.hello"},
        {"requestId", "req-1"},
        {"machineId", "m-123"},
        {"timestamp", 1700000000},
        {"nonce", "nonce-1"},
        {"signature", "sig-1"},
        {"payloadHash", "hash-1"}
    };
    std::string encoded = machinebridge::encode_message(hello_msg);
    auto decoded = machinebridge::decode_message(encoded);
    assert(decoded.type == "device.hello");
    assert(decoded.request_id == "req-1");
    assert(decoded.get<std::string>("machineId") == "m-123");
    std::cout << "  - Message encode/decode tests passed\n";

    // 4. FsOperation serialization
    machinebridge::FsOperation op;
    op.type = "write";
    op.path = "test.txt";
    op.content = "hello";
    op.append = false;
    op.encoding = "utf8";

    nlohmann::json op_j = op;
    machinebridge::FsOperation op_restored = op_j.get<machinebridge::FsOperation>();
    assert(op_restored.type == "write");
    assert(op_restored.path == "test.txt");
    assert(op_restored.content == "hello");
    assert(!*op_restored.append);
    std::cout << "  - FsOperation serialization tests passed\n";

    std::cout << "[test_protocol] All protocol tests passed!\n";
    return 0;
}
