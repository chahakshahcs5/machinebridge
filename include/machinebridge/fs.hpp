#pragma once

#include "machinebridge/protocol.hpp"
#include "machinebridge/shared.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace machinebridge {

struct FileEntry {
    std::string name;
    std::string path;
    bool is_directory = false;
    uint64_t size = 0;
    std::string mtime;
};

void to_json(nlohmann::json& j, const FileEntry& e);

struct ReadFileOptions {
    std::optional<int64_t> offset;
    std::optional<int64_t> length;
    std::string encoding = "utf8";
};

struct WriteFileOptions {
    bool append = false;
    std::string encoding = "utf8";
};

struct ListFilesOptions {
    bool recursive = false;
};

struct ReadFileResult {
    std::string path;
    std::string content;
    uint64_t size = 0;
    uint64_t bytes_read = 0;
    bool has_more = false;
    std::string encoding = "utf8";
};

void to_json(nlohmann::json& j, const ReadFileResult& r);

struct WriteFileResult {
    std::string path;
    uint64_t bytes_written = 0;
    std::string sha256;
};

void to_json(nlohmann::json& j, const WriteFileResult& r);

struct StatResult {
    std::string path;
    bool is_directory = false;
    bool is_file = false;
    uint64_t size = 0;
    std::string mtime;
    std::string birthtime;
};

void to_json(nlohmann::json& j, const StatResult& s);

using CommandExecutorFn = std::function<nlohmann::json(
    const std::string& command,
    const std::optional<std::string>& cwd,
    std::optional<int64_t> timeout_ms
)>;

class FilesystemManager {
public:
    explicit FilesystemManager(std::optional<std::string> workspace_root = std::nullopt);

    std::filesystem::path resolve_path(const std::string& user_path) const;

    ReadFileResult read_file(const std::string& user_path, const ReadFileOptions& options = {}) const;
    WriteFileResult write_file(const std::string& user_path, const std::string& content, const WriteFileOptions& options = {}) const;
    void delete_file(const std::string& user_path, bool recursive = true) const;
    void make_directory(const std::string& user_path, bool recursive = true) const;
    void move_file(const std::string& source, const std::string& destination) const;
    void copy_file(const std::string& source, const std::string& destination) const;
    std::vector<FileEntry> list_files(const std::string& user_path, const ListFilesOptions& options = {}) const;
    StatResult stat_file(const std::string& user_path) const;

    nlohmann::json execute_action(FsAction action, const nlohmann::json& params) const;

    FsBatchResult execute_batch(
        const std::vector<FsOperation>& operations,
        bool stop_on_error = true,
        CommandExecutorFn command_fn = nullptr
    ) const;

    const std::optional<std::filesystem::path>& workspace_root() const { return m_workspace_root; }

private:
    std::optional<std::filesystem::path> m_workspace_root;
};

} // namespace machinebridge
