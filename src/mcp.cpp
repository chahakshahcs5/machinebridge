#include "machinebridge/mcp.hpp"
#include "machinebridge/crypto.hpp"
#include "machinebridge/shared.hpp"
#include <iostream>
#include <sstream>

namespace machinebridge {

void to_json(nlohmann::json& j, const ToolExecutionResult& res) {
    j = nlohmann::json{
        {"content", res.content},
        {"isError", res.is_error}
    };
    if (res.meta) {
        j["_meta"] = *res.meta;
    }
}

nlohmann::json get_mcp_tools() {
    return nlohmann::json::array({
        {
            {"name", "execute_command"},
            {"description", "Execute a single command on the machine using a persistent or ephemeral terminal with timeout control."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"command", {{"type", "string"}, {"description", "The shell command to execute."}}},
                    {"cwd", {{"type", "string"}, {"description", "Optional working directory to run the command in."}}},
                    {"timeoutMs", {{"type", "number"}, {"description", "Timeout in milliseconds before terminating (default: 15000)."}}},
                    {"shell", {{"type", "string"}, {"description", "Optional shell executable to use (e.g. powershell.exe, cmd.exe, /bin/bash)."}}},
                    {"sessionId", {{"type", "string"}, {"description", "Optional persistent terminal session ID to execute the command within."}}},
                    {"idleTimeoutMs", {{"type", "number"}, {"description", "Optional inactivity timeout in milliseconds (default: 5000 for session mode)."}}}
                }},
                {"required", nlohmann::json::array({"command"})}
            }}
        },
        {
            {"name", "execute_commands"},
            {"description", "Execute a batch sequence of shell commands sequentially in a persistent terminal session with per-command status, exit code, and timing."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"commands", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Array of shell commands to execute sequentially."}}},
                    {"cwd", {{"type", "string"}, {"description", "Optional working directory for the batch execution."}}},
                    {"timeoutMs", {{"type", "number"}, {"description", "Total timeout in milliseconds for the entire batch (default: 30000)."}}},
                    {"stopOnError", {{"type", "boolean"}, {"description", "Whether to stop immediately when a command fails (default: true)."}}},
                    {"shell", {{"type", "string"}, {"description", "Optional shell executable to run the commands in."}}},
                    {"sessionId", {{"type", "string"}, {"description", "Optional active terminal session ID to execute the commands within."}}}
                }},
                {"required", nlohmann::json::array({"commands"})}
            }}
        },
        {
            {"name", "read_file"},
            {"description", "Read the content of a file on the machine with optional offset/chunking and encoding support."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Path of the file to read (absolute or relative to current directory)."}}},
                    {"offset", {{"type", "number"}, {"description", "Optional byte offset to start reading from (default: 0)."}}},
                    {"length", {{"type", "number"}, {"description", "Optional maximum bytes to read (default: 1048576 = 1 MiB)."}}},
                    {"encoding", {{"type", "string"}, {"enum", nlohmann::json::array({"utf8", "base64"})}, {"description", "Encoding of returned content (default: utf8)."}}}
                }},
                {"required", nlohmann::json::array({"path"})}
            }}
        },
        {
            {"name", "write_file"},
            {"description", "Write, create, or append content to a file on the machine."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Target path to write to."}}},
                    {"content", {{"type", "string"}, {"description", "Content to write into the file."}}},
                    {"append", {{"type", "boolean"}, {"description", "If true, appends content to the existing file."}}},
                    {"encoding", {{"type", "string"}, {"enum", nlohmann::json::array({"utf8", "base64"})}, {"description", "Encoding of the content (default: utf8)."}}}
                }},
                {"required", nlohmann::json::array({"path", "content"})}
            }}
        },
        {
            {"name", "list_directory"},
            {"description", "List files and directories at the given path with metadata (size, isDirectory, mtime)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Directory path to list (default: current working directory)."}}},
                    {"recursive", {{"type", "boolean"}, {"description", "If true, recursively lists files in subdirectories."}}}
                }}
            }}
        },
        {
            {"name", "delete_file"},
            {"description", "Delete a file or directory recursively."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Path to delete."}}},
                    {"recursive", {{"type", "boolean"}, {"description", "If true, deletes directories recursively (default: true)."}}}
                }},
                {"required", nlohmann::json::array({"path"})}
            }}
        },
        {
            {"name", "make_directory"},
            {"description", "Create a directory (including parent directories if needed)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Directory path to create."}}}
                }},
                {"required", nlohmann::json::array({"path"})}
            }}
        },
        {
            {"name", "move_file"},
            {"description", "Move or rename a file or directory on the machine."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"source", {{"type", "string"}, {"description", "Path of the file or directory to move."}}},
                    {"destination", {{"type", "string"}, {"description", "Destination path."}}}
                }},
                {"required", nlohmann::json::array({"source", "destination"})}
            }}
        },
        {
            {"name", "copy_file"},
            {"description", "Copy a file or directory on the machine."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"source", {{"type", "string"}, {"description", "Path of the file or directory to copy."}}},
                    {"destination", {{"type", "string"}, {"description", "Destination path."}}}
                }},
                {"required", nlohmann::json::array({"source", "destination"})}
            }}
        },
        {
            {"name", "stat_file"},
            {"description", "Get metadata about a file or directory (size, mtime, isDirectory, isFile)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Path to check."}}}
                }},
                {"required", nlohmann::json::array({"path"})}
            }}
        },
        {
            {"name", "batch_fs"},
            {"description", "Execute a batch sequence of filesystem operations atomically or sequentially."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"operations", {
                        {"type", "array"},
                        {"description", "List of filesystem operations (read, write, mkdir, delete, move, copy, list)."},
                        {"items", {
                            {"type", "object"},
                            {"properties", {
                                {"type", {{"type", "string"}, {"enum", nlohmann::json::array({"write", "read", "mkdir", "delete", "move", "copy", "list"})}}},
                                {"path", {{"type", "string"}}},
                                {"content", {{"type", "string"}}},
                                {"destination", {{"type", "string"}}},
                                {"recursive", {{"type", "boolean"}}},
                                {"offset", {{"type", "number"}}},
                                {"length", {{"type", "number"}}}
                            }},
                            {"required", nlohmann::json::array({"type"})}
                        }}
                    }},
                    {"stopOnError", {{"type", "boolean"}, {"description", "Stop executing remaining operations if one fails (default: true)."}}}
                }},
                {"required", nlohmann::json::array({"operations"})}
            }}
        },
        {
            {"name", "create_session"},
            {"description", "Create a new interactive terminal session ID for streaming or multi-step interaction."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"ttlSeconds", {{"type", "number"}, {"description", "Session lifetime in seconds (default: 3600)."}}},
                    {"shell", {{"type", "string"}, {"description", "Optional shell to use."}}},
                    {"cols", {{"type", "number"}, {"description", "Columns (default: 80)."}}},
                    {"rows", {{"type", "number"}, {"description", "Rows (default: 24)."}}},
                    {"cwd", {{"type", "string"}, {"description", "Working directory."}}}
                }}
            }}
        },
        {
            {"name", "send_input"},
            {"description", "Send input text or signals to an active interactive terminal session."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"sessionId", {{"type", "string"}, {"description", "The active session ID."}}},
                    {"data", {{"type", "string"}, {"description", "Data string to send to stdin."}}},
                    {"signal", {{"type", "string"}, {"description", "Signal name (SIGINT, SIGTERM, etc.)."}}}
                }},
                {"required", nlohmann::json::array({"sessionId"})}
            }}
        },
        {
            {"name", "read_output"},
            {"description", "Read the latest buffered output from an active interactive terminal session."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"sessionId", {{"type", "string"}, {"description", "The active session ID."}}},
                    {"raw", {{"type", "boolean"}, {"description", "Whether to return raw uncleaned output including ANSI escape sequences (default: false)."}}}
                }},
                {"required", nlohmann::json::array({"sessionId"})}
            }}
        },
        {
            {"name", "close_session"},
            {"description", "Close and terminate an active terminal session."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"sessionId", {{"type", "string"}, {"description", "The active session ID to close."}}}
                }},
                {"required", nlohmann::json::array({"sessionId"})}
            }}
        }
    });
}

