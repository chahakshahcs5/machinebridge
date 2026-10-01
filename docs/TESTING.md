# MachineBridge (C++20) — Testing Guide & Verification Suite

## 1. Overview

MachineBridge features a comprehensive automated test suite implemented via CMake/CTest. Tests validate every layer of the stack: low-level cryptography, HTTP and wire protocol framing, secure filesystem operations, batch operations, shell and PTY execution, tool registration, OAuth 2.1 PKCE flows, and end-to-end server orchestration.

---

## 2. Test Suite Inventory

| Test Target | Source File | Category | Focus Areas |
| :--- | :--- | :--- | :--- |
| **`test_crypto`** | `tests/test_crypto.cpp` | Security / Crypto | RFC 8032 Ed25519 keygen, signing, verification, timing-safe equality, API key generation |
| **`test_protocol`** | `tests/test_protocol.cpp` | Wire Protocol | HTTP/1.1 request/response parsing, headers, status codes, query strings, SSE events, WebSocket framing |
| **`test_fs`** | `tests/test_fs.cpp` | Filesystem | Path traversal rejection (`../`, absolute escaping), reserved Windows device names (`CON`, `NUL`), CRUD, stat |
| **`test_batch`** | `tests/test_batch.cpp` | Filesystem | `batch_fs` atomic executions, transactional rollback, error containment, sequential reads/writes |
| **`test_executor`** | `tests/test_executor.cpp` | Process Execution | Child process spawning, exit code capture, timeouts, output streams, pipe buffering |
| **`test_tools`** | `tests/test_tools.cpp` | MCP Engine | All 15 MCP tool registrations, JSON-RPC 2.0 schema validation, dispatch error codes |
| **`test_oauth`** | `tests/test_oauth.cpp` | Authentication | OAuth 2.1 PKCE flow, code challenge verification, bearer token generation and validation |
| **`test_server`** | `tests/test_server.cpp` | Integration | End-to-end HTTP server lifecycle, `/health` endpoint, `/api/environment`, MCP over HTTP |
| **`test_pty_android`** | `tests/test_pty_android.cpp` | Platform Specific | POSIX PTY allocation on Android Bionic, `/system/bin/sh` spawning, I/O streaming, window resize, signals |

---

## 3. Running Tests Locally

### 3.1 Windows (MSVC)
```powershell
cd machinebridge-cpp
cmake -B build -S . -A x64
cmake --build build --config Release --parallel

# Run all 8 test suites with full verbose output on failure
ctest --test-dir build -C Release --output-on-failure
```

### 3.2 Linux (Native Ubuntu / WSL2)
```bash
cd machinebridge-cpp
cmake -B build-linux -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-linux

# Run all 8 test suites
ctest --test-dir build-linux --output-on-failure
```

### 3.3 Running an Individual Test Suite
```bash
# Example: Run only filesystem security tests
ctest --test-dir build-linux -R test_fs --verbose
```

---

## 4. Android Remote & In-App Testing

Because Android utilizes Bionic libc, SELinux policies, and distinct UID sandboxing, MachineBridge includes specialized Android verification procedures:

### 4.1 Remote Device Testing via SSH

MachineBridge has been verified on physical Android devices across distinct privilege levels:

| SSH Alias | Device | Environment | Execution Shell | Test Procedure |
| :--- | :--- | :--- | :--- | :--- |
| `ssh redmi-root` | Redmi Note 5 Pro | Rooted (Magisk, Android 9) | `uid=0` (direct root) | Tests standalone server binary, `su` elevation, and unrestricted PTY allocation |
| `ssh redmi-debian` | Redmi Note 5 Pro | Debian chroot on Android | `/bin/bash` | Tests Linux-like userland on real mobile ARM64 hardware |
| `ssh realme-termux` | Realme 7 | Non-root (Android 14) | Termux `bash` (`uid=10690`) | Tests non-root CLI standalone server under Android 14 SELinux restrictions |

**Example Push and Test Execution**:
```bash
# 1. Cross-compile standalone binary for Android ARM64
cmake -B build-android -S . -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/android-ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release
ninja -C build-android machinebridge-server
llvm-strip --strip-all build-android/machinebridge-server

# 2. Push to device and execute
scp build-android/machinebridge-server redmi-root:/data/local/tmp/
ssh redmi-root "chmod +x /data/local/tmp/machinebridge-server && /data/local/tmp/machinebridge-server --port 8080"
```

### 4.2 In-App Diagnostic Suite (Phase 0 PTY Self-Test)
The `machinebridge-apk` management app includes a built-in **"RUN PTY SELF-TEST"** diagnostic runner directly accessible from the dashboard:
* Calls `NativeBridge.nativeRunPtyTest()` via JNI.
* Executes internal diagnostic checks directly within the untrusted Android app sandbox UID (`uid=10171`):
  1. Allocation of POSIX master/slave PTY (`posix_openpt`).
  2. Spawning of `/system/bin/sh`.
  3. Interactive input write and response capture (`id`, `pwd`).
  4. Dynamic terminal resizing (`TIOCSWINSZ`).
  5. Safe process tree termination and child wait status verification.
* Displays detailed pass/fail diagnostic outputs in the in-app log viewer.
