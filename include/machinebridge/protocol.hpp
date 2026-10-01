#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace machinebridge {

enum class TerminalSignal {
    SigInt,
    SigTerm,
    SigHup,
    SigKill
};

std::string terminal_signal_to_string(TerminalSignal sig);
std::optional<TerminalSignal> string_to_terminal_signal(const std::string& str);

enum class FsAction {
    Read,
    Write,
    Delete,
    List,
    Mkdir,
    Move,
    Copy,
    Stat
};

std::string fs_action_to_string(FsAction action);
std::optional<FsAction> string_to_fs_action(const std::string& str);

struct FsOperation {
    std::string type; // write, read, mkdir, delete, move, copy, list, command
    std::optional<std::string> path;
    std::optional<std::string> content;
    std::optional<std::string> encoding; // "utf8" or "base64"
    std::optional<bool> append;
    std::optional<std::string> destination;
    std::optional<bool> recursive;
    std::optional<int64_t> offset;
    std::optional<int64_t> length;
    std::optional<std::string> command;
    std::optional<std::string> cwd;
    std::optional<int64_t> timeout_ms;
};

void to_json(nlohmann::json& j, const FsOperation& op);
void from_json(const nlohmann::json& j, FsOperation& op);

struct FsBatchResultItem {
    int index = 0;
    std::string type;
    std::optional<std::string> path;
    std::optional<std::string> command;
    bool success = false;
    std::optional<std::string> error;
    nlohmann::json data;
    std::optional<double> duration_ms;
};

void to_json(nlohmann::json& j, const FsBatchResultItem& item);
void from_json(const nlohmann::json& j, FsBatchResultItem& item);

struct FsBatchSummary {
    int total = 0;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
};

void to_json(nlohmann::json& j, const FsBatchSummary& s);
void from_json(const nlohmann::json& j, FsBatchSummary& s);

// General protocol message representation
struct ProtocolMessage {
    std::string type;
    std::optional<std::string> request_id;
    nlohmann::json raw;

    template <typename T>
    T get(const std::string& key, const T& fallback = T{}) const {
        if (raw.contains(key) && !raw[key].is_null()) {
            return raw[key].get<T>();
        }
        return fallback;
    }
};

std::string encode_message(const nlohmann::json& message);
ProtocolMessage decode_message(std::string_view data);

} // namespace machinebridge