ToolExecutionResult execute_tool(
    const std::string& name,
    const nlohmann::json& args,
    const McpContext& ctx
) {
    try {
        if (name == "execute_command") {
            std::string cmd = args.value("command", "");
            if (cmd.empty()) {
                return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'command' parameter is required"}}}), true};
            }
            std::optional<std::string> cwd = args.contains("cwd") ? std::make_optional(args["cwd"].get<std::string>()) : std::nullopt;
            int64_t timeout_ms = args.value("timeoutMs", 15000LL);
            std::optional<std::string> shell = args.contains("shell") ? std::make_optional(args["shell"].get<std::string>()) : std::nullopt;
            std::optional<std::string> session_id = args.contains("sessionId") ? std::make_optional(args["sessionId"].get<std::string>()) : std::nullopt;
            std::optional<int64_t> idle_timeout = args.contains("idleTimeoutMs") ? std::make_optional(args["idleTimeoutMs"].get<int64_t>()) : std::nullopt;

            auto res = ctx.executor->execute_command(cmd, cwd, timeout_ms, shell, session_id, idle_timeout);
            nlohmann::json data = {
                {"sessionId", res.session_id},
                {"output", res.output},
                {"exitCode", res.exit_code}
            };
            return {nlohmann::json::array({{{"type", "text"}, {"text", data.dump(2)}}}), res.exit_code != 0};
        }

        if (name == "execute_commands") {
            if (!args.contains("commands") || !args["commands"].is_array() || args["commands"].empty()) {
                return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'commands' parameter must be a non-empty array"}}}), true};
            }
            std::vector<std::string> cmds = args["commands"].get<std::vector<std::string>>();
            std::optional<std::string> cwd = args.contains("cwd") ? std::make_optional(args["cwd"].get<std::string>()) : std::nullopt;
            int64_t timeout_ms = args.value("timeoutMs", 30000LL);
            bool stop_on_err = args.value("stopOnError", true);
            std::optional<std::string> shell = args.contains("shell") ? std::make_optional(args["shell"].get<std::string>()) : std::nullopt;
            std::optional<std::string> session_id = args.contains("sessionId") ? std::make_optional(args["sessionId"].get<std::string>()) : std::nullopt;

            auto res = ctx.executor->execute_commands(cmds, cwd, timeout_ms, stop_on_err, shell, session_id);
            std::string formatted = format_batch_markdown(res);
            return {nlohmann::json::array({{{"type", "text"}, {"text", formatted}}}), !res.ok};
        }

        if (name == "read_file") {
            std::string p = args.value("path", "");
            if (p.empty()) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'path' parameter is required"}}}), true};
            ReadFileOptions opt;
            if (args.contains("offset")) opt.offset = args["offset"].get<int64_t>();
            if (args.contains("length")) opt.length = args["length"].get<int64_t>();
            if (args.contains("encoding")) opt.encoding = args["encoding"].get<std::string>();

            auto res = ctx.fs_manager->read_file(p, opt);
            nlohmann::json j = res;
            return {nlohmann::json::array({{{"type", "text"}, {"text", j.dump(2)}}}), false};
        }

        if (name == "write_file") {
            std::string p = args.value("path", "");
            if (p.empty()) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'path' parameter is required"}}}), true};
            std::string content = args.value("content", "");
            WriteFileOptions opt;
            opt.append = args.value("append", false);
            if (args.contains("encoding")) opt.encoding = args["encoding"].get<std::string>();

            auto res = ctx.fs_manager->write_file(p, content, opt);
            nlohmann::json j = res;
            return {nlohmann::json::array({{{"type", "text"}, {"text", j.dump(2)}}}), false};
        }

        if (name == "list_directory") {
            std::string p = args.value("path", ".");
            ListFilesOptions opt;
            opt.recursive = args.value("recursive", false);
            auto entries = ctx.fs_manager->list_files(p, opt);
            nlohmann::json j = {{"path", p}, {"entries", entries}};
            return {nlohmann::json::array({{{"type", "text"}, {"text", j.dump(2)}}}), false};
        }

        if (name == "delete_file") {
            std::string p = args.value("path", "");
            if (p.empty()) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'path' parameter is required"}}}), true};
            bool rec = args.value("recursive", true);
            ctx.fs_manager->delete_file(p, rec);
            return {nlohmann::json::array({{{"type", "text"}, {"text", "Deleted " + p}}}), false};
        }

        if (name == "make_directory") {
            std::string p = args.value("path", "");
            if (p.empty()) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'path' parameter is required"}}}), true};
            ctx.fs_manager->make_directory(p, true);
            return {nlohmann::json::array({{{"type", "text"}, {"text", "Created directory " + p}}}), false};
        }

        if (name == "move_file") {
            std::string src = args.value("source", "");
            std::string dst = args.value("destination", "");
            if (src.empty() || dst.empty()) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'source' and 'destination' parameters are required"}}}), true};
            ctx.fs_manager->move_file(src, dst);
            return {nlohmann::json::array({{{"type", "text"}, {"text", "Moved " + src + " to " + dst}}}), false};
        }

        if (name == "copy_file") {
            std::string src = args.value("source", "");
            std::string dst = args.value("destination", "");
            if (src.empty() || dst.empty()) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'source' and 'destination' parameters are required"}}}), true};
            ctx.fs_manager->copy_file(src, dst);
            return {nlohmann::json::array({{{"type", "text"}, {"text", "Copied " + src + " to " + dst}}}), false};
        }

        if (name == "stat_file") {
            std::string p = args.value("path", "");
            if (p.empty()) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'path' parameter is required"}}}), true};
            auto st = ctx.fs_manager->stat_file(p);
            nlohmann::json j = st;
            return {nlohmann::json::array({{{"type", "text"}, {"text", j.dump(2)}}}), false};
        }

        if (name == "batch_fs") {
            if (!args.contains("operations") || !args["operations"].is_array() || args["operations"].empty()) {
                return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'operations' parameter must be a non-empty array"}}}), true};
            }
            std::vector<FsOperation> ops = args["operations"].get<std::vector<FsOperation>>();
            bool stop_on_err = args.value("stopOnError", true);
            auto res = ctx.fs_manager->execute_batch(ops, stop_on_err, [&](const std::string& cmd, const std::optional<std::string>& cwd, std::optional<int64_t> timeout_ms) {
                auto exec_res = ctx.executor->execute_command(cmd, cwd, timeout_ms.value_or(15000));
                return nlohmann::json{{"output", exec_res.output}, {"exitCode", exec_res.exit_code}};
            });
            nlohmann::json j = res;
            return {nlohmann::json::array({{{"type", "text"}, {"text", j.dump(2)}}}), !res.ok};
        }

        if (name == "create_session") {
            uint32_t ttl = args.value("ttlSeconds", 3600U);
            std::optional<std::string> shell = args.contains("shell") ? std::make_optional(args["shell"].get<std::string>()) : std::nullopt;
            int cols = args.value("cols", 80);
            int rows = args.value("rows", 24);
            std::optional<std::string> cwd = args.contains("cwd") ? std::make_optional(args["cwd"].get<std::string>()) : std::nullopt;

            std::string st = "powershell";
            if (shell) {
                auto type = detect_shell_type(shell);
                if (type == ShellType::Cmd) st = "cmd";
                else if (type == ShellType::Sh) st = "sh";
            }

            auto rec = ctx.sessions->create(ttl, shell, st);
            PtyOptions opts{rec.id, cols, rows, shell, {}, cwd};
            PtyCallbacks cbs;
            std::string sid = rec.id;
            cbs.on_data = [sessions = ctx.sessions, sid](const std::string& d) { sessions->append_output(sid, d); };
            cbs.on_exit = [sessions = ctx.sessions, sid](int, int) { sessions->set_status(sid, "closed"); };

            auto pty_s = ctx.pty_manager->create(opts, cbs);
            ctx.sessions->set_status(rec.id, "running");

            nlohmann::json j = {
                {"sessionId", rec.id},
                {"pid", pty_s->pid()},
                {"status", "running"},
                {"expiresAt", iso8601_time(std::chrono::duration_cast<std::chrono::seconds>(rec.expires_at.time_since_epoch()).count())}
            };
            return {nlohmann::json::array({{{"type", "text"}, {"text", j.dump(2)}}}), false};
        }

        if (name == "send_input") {
            std::string sid = args.value("sessionId", args.value("session_id", args.value("id", "")));
            if (sid.empty()) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'sessionId' parameter is required"}}}), true};

            auto pty_s = ctx.pty_manager->get(sid);
            if (!pty_s) {
                return {nlohmann::json::array({{{"type", "text"}, {"text", "Session " + sid + " not active or not found"}}}), true};
            }
            std::string data = args.value("data", "");
            if (!data.empty()) pty_s->write(data);

            if (args.contains("signal")) {
                std::string sig_str = args["signal"].get<std::string>();
                if (auto sig = string_to_terminal_signal(sig_str)) {
                    pty_s->signal(*sig);
                }
            }
            return {nlohmann::json::array({{{"type", "text"}, {"text", "Sent input to session " + sid}}}), false};
        }

        if (name == "read_output") {
            std::string sid = args.value("sessionId", args.value("session_id", args.value("id", "")));
            if (sid.empty()) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'sessionId' parameter is required"}}}), true};
            auto rec = ctx.sessions->get(sid);
            if (!rec) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: Session " + sid + " not found"}}}), true};
            std::string out = ctx.sessions->get_output(sid);
            bool raw = args.value("raw", false);
            if (!raw) {
                out = clean_terminal_output(out);
            }
            return {nlohmann::json::array({{{"type", "text"}, {"text", out}}}), false};
        }

        if (name == "close_session") {
            std::string sid = args.value("sessionId", args.value("session_id", args.value("id", "")));
            if (sid.empty()) return {nlohmann::json::array({{{"type", "text"}, {"text", "Error: 'sessionId' parameter is required"}}}), true};
            ctx.pty_manager->close(sid);
            ctx.sessions->set_status(sid, "closed");
            return {nlohmann::json::array({{{"type", "text"}, {"text", "Session " + sid + " closed successfully"}}}), false};
        }

        return {nlohmann::json::array({{{"type", "text"}, {"text", "Unknown tool: " + name}}}), true};
    } catch (const std::exception& ex) {
        return {nlohmann::json::array({{{"type", "text"}, {"text", std::string("Error executing ") + name + ": " + ex.what()}}}), true};
    }
}

