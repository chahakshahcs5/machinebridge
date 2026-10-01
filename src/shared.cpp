#include "machinebridge/shared.hpp"
#include "machinebridge/crypto.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

namespace machinebridge {

std::string create_uuid() {
    uint8_t bytes[16];
#if defined(_WIN32)
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    std::random_device rd;
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = static_cast<uint8_t>(rd());
#endif

    bytes[6] = (bytes[6] & 0x0F) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80; // variant RFC 4122

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) oss << '-';
        oss << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return oss.str();
}

int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

int64_t now_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string iso8601_time(int64_t epoch_seconds) {
    std::time_t t = static_cast<std::time_t>(epoch_seconds);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

std::string iso8601_now() {
    return iso8601_time(now_seconds());
}

bool verify_api_key(const std::string& expected_key, const std::string& provided_key) {
    if (provided_key.empty() || expected_key.empty()) return false;
    return secure_equal(expected_key, provided_key);
}

void to_json(nlohmann::json& j, const BatchCommandResult& res) {
    j = nlohmann::json{
        {"command", res.command},
        {"exitCode", res.exit_code},
        {"status", res.status},
        {"output", res.output}
    };
    if (res.duration_ms) j["durationMs"] = *res.duration_ms;
}

void from_json(const nlohmann::json& j, BatchCommandResult& res) {
    res.command = j.value("command", "");
    res.exit_code = j.value("exitCode", 0);
    res.status = j.value("status", "success");
    res.output = j.value("output", "");
    if (j.contains("durationMs") && !j["durationMs"].is_null()) res.duration_ms = j["durationMs"].get<double>();
}

void to_json(nlohmann::json& j, const BatchExecuteResult& res) {
    j = nlohmann::json{
        {"ok", res.ok},
        {"sessionId", res.session_id},
        {"summary", res.summary},
        {"results", res.results}
    };
}

void from_json(const nlohmann::json& j, BatchExecuteResult& res) {
    res.ok = j.value("ok", true);
    res.session_id = j.value("sessionId", "");
    if (j.contains("summary")) res.summary = j["summary"].get<FsBatchSummary>();
    if (j.contains("results")) res.results = j["results"].get<std::vector<BatchCommandResult>>();
}

void to_json(nlohmann::json& j, const FsBatchResult& res) {
    j = nlohmann::json{
        {"ok", res.ok},
        {"total", res.total},
        {"passed", res.passed},
        {"failed", res.failed},
        {"skipped", res.skipped},
        {"results", res.results}
    };
}

void from_json(const nlohmann::json& j, FsBatchResult& res) {
    res.ok = j.value("ok", true);
    res.total = j.value("total", 0);
    res.passed = j.value("passed", 0);
    res.failed = j.value("failed", 0);
    res.skipped = j.value("skipped", 0);
    if (j.contains("results")) res.results = j["results"].get<std::vector<FsBatchResultItem>>();
}

std::string format_batch_markdown(const BatchExecuteResult& result) {
    const auto& s = result.summary;
    std::ostringstream oss;
    oss << "### Batch Execution: " << s.passed << "/" << s.total << " Succeeded";
    if (s.failed > 0) oss << " (" << s.failed << " Failed)";
    if (s.skipped > 0) oss << ", " << s.skipped << " Skipped";
    oss << "\n\n";

    for (size_t i = 0; i < result.results.size(); ++i) {
        const auto& r = result.results[i];
        std::string badge = "Succeeded";
        if (r.status == "failed") badge = "Failed (Exit Code: " + std::to_string(r.exit_code) + ")";
        else if (r.status == "skipped") badge = "Skipped";

        std::string dur = r.duration_ms ? (" [" + std::to_string(static_cast<int>(*r.duration_ms)) + "ms]") : "";
        oss << "#### [" << (i + 1) << "/" << result.results.size() << "] `" << r.command << "` — " << badge << dur << "\n";

        if (r.status == "skipped") {
            oss << "*(Command skipped due to previous failure)*\n\n";
        } else if (!r.output.empty()) {
            oss << "```text\n" << r.output << "\n```\n\n";
        } else {
            oss << "*(Command completed with no output)*\n\n";
        }
    }

    return oss.str();
}

std::string format_batch_json(const BatchExecuteResult& result) {
    nlohmann::json j = result;
    return j.dump(2);
}

std::string format_fs_batch_markdown(const FsBatchResult& result) {
    std::ostringstream oss;
    oss << "### Batch Operations: " << result.passed << "/" << result.total << " Succeeded";
    if (result.failed > 0) oss << " (" << result.failed << " Failed)";
    if (result.skipped > 0) oss << ", " << result.skipped << " Skipped";
    oss << "\n\n";

    for (size_t i = 0; i < result.results.size(); ++i) {
        const auto& r = result.results[i];
        bool is_skipped = (!r.success && r.error.value_or("") == "Skipped due to previous operation failure");
        std::string badge = "Succeeded";
        if (is_skipped) badge = "Skipped";
        else if (!r.success) badge = "Failed: " + r.error.value_or("Unknown error");

        std::string dur = r.duration_ms ? (" [" + std::to_string(static_cast<int>(*r.duration_ms)) + "ms]") : "";
        std::string label = (r.type == "command") ? ("Command `" + r.command.value_or("") + "`") : (r.type + " `" + r.path.value_or("") + "`");

        oss << "#### [" << (i + 1) << "/" << result.results.size() << "] " << label << " — " << badge << dur << "\n";

        if (is_skipped) {
            oss << "*(Operation skipped due to previous failure)*\n\n";
        } else if (!r.success) {
            oss << "Error: " << r.error.value_or("Operation failed") << "\n\n";
        } else if (r.type == "command") {
            std::string out;
            if (r.data.contains("output")) out = r.data["output"].get<std::string>();
            oss << (out.empty() ? "*(Command completed with no output)*" : ("```text\n" + out + "\n```")) << "\n\n";
        } else if (r.type == "write") {
            uint64_t bytes = r.data.value("bytesWritten", 0ULL);
            std::string sha = r.data.value("sha256", "N/A");
            if (sha.size() > 12) sha = sha.substr(0, 12) + "...";
            oss << "*Wrote " << bytes << " bytes (SHA-256: " << sha << ")*\n\n";
        } else if (r.type == "read") {
            std::string content = r.data.value("content", "");
            uint64_t bytes = r.data.value("bytesRead", 0ULL);
            oss << (!content.empty() ? ("```text\n" + content + "\n```") : ("*(Read " + std::to_string(bytes) + " bytes)*")) << "\n\n";
        } else {
            oss << "*(Completed successfully)*\n\n";
        }
    }

    return oss.str();
}

std::string format_fs_batch_json(const FsBatchResult& result) {
    nlohmann::json j = result;
    return j.dump(2);
}

} // namespace machinebridge
