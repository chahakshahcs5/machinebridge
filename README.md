# MachineBridge (C++20)

High-performance, production-ready C++20 core engine and canonical reference implementation of **MachineBridge** featuring a unified HTTP/REST, Server-Sent Events (SSE), and WebSocket server, interactive pseudo-terminal (Windows ConPTY & POSIX PTY), RFC 8032 Ed25519 and OAuth 2.0 PKCE authentication, a full Model Context Protocol (MCP) tool suite with 15 verified tools, and automated Cloudflare Tunnel connectivity via `cloudflared`.

Featuring **first-class multi-environment capability**, allowing the exact same C++ codebase to adapt dynamically to desktop, server, Termux, rooted Android shells, and non-rooted Android application sandboxes.

---

## Technical Documentation & In-Depth Guides

Detailed technical specifications, architecture diagrams, and verification reports are available in the [`docs/`](./docs/) directory:

* 📐 [**Architecture & Subsystems**](./docs/ARCHITECTURE.md) — Comprehensive architectural breakdown, component model, PTY backends, and multi-environment detection.
* ⚡ [**Benchmarks & Performance**](./docs/BENCHMARKS.md) — Cold-start latency, memory footprint, throughput, and direct head-to-head comparison with Node.js/V8.
* 📦 [**Binary Size & Optimization**](./docs/BINARY_SIZE.md) — 1.05 MB Windows executable, 1.40 MB stripped Android binary, and the 523 KB APK DEFLATE packaging breakdown.
* 🧪 [**Testing & Quality Assurance**](./docs/TESTING.md) — The 8 CTest suites, unit and integration coverage, and Android remote/in-app verification procedures.
* 🌐 [**Wire Protocol Specification**](./docs/PROTOCOL.md) — REST API endpoints, SSE streams, raw terminal WebSocket protocol, and JSON-RPC 2.0 MCP schemas.
* 🛡️ [**Security Architecture**](./docs/SECURITY.md) — Constant-time authentication, RFC 8032 Ed25519, OAuth 2.1 PKCE, filesystem path traversal guards, and sandbox isolation.

---

## Multi-Platform Support Matrix

MachineBridge is designed and verified across five primary operating environments:

| Platform / Environment | Architecture | Process Identity | Execution PTY Backend | Binary / Artifact Size | Tested & Verified |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Windows 10/11 / Server** | `x86_64` (MSVC) | User UID | Windows ConPTY (`CreatePseudoConsole`) | **1.05 MB** (`machinebridge-server.exe`) | Windows 11 (8/8 tests passed) |
| **Linux (WSL / Ubuntu / Debian)** | `x86_64` (GCC 13.3) | User UID | POSIX PTY (`posix_openpt` / `grantpt`) | **968 KB** (`machinebridge-server`, stripped) | Ubuntu 24.04 (8/8 tests passed) |
| **Android Termux (CLI)** | `arm64-v8a` (NDK r26c) | Termux UID (`uid=10690`) | POSIX PTY (`/data/data/com.termux/files/usr/bin/bash`) | **1.40 MB** (stripped) | Realme (Android 14 / Linux 5.4) |
| **Android Standalone APK (Non-Root)** | `arm64-v8a` (NDK r26c) | App Sandbox UID (`uid=10171`) | POSIX PTY (`/system/bin/sh`) | **1.39 MB** (`libmachinebridge.so`, **510 KB** in APK) | Redmi Note 5 Pro (Android 9 / Linux 4.4) |
| **Android Rooted Phone Shell / Debian** | `arm64-v8a` (NDK r26c) | Root UID (`uid=0`) | POSIX PTY (`su` / `/bin/bash`) | **1.40 MB** (stripped) | Redmi Note 5 Pro (Magisk / Debian chroot) |

---

## Core Philosophy

> **"Machine Bridge provides the machine and exposes its capabilities. The AI determines how to use each available execution environment."**

Machine Bridge delivers rock-solid execution primitives:
* Command execution & child process trees
* Interactive PTY streaming (TIOCSWINSZ resize, signals, process groups)
* Filesystem operations (atomic read/write, batch operations, directory listing)
* Networking (REST, WebSocket, SSE, MCP)
* Host environment & per-session capability introspection

Machine Bridge does **not** bundle heavy development toolchains (Node.js, Python, Clang, GCC, CMake, Git, Rust, or BusyBox). If an AI agent requires specific tools, it inspects the environment capabilities, downloads compatible binaries dynamically into permitted application storage, and executes them through Machine Bridge primitives.

### Cloudflare Tunnel On-Demand Design
The official Go-based `cloudflared` binary is ~70 MB. It is **never bundled** into the C++ binary or the Android APK. Instead, when `--tunnel` is enabled, the runtime checks the cache directory, downloads the official binary on the fly, verifies its SHA-256 hash, and launches it as a background process.

### Size Optimization & Stripping
Unstripped Android binaries originally measured ~19 MB due to full DWARF debug info (`.debug_info`, `.debug_line`) and C++ template symbols. Passing `-s -Wl,--gc-sections` or running `llvm-strip --strip-all` eliminates unneeded metadata and brings the final standalone binary to **1.40 MB**.

