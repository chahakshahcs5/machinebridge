#include "machinebridge/protocol.hpp"

namespace machinebridge {

std::string terminal_signal_to_string(TerminalSignal sig) {
    switch (sig) {
        case TerminalSignal::SigInt: return "SIGINT";
        case TerminalSignal::SigTerm: return "SIGTERM";
        case TerminalSignal::SigHup: return "SIGHUP";
        case TerminalSignal::SigKill: return "SIGKILL";
    }
    return "SIGINT";
}

std::optional<TerminalSignal> string_to_terminal_signal(const std::string& str) {
    if (str == "SIGINT") return TerminalSignal::SigInt;
    if (str == "SIGTERM") return TerminalSignal::SigTerm;
    if (str == "SIGHUP") return TerminalSignal::SigHup;
    if (str == "SIGKILL") return TerminalSignal::SigKill;
    return std::nullopt;
}

std::string fs_action_to_string(FsAction action) {
    switch (action) {
        case FsAction::Read: return "read";
        case FsAction::Write: return "write";
        case FsAction::Delete: return "delete";
        case FsAction::List: return "list";
        case FsAction::Mkdir: return "mkdir";
        case FsAction::Move: return "move";
        case FsAction::Copy: return "copy";
        case FsAction::Stat: return "stat";
    }
    return "read";
}

std::optional<FsAction> string_to_fs_action(const std::string& str) {
    if (str == "read") return FsAction::Read;
    if (str == "write") return FsAction::Write;
    if (str == "delete") return FsAction::Delete;
    if (str == "list") return FsAction::List;
    if (str == "mkdir") return FsAction::Mkdir;
    if (str == "move") return FsAction::Move;
    if (str == "copy") return FsAction::Copy;
    if (str == "stat") return FsAction::Stat;
    return std::nullopt;
}

void to_json(nlohmann::json& j, const FsOperation& op) {
    j = nlohmann::json{{"type", op.type}};
    if (op.path) j["path"] = *op.path;
    if (op.content) j["content"] = *op.content;
    if (op.encoding) j["encoding"] = *op.encoding;
    if (op.append) j["append"] = *op.append;
    if (op.destination) j["destination"] = *op.destination;
    if (op.recursive) j["recursive"] = *op.recursive;
    if (op.offset) j["offset"] = *op.offset;
    if (op.length) j["length"] = *op.length;
    if (op.command) j["command"] = *op.command;
    if (op.cwd) j["cwd"] = *op.cwd;
    if (op.timeout_ms) j["timeoutMs"] = *op.timeout_ms;
}

void from_json(const nlohmann::json& j, FsOperation& op) {
    op.type = j.value("type", "");
    if (j.contains("path") && !j["path"].is_null()) op.path = j["path"].get<std::string>();
    if (j.contains("content") && !j["content"].is_null()) op.content = j["content"].get<std::string>();
    if (j.contains("encoding") && !j["encoding"].is_null()) op.encoding = j["encoding"].get<std::string>();
    if (j.contains("append") && !j["append"].is_null()) op.append = j["append"].get<bool>();
    if (j.contains("destination") && !j["destination"].is_null()) op.destination = j["destination"].get<std::string>();
    if (j.contains("recursive") && !j["recursive"].is_null()) op.recursive = j["recursive"].get<bool>();
    if (j.contains("offset") && !j["offset"].is_null()) op.offset = j["offset"].get<int64_t>();
    if (j.contains("length") && !j["length"].is_null()) op.length = j["length"].get<int64_t>();
    if (j.contains("command") && !j["command"].is_null()) op.command = j["command"].get<std::string>();
    if (j.contains("cwd") && !j["cwd"].is_null()) op.cwd = j["cwd"].get<std::string>();
    if (j.contains("timeoutMs") && !j["timeoutMs"].is_null()) op.timeout_ms = j["timeoutMs"].get<int64_t>();
}

void to_json(nlohmann::json& j, const FsBatchResultItem& item) {
    j = nlohmann::json{
        {"index", item.index},
        {"type", item.type},
        {"success", item.success}
    };
    if (item.path) j["path"] = *item.path;
    if (item.command) j["command"] = *item.command;
    if (item.error) j["error"] = *item.error;
    if (!item.data.is_null()) j["data"] = item.data;
    if (item.duration_ms) j["durationMs"] = *item.duration_ms;
}

void from_json(const nlohmann::json& j, FsBatchResultItem& item) {
    item.index = j.value("index", 0);
    item.type = j.value("type", "");
    item.success = j.value("success", false);
    if (j.contains("path") && !j["path"].is_null()) item.path = j["path"].get<std::string>();
    if (j.contains("command") && !j["command"].is_null()) item.command = j["command"].get<std::string>();
    if (j.contains("error") && !j["error"].is_null()) item.error = j["error"].get<std::string>();
    if (j.contains("data")) item.data = j["data"];
    if (j.contains("durationMs") && !j["durationMs"].is_null()) item.duration_ms = j["durationMs"].get<double>();
}

void to_json(nlohmann::json& j, const FsBatchSummary& s) {
    j = nlohmann::json{
        {"total", s.total},
        {"passed", s.passed},
        {"failed", s.failed},
        {"skipped", s.skipped}
    };
}

void from_json(const nlohmann::json& j, FsBatchSummary& s) {
    s.total = j.value("total", 0);
    s.passed = j.value("passed", 0);
    s.failed = j.value("failed", 0);
    s.skipped = j.value("skipped", 0);
}

std::string encode_message(const nlohmann::json& message) {
    return message.dump();
}

ProtocolMessage decode_message(std::string_view data) {
    ProtocolMessage msg;
    msg.raw = nlohmann::json::parse(data);
    msg.type = msg.raw.value("type", "");
    if (msg.raw.contains("requestId") && !msg.raw["requestId"].is_null()) {
        msg.request_id = msg.raw["requestId"].get<std::string>();
    }
    return msg;
}

} // namespace machinebridge
