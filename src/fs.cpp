#include "machinebridge/fs.hpp"
#include "machinebridge/crypto.hpp"
#include "machinebridge/shared.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace machinebridge {

namespace {

static const std::regex RESERVED_WIN_NAMES(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\..*)?$)", std::regex_constants::icase);

std::string format_fs_time(std::filesystem::file_time_type ftime) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
    );
    return iso8601_time(std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count());
}

} // namespace

void to_json(nlohmann::json& j, const FileEntry& e) {
    j = nlohmann::json{
        {"name", e.name},
        {"path", e.path},
        {"isDirectory", e.is_directory},
        {"size", e.size},
        {"mtime", e.mtime}
    };
}

void to_json(nlohmann::json& j, const ReadFileResult& r) {
    j = nlohmann::json{
        {"path", r.path},
        {"content", r.content},
        {"size", r.size},
        {"bytesRead", r.bytes_read},
        {"hasMore", r.has_more},
        {"encoding", r.encoding}
    };
}

void to_json(nlohmann::json& j, const WriteFileResult& r) {
    j = nlohmann::json{
        {"path", r.path},
        {"bytesWritten", r.bytes_written},
        {"sha256", r.sha256}
    };
}

void to_json(nlohmann::json& j, const StatResult& s) {
    j = nlohmann::json{
        {"path", s.path},
        {"isDirectory", s.is_directory},
        {"isFile", s.is_file},
        {"size", s.size},
        {"mtime", s.mtime},
        {"birthtime", s.birthtime}
    };
}

FilesystemManager::FilesystemManager(std::optional<std::string> workspace_root) {
    if (workspace_root && !workspace_root->empty()) {
        m_workspace_root = std::filesystem::weakly_canonical(*workspace_root);
    }
}

std::filesystem::path FilesystemManager::resolve_path(const std::string& user_path) const {
    if (user_path.empty()) {
        throw std::runtime_error("INVALID_PATH: Path must be a non-empty string");
    }

    if (user_path.find('\0') != std::string::npos) {
        throw std::runtime_error("PATH_SECURITY_VIOLATION: Path contains null bytes");
    }

    // Check Windows reserved names
    std::string norm = user_path;
    std::replace(norm.begin(), norm.end(), '/', '\\');
    std::istringstream stream(norm);
    std::string seg;
    while (std::getline(stream, seg, '\\')) {
        if (seg.empty()) continue;
        if (std::regex_match(seg, RESERVED_WIN_NAMES)) {
            throw std::runtime_error("PATH_SECURITY_VIOLATION: Path uses reserved Windows device name (" + seg + ")");
        }
    }

    std::filesystem::path base = m_workspace_root ? *m_workspace_root : std::filesystem::current_path();
    std::filesystem::path resolved = std::filesystem::weakly_canonical(base / user_path);

    if (m_workspace_root) {
        std::string root_s = m_workspace_root->string();
        std::string res_s = resolved.string();

#if defined(_WIN32)
        std::string root_lower = root_s;
        std::string res_lower = res_s;
        std::transform(root_lower.begin(), root_lower.end(), root_lower.begin(), ::tolower);
        std::transform(res_lower.begin(), res_lower.end(), res_lower.begin(), ::tolower);
        if (res_lower != root_lower && res_lower.rfind(root_lower + "\\", 0) != 0 && res_lower.rfind(root_lower + "/", 0) != 0) {
            throw std::runtime_error("PATH_TRAVERSAL_DETECTED: Access outside authorized workspace denied (" + user_path + ")");
        }
#else
        if (res_s != root_s && res_s.rfind(root_s + "/", 0) != 0) {
            throw std::runtime_error("PATH_TRAVERSAL_DETECTED: Access outside authorized workspace denied (" + user_path + ")");
        }
#endif
    }

    return resolved;
}

ReadFileResult FilesystemManager::read_file(const std::string& user_path, const ReadFileOptions& options) const {
    auto path = resolve_path(user_path);
    if (!std::filesystem::exists(path) || std::filesystem::is_directory(path)) {
        throw std::runtime_error("FILE_NOT_FOUND: File does not exist or is a directory (" + user_path + ")");
    }

    uint64_t file_size = std::filesystem::file_size(path);
    int64_t offset = options.offset.value_or(0);
    if (offset < 0) offset = 0;
    int64_t max_bytes = options.length.value_or(1024 * 1024); // 1 MiB default

    if (static_cast<uint64_t>(offset) >= file_size) {
        return ReadFileResult{user_path, "", file_size, 0, false, options.encoding};
    }

    uint64_t remaining = file_size - offset;
    uint64_t bytes_to_read = (std::min)(remaining, static_cast<uint64_t>(max_bytes));

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("CANNOT_OPEN_FILE: Failed to open file for reading (" + user_path + ")");
    }

    stream.seekg(offset);
    std::vector<uint8_t> buffer(bytes_to_read);
    stream.read(reinterpret_cast<char*>(buffer.data()), bytes_to_read);
    size_t actual_read = static_cast<size_t>(stream.gcount());
    buffer.resize(actual_read);

    bool has_more = (offset + actual_read) < file_size;
    std::string content;
    if (options.encoding == "base64") {
        content = base64_encode(buffer.data(), buffer.size());
    } else {
        content = std::string(buffer.begin(), buffer.end());
    }

    return ReadFileResult{user_path, content, file_size, actual_read, has_more, options.encoding};
}

