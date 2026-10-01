cmake_minimum_required(VERSION 3.20)

project(machinebridge
    VERSION 1.0.0
    DESCRIPTION "Core C++20 reference engine for MachineBridge with unified REST, WebSocket, SSE, MCP, and Cloudflare Tunnel"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MACHINEBRIDGE_STATIC_RUNTIME "Link MSVC runtime statically (/MT)" ON)
if(MSVC AND MACHINEBRIDGE_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(MACHINEBRIDGE_ENABLE_LTCG "Enable Link-Time Code Generation (/GL and /LTCG) for Release executables" OFF)
option(MACHINEBRIDGE_ENABLE_UNITY_BUILD "Enable Unity (jumbo) build for faster compilation" OFF)


# ---------------------------------------------------------------------------
# Cloudflare Tunnel Subproject Integration
# ---------------------------------------------------------------------------

option(MACHINEBRIDGE_ENABLE_TUNNEL "Enable Cloudflare Tunnel integration" ON)

set(HAVE_CLOUDFLARED FALSE)
if(MACHINEBRIDGE_ENABLE_TUNNEL)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../cloudflare-tunnel/CMakeLists.txt")
        set(DEFAULT_CLOUDFLARE_TUNNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cloudflare-tunnel")
    elseif(WIN32 AND EXISTS "C:/Users/chaha/Projects/cloudflare-tunnel/CMakeLists.txt")
        set(DEFAULT_CLOUDFLARE_TUNNEL_DIR "C:/Users/chaha/Projects/cloudflare-tunnel")
    else()
        set(DEFAULT_CLOUDFLARE_TUNNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cloudflare-tunnel")
    endif()
    set(CLOUDFLARE_TUNNEL_DIR "${DEFAULT_CLOUDFLARE_TUNNEL_DIR}" CACHE PATH "Path to cloudflare-tunnel source")

    if(EXISTS "${CLOUDFLARE_TUNNEL_DIR}/CMakeLists.txt")
        message(STATUS "Including cloudflare-tunnel from: ${CLOUDFLARE_TUNNEL_DIR}")
        add_subdirectory("${CLOUDFLARE_TUNNEL_DIR}" "${CMAKE_BINARY_DIR}/cloudflare-tunnel" EXCLUDE_FROM_ALL)
        set(HAVE_CLOUDFLARED TRUE)
    else()
        message(WARNING "cloudflare-tunnel directory not found at ${CLOUDFLARE_TUNNEL_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Core Library: machinebridge_core
# ---------------------------------------------------------------------------

set(CORE_SOURCES
    src/config.cpp
    src/crypto.cpp
    src/protocol.cpp
    src/shared.cpp
    src/fs.cpp
    src/executor.cpp
    src/session_store.cpp
    src/oauth.cpp
    src/mcp.cpp
    src/tunnel.cpp
    src/websocket.cpp
    src/http_server.cpp
    src/logger.cpp
    src/environment.cpp
    src/server.cpp
)

if(WIN32)
    list(APPEND CORE_SOURCES src/pty_windows.cpp)
else()
    list(APPEND CORE_SOURCES src/pty_posix.cpp)
endif()

add_library(machinebridge_core STATIC ${CORE_SOURCES})
add_library(machinebridge::core ALIAS machinebridge_core)

target_include_directories(machinebridge_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(MACHINEBRIDGE_ENABLE_UNITY_BUILD)
    set_target_properties(machinebridge_core PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 8)
endif()


target_precompile_headers(machinebridge_core PRIVATE
    <iostream>
    <string>
    <vector>
    <memory>
    <chrono>
    <filesystem>
    <functional>
    <sstream>
    <nlohmann/json.hpp>
)

if(HAVE_CLOUDFLARED AND TARGET cloudflared)
    target_link_libraries(machinebridge_core PUBLIC cloudflared)
    target_compile_definitions(machinebridge_core PUBLIC MACHINEBRIDGE_ENABLE_TUNNEL=1)
endif()

if(WIN32)
    target_link_libraries(machinebridge_core PUBLIC ws2_32 bcrypt crypt32)
    target_compile_definitions(machinebridge_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(machinebridge_core PUBLIC 
            /MP
            /utf-8 
            /W4 
            /Gy 
            /Gw
            $<$<CONFIG:Release>:/O1> 
            $<$<CONFIG:Release>:/Os> 
            $<$<AND:$<CONFIG:Release>,$<BOOL:${MACHINEBRIDGE_ENABLE_LTCG}>>:/GL>
        )
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(machinebridge_core PUBLIC Threads::Threads)
    if(ANDROID)
        target_link_libraries(machinebridge_core PUBLIC log)
    endif()
    target_compile_options(machinebridge_core PUBLIC -Wall -Wextra -Wpedantic -Os)
endif()

# ---------------------------------------------------------------------------
# Android Native Artifacts (JNI Shared Library & PTY Test)
# ---------------------------------------------------------------------------

if(ANDROID)
    add_library(machinebridge_shared SHARED
        src/platform/android/jni_bridge.cpp
    )
    set_target_properties(machinebridge_shared PROPERTIES 
        OUTPUT_NAME "machinebridge"
        PREFIX "lib"
        SUFFIX ".so"
    )
    target_link_libraries(machinebridge_shared PRIVATE machinebridge_core log)
    target_compile_options(machinebridge_shared PRIVATE
        -Os
        -fvisibility=hidden
        -fdata-sections
        -ffunction-sections
    )
    target_link_options(machinebridge_shared PRIVATE
        -Wl,--gc-sections
        -Wl,--exclude-libs,ALL
        -s
    )

    add_executable(test_pty_android tests/test_pty_android.cpp)
    target_link_libraries(test_pty_android PRIVATE machinebridge_core log)
endif()

# ---------------------------------------------------------------------------
# Code Signing for Windows Smart App Control
# ---------------------------------------------------------------------------

if(WIN32)
    function(auto_sign_target TARGET_NAME)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND powershell.exe -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/sign_target.ps1" "$<TARGET_FILE:${TARGET_NAME}>"
            COMMENT "Signing ${TARGET_NAME} with developer certificate"
        )
    endfunction()
else()
    function(auto_sign_target TARGET_NAME)
    endfunction()
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------

# Unified Server (HTTP, SSE, WebSocket, MCP Stdio, Tunnel)
add_executable(machinebridge_server src/server_main.cpp)
set_target_properties(machinebridge_server PROPERTIES OUTPUT_NAME "machinebridge-server")
target_link_libraries(machinebridge_server PRIVATE machinebridge_core)
if(MSVC)
    if(MACHINEBRIDGE_ENABLE_LTCG)
        target_compile_options(machinebridge_server PRIVATE $<$<CONFIG:Release>:/GL>)
        target_link_options(machinebridge_server PRIVATE 
            $<$<CONFIG:Release>:/LTCG> 
            $<$<CONFIG:Release>:/OPT:REF> 
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    else()
        target_link_options(machinebridge_server PRIVATE 
            $<$<CONFIG:Release>:/OPT:REF> 
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    endif()
else()
    target_link_options(machinebridge_server PRIVATE -s -Wl,--gc-sections)
endif()
auto_sign_target(machinebridge_server)

# CLI Client
add_executable(machinebridge_cli src/cli_main.cpp)
set_target_properties(machinebridge_cli PROPERTIES OUTPUT_NAME "machinebridge-cli")
target_link_libraries(machinebridge_cli PRIVATE machinebridge_core)
if(MSVC)
    if(MACHINEBRIDGE_ENABLE_LTCG)
        target_compile_options(machinebridge_cli PRIVATE $<$<CONFIG:Release>:/GL>)
        target_link_options(machinebridge_cli PRIVATE 
            $<$<CONFIG:Release>:/LTCG> 
            $<$<CONFIG:Release>:/OPT:REF> 
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    else()
        target_link_options(machinebridge_cli PRIVATE 
            $<$<CONFIG:Release>:/OPT:REF> 
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    endif()
else()
    target_link_options(machinebridge_cli PRIVATE -s -Wl,--gc-sections)
endif()
auto_sign_target(machinebridge_cli)

# ---------------------------------------------------------------------------
# Unit Tests
# ---------------------------------------------------------------------------

enable_testing()

function(add_machinebridge_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE machinebridge_core)
    auto_sign_target(${TEST_NAME})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

add_machinebridge_test(test_crypto tests/test_crypto.cpp)
add_machinebridge_test(test_protocol tests/test_protocol.cpp)
add_machinebridge_test(test_fs tests/test_fs.cpp)
add_machinebridge_test(test_executor tests/test_executor.cpp)
add_machinebridge_test(test_batch tests/test_batch.cpp)
add_machinebridge_test(test_tools tests/test_tools.cpp)
add_machinebridge_test(test_oauth tests/test_oauth.cpp)
add_machinebridge_test(test_server tests/test_server.cpp)