std::optional<nlohmann::json> compute_jsonrpc_response(
    const nlohmann::json& payload,
    const McpContext& ctx,
    bool is_authenticated
) {
    if (!payload.contains("method")) return std::nullopt;

    std::string method = payload["method"].get<std::string>();
    nlohmann::json id = payload.contains("id") ? payload["id"] : nlohmann::json();
    nlohmann::json params = payload.contains("params") ? payload["params"] : nlohmann::json::object();

    if (method == "server/discover") {
        return nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", {
                {"supportedVersions", nlohmann::json::array({"2026-07-28", "2025-11-25", "2024-11-05"})},
                {"capabilities", {{"tools", {{"listChanged", false}}}}},
                {"serverInfo", {{"name", "machinebridge-mcp"}, {"version", "1.0.0"}}},
                {"_meta", {{"io.modelcontextprotocol/serverInfo", {{"name", "machinebridge-mcp"}, {"version", "1.0.0"}}}}},
                {"instructions", "MachineBridge secure remote PTY system."}
            }}
        };
    }

    if (method == "initialize") {
        std::string proto = params.value("protocolVersion", "2026-07-28");
        return nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", {
                {"protocolVersion", proto},
                {"capabilities", {{"tools", nlohmann::json::object()}}},
                {"serverInfo", {{"name", "machinebridge-mcp"}, {"version", "1.0.0"}}}
            }}
        };
    }

    if (id.is_null()) {
        // Notification
        return std::nullopt;
    }

    if (method == "ping") {
        return nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", nlohmann::json::object()}
        };
    }

    if (method == "tools/list") {
        auto tools = get_mcp_tools();
        auto schemes = nlohmann::json::array({
            nlohmann::json{
                {"type", "oauth2"},
                {"scopes", nlohmann::json::array({"mcp"})}
            }
        });
        for (auto& t : tools) {
            t["securitySchemes"] = schemes;
            t["_meta"] = nlohmann::json{{"securitySchemes", schemes}};
        }
        return nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", {{"tools", tools}}}
        };
    }

    if (method == "tools/call") {
        if (!is_authenticated) {
            return nlohmann::json{
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result", {
                    {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Authentication required: please log in to authorize MachineBridge tools."}}})},
                    {"_meta", {
                        {"mcp/www_authenticate", nlohmann::json::array({
                            "Bearer resource_metadata=\"" + ctx.base_url + "/.well-known/oauth-protected-resource\", error=\"insufficient_scope\", error_description=\"Authentication required\""
                        })}
                    }},
                    {"isError", true}
                }}
            };
        }

        std::string tool_name = params.value("name", "");
        nlohmann::json tool_args = params.value("arguments", nlohmann::json::object());
        auto res = execute_tool(tool_name, tool_args, ctx);

        nlohmann::json res_json = res;
        return nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", res_json}
        };
    }

    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {
            {"code", -32601},
            {"message", "Method not found: " + method}
        }}
    };
}

int run_mcp_stdio(const McpContext& ctx) {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::string trimmed = line;
        size_t s = trimmed.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        trimmed = trimmed.substr(s);

        try {
            nlohmann::json req = nlohmann::json::parse(trimmed);
            auto resp = compute_jsonrpc_response(req, ctx, true); // Stdio transport is inherently authenticated
            if (resp) {
                std::cout << resp->dump() << "\n" << std::flush;
            }
        } catch (const std::exception& ex) {
            nlohmann::json err = {
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {{"code", -32700}, {"message", std::string("Parse error: ") + ex.what()}}}
            };
            std::cout << err.dump() << "\n" << std::flush;
        }
    }
    return 0;
}

} // namespace machinebridge