---

## Host Environment vs. Per-Session Execution Context

Machine Bridge strictly separates **Server Process Identity** from **Child Execution Session Identity**:

```text
Machine Bridge Server Process
(Host Environment: UID = App UID, is_root = false, privileged_shell_available = true/false)
        │
        ├── Standard Session
        │       ↓
        │   /system/bin/sh
        │       ↓
        │   Session UID = App UID, is_root = false
        │
        └── Privileged Session (if su is verified available)
                ↓
               su
                ↓
            Root Shell
                ↓
            Session UID = 0, is_root = true
```

### 1. Host Environment (`EnvironmentInfo`)
Describes the Machine Bridge server process itself and host capabilities:
* `type`: `windows`, `linux`, `termux`, or `android-app`
* `process_uid` / `effective_uid`: Process identity under the host OS
* `is_root`: `true` only if the server process itself runs as root (UID 0)
* `privileged_shell_available`: `true` only if verified working `su` capability exists (`su -c "id -u"` exits 0 and returns `0`)
* `default_shell`, `home_dir`, `tmp_dir`, `workspace_dir`, `path_env`, `host_capabilities`

Available to AI agents via `GET /api/environment`.

### 2. Per-Session Execution Context (`SessionExecutionContext`)
Describes individual child execution sessions:
* Standard session on Android: `shell = "/system/bin/sh"`, `uid = 10171`, `is_root = false`
* Privileged session on rooted Android: `shell = "su"`, `uid = 0`, `is_root = true`

---

## Architecture & Features

```text
                            ┌───────────────────────────────────────────────┐
                            │               External Clients                │
                            │ (ChatGPT, Claude Desktop, Web UI, CLI Agent) │
                            └───────────────────────┬───────────────────────┘
                                                    │
                                     [Cloudflare Tunnel / HTTPS / LAN]
                                                    │
                                                    ▼
┌───────────────────────────────────────────────────────────────────────────────────────────────┐
│ MachineBridge Unified C++ Server (Port 8080)                                                  │
│                                                                                               │
│  ┌───────────────────────┐  ┌──────────────────────┐  ┌────────────────────────────────────┐  │
│  │   HTTP / REST API     │  │  WebSocket Streaming │  │    Server-Sent Events (SSE)        │  │
│  │  - Health checks      │  │  - Bi-directional PTY│  │    - /sse & /messages endpoints    │  │
│  │  - /api/environment   │  │    terminal I/O      │  │    - Terminal output push          │  │
│  │  - /api/status        │  │  - Signals (SigInt)  │  │    - RFC 8032 signed headers       │  │
│  │  - OAuth 2.1 PKCE UI  │  │  - Window resize     │  │                                    │  │
│  └───────────────────────┘  └──────────────────────┘  └────────────────────────────────────┘  │
│                                                                                               │
│  ┌─────────────────────────────────────────────────────────────────────────────────────────┐  │
│  │                     Model Context Protocol (MCP) Engine                                 │  │
│  │  - 15 verified tools (Execution, Filesystem batch, Session lifecycle)                   │  │
│  │  - JSON-RPC 2.0 over HTTP (/mcp, /sse) or Interactive Stdio (`--stdio`)                 │  │
│  └─────────────────────────────────────────────────────────────────────────────────────────┘  │
│                                                                                               │
│  ┌─────────────────────────────────────────────────────────────────────────────────────────┐  │
│  │                     Native Execution Engine & Storage                                   │  │
│  │  - Windows ConPTY (CreatePseudoConsole) & POSIX PTY (posix_openpt)                      │  │
│  │  - EnvironmentDetector (Windows, Linux, Termux, Android Sandbox, Verified Root)        │  │
│  │  - In-memory session ring buffers with TTL expiration (zero SQL/DB dependencies)        │  │
│  │  - Constant-time API key verification & RFC 8032 Curve25519 cryptography                │  │
│  │  - Path traversal & Windows reserved device name security defenses                      │  │
│  │  - Thread-safe recent log ring buffer (300 lines) with JNI drain interface              │  │
│  └─────────────────────────────────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────┬───────────────────────────────────────────────┘
                                                │
                                                ▼
                            ┌───────────────────────────────────────┐
                            │    Cloudflare Tunnel (cloudflared)    │
                            │  - Named tunnels with token           │
                            │  - Quick ad-hoc public trycloudflare  │
                            │  - Downloaded on demand at runtime    │
                            └───────────────────────────────────────┘
```

---

## Command-Line Arguments

The standalone executable accepts the following arguments:

```text
Usage: machinebridge-server [options]

Options:
  --port <port>                 Listen port (default: 8080)
  --host <host>                 Bind host address (default: 0.0.0.0)
  --api-key <key>               Authentication API key (auto-generated if omitted)
  --shell <path>                Default shell path (default: /system/bin/sh, /bin/bash, or cmd.exe)
  --workspace <path>            Root workspace directory (default: current working directory)
  --tunnel                      Start Cloudflare Tunnel (trycloudflare or named token)
  --tunnel-protocol <protocol>  Cloudflare Tunnel protocol: 'http2' (default) or 'quic'
  --tunnel-token <token>        Cloudflare Named Tunnel authentication token
  --log-level <level>           Log level: debug, info (default), warn, error
  --max-sessions <num>          Maximum concurrent PTY sessions (default: 10)
  --session-ttl <seconds>       Inactive session timeout in seconds (default: 3600)
  --verbose                     Enable verbose debug logging to stdout/stderr
  --stdio                       Run MCP in stdio mode (JSON-RPC over stdin/stdout)
  --help                        Display this help message
```

---

## 15 Verified MCP Tools

| Tool Name | Category | Description |
|---|---|---|
| `execute_command` | Terminal | Execute a single shell command with timeout and ANSI output parsing |
| `execute_commands` | Terminal | Sequentially execute an array of commands with stop-on-error and per-command exit codes |
| `read_file` | Filesystem | Read file contents with offset, length chunking, and utf8/base64 encoding |
| `write_file` | Filesystem | Write or append file contents with SHA-256 verification and automatic parent directories |
| `delete_file` | Filesystem | Delete a file or directory recursively |
| `make_directory` | Filesystem | Create directory hierarchies |
| `move_file` | Filesystem | Move or rename files and directories |
| `copy_file` | Filesystem | Copy files and directories recursively with overwrite control |
| `list_directory` | Filesystem | List directory entries with size, timestamps, and permissions |
| `stat_file` | Filesystem | Retrieve detailed file/directory metadata (size, isDirectory, mtime, birthtime) |
| `batch_fs` | Filesystem | Atomically execute a batch sequence of filesystem operations (`read`, `write`, `stat`, `delete`) |
| `create_session` | Session | Allocate a persistent virtual terminal session with TTL expiration |
| `send_input` | Session | Send keystrokes, commands, or signals (`SigInt`) to an active PTY session |
| `read_output` | Session | Retrieve accumulated output from a session's ring buffer |
| `close_session` | Session | Terminate an active terminal process tree and release session resources |

---

## Building from Source

### Prerequisites
* **C++20 Compiler**: MSVC 2022/2026 (Windows), GCC 13+ or Clang 16+ (Linux), Android NDK r26c (Android)
* **CMake**: Version 3.20 or newer
* **Ninja**: Recommended for fast builds

---

### 1. Linux Build (WSL / Native Ubuntu)
```bash
cmake -B build-linux -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-linux
strip --strip-all build-linux/machinebridge-server
ctest --test-dir build-linux --output-on-failure
```
*Resulting binary: ~968 KB.*

---

### 2. Windows Build (Visual Studio / MSVC)

The Windows build includes automatic multi-processor compilation (`/MP`) and CMake native Precompiled Headers (PCH) to accelerate compilation of template-heavy headers such as `nlohmann/json.hpp`.

```powershell
# Standard fast parallel build (PCH + /MP enabled)
cmake -B build -S . -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```
*Resulting executable: ~1.05 MB (`machinebridge-server.exe`).*

#### Build Acceleration Options

| CMake Option | Default | Description |
| :--- | :--- | :--- |
| `MACHINEBRIDGE_ENABLE_LTCG` | `OFF` | Enables Whole Program Optimization (`/GL` & `/LTCG`) for Release server/CLI binaries. Kept `OFF` during development for fast linking. |
| `MACHINEBRIDGE_ENABLE_UNITY_BUILD` | `OFF` | Merges `machinebridge_core` translation units into jumbo batches for ultra-fast clean builds. |

For maximum build throughput on Windows, building with **Ninja** via the Visual Studio developer environment is recommended:
```powershell
cmake -B build-ninja -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-ninja
```


---

### 3. Android Standalone Executable & Shared Library (NDK r26c)
```bash
cmake -B build-android -S . -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/android-ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release

# Build standalone CLI executable (for Termux / root shell)
ninja -C build-android machinebridge-server
llvm-strip --strip-all build-android/machinebridge-server

# Build shared library for Android APK
ninja -C build-android machinebridge_shared
llvm-strip --strip-unneeded build-android/libmachinebridge.so
```
*Standalone CLI binary: ~1.40 MB. Shared library: ~1.39 MB (compresses to 510 KB in APK).*

---

## In-App Logging & JNI Architecture

To support real-time log streaming in the Android app without performance penalties or locking bugs:
* `Logger` provides `get_and_clear_recent_logs()` which drains an internal 300-entry `std::deque<std::string>`.
* `Java_com_machinebridge_app_NativeBridge_nativeGetRecentLogs` fetches buffered log lines every second from the UI thread.
* `MachineBridgeServer::stop()` detaches tunnel worker threads rather than blocking up to 45 seconds on process exit, ensuring responsive start/stop state transitions without ANRs.

---

## License

MIT License.
