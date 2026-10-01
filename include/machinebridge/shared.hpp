#pragma once

#include "machinebridge/protocol.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace machinebridge {

std::string create_uuid();
int64_t now_seconds();
int64_t now_millis();
std::string iso8601_now();
std::string iso8601_time(int64_t epoch_seconds);

bool verify_api_key(const std::string& expected_key, const std::string& provided_key);

struct BatchCommandResult {
    std::string command;
    int exit_code = 0;
    std::string status; // "success", "failed", "skipped"
    std::string output;
    std::optional<double> duration_ms;
};

void to_json(nlohmann::json& j, const BatchCommandResult& res);
void from_json(const nlohmann::json& j, BatchCommandResult& res);

struct BatchExecuteResult {
    bool ok = true;
    std::string session_id;
    FsBatchSummary summary;
    std::vector<BatchCommandResult> results;
};

void to_json(nlohmann::json& j, const BatchExecuteResult& res);
void from_json(const nlohmann::json& j, BatchExecuteResult& res);

struct FsBatchResult {
    bool ok = true;
    int total = 0;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    std::vector<FsBatchResultItem> results;
};

void to_json(nlohmann::json& j, const FsBatchResult& res);
void from_json(const nlohmann::json& j, FsBatchResult& res);

std::string format_batch_markdown(const BatchExecuteResult& result);
std::string format_batch_json(const BatchExecuteResult& result);

std::string format_fs_batch_markdown(const FsBatchResult& result);
std::string format_fs_batch_json(const FsBatchResult& result);

} // namespace machinebridge
