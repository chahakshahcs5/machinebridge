#pragma once

#include "machinebridge/executor.hpp"
#include "machinebridge/fs.hpp"
#include "machinebridge/pty.hpp"
#include "machinebridge/session_store.hpp"
#include <functional>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace machinebridge {

struct ToolExecutionResult {
    nlohmann::json content; // Array of { "type": "text", "text": "..." }
    bool is_error = false;
    std::optional<nlohmann::json> meta;
};

void to_json(nlohmann::json& j, const ToolExecutionResult& res);

struct McpContext {
    std::shared_ptr<CommandExecutor> executor;
    std::shared_ptr<FilesystemManager> fs_manager;
    std::shared_ptr<InMemorySessionStore> sessions;
    std::shared_ptr<PtyManager> pty_manager;
    std::string base_url = "http://localhost:8080";
    std::string api_key;
    std::function<bool(const std::string& session_id)> is_session_authenticated;
};

// Returns the full list of 15 verified MCP tools and their schemas
nlohmann::json get_mcp_tools();

// Executes any of the 15 MCP tools by name
ToolExecutionResult execute_tool(
    const std::string& name,
    const nlohmann::json& args,
    const McpContext& ctx
);

// Computes JSON-RPC 2.0 response for an MCP method call (initialize, server/discover, ping, tools/list, tools/call)
std::optional<nlohmann::json> compute_jsonrpc_response(
    const nlohmann::json& payload,
    const McpContext& ctx,
    bool is_authenticated
);

// Runs the interactive MCP stdio loop reading JSON-RPC from stdin and writing to stdout
int run_mcp_stdio(const McpContext& ctx);

} // namespace machinebridge