WriteFileResult FilesystemManager::write_file(const std::string& user_path, const std::string& content, const WriteFileOptions& options) const {
    auto path = resolve_path(user_path);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::vector<uint8_t> raw_bytes;
    if (options.encoding == "base64") {
        raw_bytes = base64_decode(content);
    } else {
        raw_bytes.assign(content.begin(), content.end());
    }

    auto mode = std::ios::binary | (options.append ? std::ios::app : std::ios::trunc);
    std::ofstream stream(path, mode);
    if (!stream.is_open()) {
        throw std::runtime_error("CANNOT_WRITE_FILE: Failed to open file for writing (" + user_path + ")");
    }

    stream.write(reinterpret_cast<const char*>(raw_bytes.data()), raw_bytes.size());
    stream.flush();

    std::string sha = sha256_hex(std::string_view(reinterpret_cast<const char*>(raw_bytes.data()), raw_bytes.size()));
    return WriteFileResult{user_path, raw_bytes.size(), sha};
}

void FilesystemManager::delete_file(const std::string& user_path, bool recursive) const {
    auto path = resolve_path(user_path);
    if (!std::filesystem::exists(path)) return;

    if (recursive) {
        std::filesystem::remove_all(path);
    } else {
        std::filesystem::remove(path);
    }
}

void FilesystemManager::make_directory(const std::string& user_path, bool /*recursive*/) const {
    auto path = resolve_path(user_path);
    std::filesystem::create_directories(path);
}

void FilesystemManager::move_file(const std::string& source, const std::string& destination) const {
    auto src_path = resolve_path(source);
    auto dest_path = resolve_path(destination);
    if (dest_path.has_parent_path()) {
        std::filesystem::create_directories(dest_path.parent_path());
    }
    std::filesystem::rename(src_path, dest_path);
}

void FilesystemManager::copy_file(const std::string& source, const std::string& destination) const {
    auto src_path = resolve_path(source);
    auto dest_path = resolve_path(destination);
    if (dest_path.has_parent_path()) {
        std::filesystem::create_directories(dest_path.parent_path());
    }
    std::filesystem::copy(src_path, dest_path, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
}

std::vector<FileEntry> FilesystemManager::list_files(const std::string& user_path, const ListFilesOptions& options) const {
    auto path = resolve_path(user_path);
    std::vector<FileEntry> entries;

    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("FILE_NOT_FOUND: Directory or file not found (" + user_path + ")");
    }

    if (!std::filesystem::is_directory(path)) {
        entries.push_back(FileEntry{
            path.filename().string(),
            path.filename().string(),
            false,
            std::filesystem::file_size(path),
            format_fs_time(std::filesystem::last_write_time(path))
        });
        return entries;
    }

    auto walk = [&](auto& self, const std::filesystem::path& dir, const std::string& rel_base) -> void {
        for (const auto& item : std::filesystem::directory_iterator(dir)) {
            std::string name = item.path().filename().string();
            std::string item_rel = rel_base.empty() ? name : (rel_base + "/" + name);
            bool is_dir = item.is_directory();
            uint64_t sz = 0;
            std::string mtime = iso8601_now();

            try {
                if (!is_dir) sz = item.file_size();
                mtime = format_fs_time(item.last_write_time());
            } catch (...) {}

            entries.push_back(FileEntry{name, item_rel, is_dir, sz, mtime});
            if (options.recursive && is_dir) {
                self(self, item.path(), item_rel);
            }
        }
    };

    walk(walk, path, "");
    return entries;
}

StatResult FilesystemManager::stat_file(const std::string& user_path) const {
    auto path = resolve_path(user_path);
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("FILE_NOT_FOUND: File not found (" + user_path + ")");
    }

    bool is_dir = std::filesystem::is_directory(path);
    uint64_t sz = is_dir ? 0 : std::filesystem::file_size(path);
    std::string mtime = format_fs_time(std::filesystem::last_write_time(path));

    return StatResult{
        user_path,
        is_dir,
        !is_dir,
        sz,
        mtime,
        mtime // birthtime fallback
    };
}

nlohmann::json FilesystemManager::execute_action(FsAction action, const nlohmann::json& params) const {
    std::string p = params.value("path", "");
    switch (action) {
        case FsAction::Read: {
            ReadFileOptions opt;
            if (params.contains("offset")) opt.offset = params["offset"].get<int64_t>();
            if (params.contains("length")) opt.length = params["length"].get<int64_t>();
            if (params.contains("encoding")) opt.encoding = params["encoding"].get<std::string>();
            return read_file(p, opt);
        }
        case FsAction::Write: {
            WriteFileOptions opt;
            if (params.contains("append")) opt.append = params["append"].get<bool>();
            if (params.contains("encoding")) opt.encoding = params["encoding"].get<std::string>();
            std::string c = params.value("content", "");
            return write_file(p, c, opt);
        }
        case FsAction::Delete: {
            bool rec = params.value("recursive", true);
            delete_file(p, rec);
            return nlohmann::json{{"path", p}, {"deleted", true}};
        }
        case FsAction::Mkdir: {
            make_directory(p, true);
            return nlohmann::json{{"path", p}, {"created", true}};
        }
        case FsAction::Move: {
            std::string dest = params.value("destination", "");
            move_file(p, dest);
            return nlohmann::json{{"source", p}, {"destination", dest}, {"moved", true}};
        }
        case FsAction::Copy: {
            std::string dest = params.value("destination", "");
            copy_file(p, dest);
            return nlohmann::json{{"source", p}, {"destination", dest}, {"copied", true}};
        }
        case FsAction::List: {
            ListFilesOptions opt;
            if (params.contains("recursive")) opt.recursive = params["recursive"].get<bool>();
            return nlohmann::json{{"path", p}, {"entries", list_files(p, opt)}};
        }
        case FsAction::Stat: {
            return stat_file(p);
        }
    }
    throw std::runtime_error("UNKNOWN_FS_ACTION");
}

FsBatchResult FilesystemManager::execute_batch(
    const std::vector<FsOperation>& operations,
    bool stop_on_error,
    CommandExecutorFn command_fn
) const {
    FsBatchResult result;
    result.total = static_cast<int>(operations.size());
    bool has_failed = false;

    for (size_t i = 0; i < operations.size(); ++i) {
        const auto& op = operations[i];
        FsBatchResultItem item;
        item.index = static_cast<int>(i);
        item.type = op.type;
        item.path = op.path;
        item.command = op.command;

        if (has_failed && stop_on_error) {
            item.success = false;
            item.error = "Skipped due to previous operation failure";
            result.results.push_back(item);
            result.skipped++;
            continue;
        }

        auto start = std::chrono::steady_clock::now();
        try {
            if (op.type == "write") {
                WriteFileOptions opt;
                opt.append = op.append.value_or(false);
                opt.encoding = op.encoding.value_or("utf8");
                item.data = this->write_file(op.path.value_or(""), op.content.value_or(""), opt);
                item.success = true;
            } else if (op.type == "read") {
                ReadFileOptions opt;
                opt.offset = op.offset;
                opt.length = op.length;
                opt.encoding = op.encoding.value_or("utf8");
                item.data = this->read_file(op.path.value_or(""), opt);
                item.success = true;
            } else if (op.type == "mkdir") {
                this->make_directory(op.path.value_or(""), op.recursive.value_or(true));
                item.data = nlohmann::json{{"path", op.path.value_or("")}, {"created", true}};
                item.success = true;
            } else if (op.type == "delete") {
                this->delete_file(op.path.value_or(""), op.recursive.value_or(true));
                item.data = nlohmann::json{{"path", op.path.value_or("")}, {"deleted", true}};
                item.success = true;
            } else if (op.type == "move") {
                this->move_file(op.path.value_or(""), op.destination.value_or(""));
                item.data = nlohmann::json{{"source", op.path.value_or("")}, {"destination", op.destination.value_or("")}, {"moved", true}};
                item.success = true;
            } else if (op.type == "copy") {
                this->copy_file(op.path.value_or(""), op.destination.value_or(""));
                item.data = nlohmann::json{{"source", op.path.value_or("")}, {"destination", op.destination.value_or("")}, {"copied", true}};
                item.success = true;
            } else if (op.type == "list") {
                ListFilesOptions opt;
                opt.recursive = op.recursive.value_or(false);
                item.data = nlohmann::json{{"path", op.path.value_or("")}, {"entries", this->list_files(op.path.value_or(""), opt)}};
                item.success = true;
            } else if (op.type == "command") {
                if (!command_fn) {
                    throw std::runtime_error("Command execution is not configured for this filesystem session");
                }
                auto cmd_res = command_fn(op.command.value_or(""), op.cwd, op.timeout_ms);
                item.data = cmd_res;
                int ec = cmd_res.value("exitCode", 0);
                if (ec != 0) {
                    throw std::runtime_error("Command failed with exit code " + std::to_string(ec) + ": " + cmd_res.value("output", ""));
                }
                item.success = true;
            } else {
                throw std::runtime_error("UNKNOWN_OPERATION_TYPE: " + op.type);
            }
        } catch (const std::exception& ex) {
            has_failed = true;
            item.success = false;
            item.error = ex.what();
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        item.duration_ms = static_cast<double>(elapsed.count());
        result.results.push_back(item);

        if (item.success) {
            result.passed++;
        } else {
            result.failed++;
        }
    }

    result.ok = (result.failed == 0);
    return result;
}

} // namespace machinebridge
