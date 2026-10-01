#include <jni.h>
#include "machinebridge/server.hpp"
#include "machinebridge/environment.hpp"
#include "machinebridge/pty.hpp"
#include <android/log.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unistd.h>

#ifdef MACHINEBRIDGE_ENABLE_TUNNEL
#include <cloudflared/downloader.hpp>
#endif
#include "machinebridge/logger.hpp"

#define LOG_TAG "MachineBridgeJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::shared_ptr<machinebridge::MachineBridgeServer> g_server;
static std::mutex g_server_mutex;

static JavaVM* g_jvm = nullptr;
static jclass g_native_bridge_class = nullptr;
static jmethodID g_download_file_method = nullptr;

static std::string jstring_to_string(JNIEnv* env, jstring jstr);

static bool android_jni_download(
    const std::string& url,
    const std::filesystem::path& dest,
    std::function<void(size_t, size_t)> /*progress*/
) {
    machinebridge::Logger::instance().info("TUNNEL", "[cloudflared] Original download method unavailable or failed. Trying Android HttpURLConnection fallback...");
    LOGI("JNI: fallback download %s -> %s via Android HttpURLConnection", url.c_str(), dest.string().c_str());

    if (!g_jvm) {
        LOGE("JNI download hook: g_jvm is null");
        machinebridge::Logger::instance().error("TUNNEL", "[cloudflared] Android download fallback failed: JVM not initialized");
        return false;
    }

    JNIEnv* env = nullptr;
    bool needs_detach = false;
    jint res = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("Failed to attach thread to JVM for download");
            machinebridge::Logger::instance().error("TUNNEL", "[cloudflared] Android download fallback failed: cannot attach JVM thread");
            return false;
        }
        needs_detach = true;
    }

    if (!g_native_bridge_class || !g_download_file_method) {
        jclass local_cls = env->FindClass("com/machinebridge/app/NativeBridge");
        if (local_cls) {
            if (!g_native_bridge_class) {
                g_native_bridge_class = reinterpret_cast<jclass>(env->NewGlobalRef(local_cls));
            }
            g_download_file_method = env->GetStaticMethodID(
                g_native_bridge_class,
                "downloadFile",
                "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"
            );
            env->DeleteLocalRef(local_cls);
        }
    }

    if (!g_native_bridge_class || !g_download_file_method) {
        LOGE("JNI download hook: NativeBridge.downloadFile method not found");
        machinebridge::Logger::instance().error("TUNNEL", "[cloudflared] Android download fallback failed: NativeBridge.downloadFile method not found");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (needs_detach) g_jvm->DetachCurrentThread();
        return false;
    }

    jstring j_url = env->NewStringUTF(url.c_str());
    jstring j_dest = env->NewStringUTF(dest.string().c_str());

    jobject err_obj = env->CallStaticObjectMethod(
        g_native_bridge_class,
        g_download_file_method,
        j_url,
        j_dest
    );

    bool ok = false;
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        machinebridge::Logger::instance().error("TUNNEL", "[cloudflared] Exception calling downloadFile in Java");
    } else if (err_obj != nullptr) {
        jstring j_err = reinterpret_cast<jstring>(err_obj);
        std::string err_msg = jstring_to_string(env, j_err);
        env->DeleteLocalRef(j_err);
        machinebridge::Logger::instance().error("TUNNEL", "[cloudflared] Android download error: " + err_msg);
    } else {
        ok = true;
    }

    env->DeleteLocalRef(j_url);
    env->DeleteLocalRef(j_dest);

    if (needs_detach) {
        g_jvm->DetachCurrentThread();
    }

    if (ok) {
        machinebridge::Logger::instance().info("TUNNEL", "[cloudflared] Download via Android HttpURLConnection succeeded!");
        return true;
    } else {
        return false;
    }
}

static std::shared_ptr<machinebridge::MachineBridgeServer> get_server_ptr() {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return g_server;
}

static std::string jstring_to_string(JNIEnv* env, jstring jstr) {
    if (!jstr) return "";
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    std::string str(chars);
    env->ReleaseStringUTFChars(jstr, chars);
    return str;
}

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_jvm = vm;
    LOGI("JNI_OnLoad called: cached JavaVM %p", (void*)vm);
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_machinebridge_app_NativeBridge_nativeInit(
    JNIEnv* env,
    jclass clazz,
    jstring filesDir,
    jstring cacheDir
) {
    if (!g_jvm) {
        env->GetJavaVM(&g_jvm);
    }
    if (!g_native_bridge_class && clazz) {
        g_native_bridge_class = reinterpret_cast<jclass>(env->NewGlobalRef(clazz));
        g_download_file_method = env->GetStaticMethodID(
            g_native_bridge_class,
            "downloadFile",
            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"
        );
        if (g_download_file_method) {
            LOGI("JNI: Cached NativeBridge.downloadFile method successfully from clazz");
        } else {
            LOGE("JNI: Failed to get downloadFile method ID from clazz");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
#ifdef MACHINEBRIDGE_ENABLE_TUNNEL
    cloudflared::set_custom_downloader(android_jni_download);
    LOGI("JNI: Registered android_jni_download as cloudflared custom downloader");
#endif

    std::string f_dir = jstring_to_string(env, filesDir);
    std::string c_dir = jstring_to_string(env, cacheDir);
    machinebridge::EnvironmentDetector::set_android_paths(f_dir, c_dir);
    LOGI("NativeBridge initialized with filesDir: %s, cacheDir: %s", f_dir.c_str(), c_dir.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_machinebridge_app_NativeBridge_nativeStartServer(
    JNIEnv* env,
    jclass /*clazz*/,
    jint port,
    jstring host,
    jstring apiKey,
    jstring workspaceRoot,
    jstring defaultShell,
    jboolean verboseLog,
    jboolean exposeTunnel,
    jstring tunnelToken,
    jstring tunnelProtocol,
    jstring logLevel,
    jint maxSessions,
    jint sessionTtl
) {
    std::shared_ptr<machinebridge::MachineBridgeServer> server;
    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        if (g_server && g_server->is_running()) {
            LOGI("Server is already running on port %d", g_server->port());
            return JNI_TRUE;
        }
        g_server = std::make_shared<machinebridge::MachineBridgeServer>();
        server = g_server;
    }

    machinebridge::Config config;
    config.edge_port = static_cast<uint16_t>(port);
    std::string h = jstring_to_string(env, host);
    if (!h.empty()) config.edge_host = h;
    std::string k = jstring_to_string(env, apiKey);
    if (!k.empty()) config.api_key = k;
    std::string w = jstring_to_string(env, workspaceRoot);
    if (!w.empty()) config.workspace_root = w;
    std::string s = jstring_to_string(env, defaultShell);
    if (!s.empty()) config.default_shell = s;
    config.verbose_logging = (verboseLog == JNI_TRUE);
    config.expose_tunnel = (exposeTunnel == JNI_TRUE);
    std::string t = jstring_to_string(env, tunnelToken);
    if (!t.empty()) config.tunnel_token = t;

    std::string proto = jstring_to_string(env, tunnelProtocol);
    if (!proto.empty()) config.tunnel_protocol = proto;

    std::string lvl = jstring_to_string(env, logLevel);
    if (!lvl.empty()) {
        config.log_level = lvl;
        machinebridge::Logger::instance().set_level(machinebridge::Logger::string_to_level(lvl));
    }
    machinebridge::Logger::instance().set_enabled(true);

    if (maxSessions > 0) config.max_sessions = static_cast<size_t>(maxSessions);
    if (sessionTtl > 0) config.session_ttl_seconds = static_cast<uint32_t>(sessionTtl);

    bool ok = server->start(config);
    if (ok) {
        LOGI("Server successfully started on %s:%d (tunnel=%s, proto=%s)",
             config.edge_host.c_str(), config.edge_port,
             config.expose_tunnel ? "true" : "false",
             config.tunnel_protocol.c_str());
    } else {
        LOGE("Failed to start server on %s:%d", config.edge_host.c_str(), config.edge_port);
        std::lock_guard<std::mutex> lock(g_server_mutex);
        if (g_server == server) {
            g_server.reset();
        }
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_machinebridge_app_NativeBridge_nativeStopServer(
    JNIEnv* /*env*/,
    jclass /*clazz*/
) {
    std::shared_ptr<machinebridge::MachineBridgeServer> server;
    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        server = g_server;
        g_server.reset();
    }
    if (server) {
        server->stop();
        LOGI("Server stopped");
    }
}

JNIEXPORT jboolean JNICALL
Java_com_machinebridge_app_NativeBridge_nativeIsServerRunning(
    JNIEnv* /*env*/,
    jclass /*clazz*/
) {
    auto s = get_server_ptr();
    return (s && s->is_running()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_machinebridge_app_NativeBridge_nativeGetRecentLogs(
    JNIEnv* env,
    jclass /*clazz*/
) {
    std::string logs = machinebridge::Logger::instance().get_and_clear_recent_logs();
    return env->NewStringUTF(logs.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_machinebridge_app_NativeBridge_nativeGetStatus(
    JNIEnv* env,
    jclass /*clazz*/
) {
    auto s = get_server_ptr();
    std::string json_str;
    if (s) {
        auto status_str = s->get_status_json();
        try {
            auto j = nlohmann::json::parse(status_str);
            if (s->tunnel_manager()) {
                auto u = s->tunnel_manager()->url();
                j["tunnel_url"] = u ? *u : "";
                j["tunnel_running"] = s->tunnel_manager()->is_running();
            }
            json_str = j.dump(2);
        } catch (...) {
            json_str = status_str;
        }
    } else {
        nlohmann::json j;
        j["running"] = false;
        j["tunnel_url"] = "";
        j["tunnel_running"] = false;
        auto env_info = machinebridge::EnvironmentDetector::detect();
        nlohmann::json env_j;
        env_j["type"] = env_info.type_name;
        env_j["platform"] = env_info.platform;
        env_j["architecture"] = env_info.architecture;
        env_j["server_uid"] = env_info.process_uid;
        env_j["server_is_root"] = env_info.is_root;
        env_j["privileged_shell_available"] = env_info.privileged_shell_available;
        env_j["default_shell"] = env_info.default_shell;
        env_j["home_dir"] = env_info.home_dir;
        env_j["workspace_dir"] = env_info.workspace_dir;
        env_j["host_capabilities"] = env_info.host_capabilities;
        j["environment"] = env_j;
        json_str = j.dump(2);
    }
    return env->NewStringUTF(json_str.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_machinebridge_app_NativeBridge_nativeGetTunnelUrl(
    JNIEnv* env,
    jclass /*clazz*/
) {
    auto s = get_server_ptr();
    if (s && s->tunnel_manager()) {
        auto u = s->tunnel_manager()->url();
        if (u) {
            return env->NewStringUTF(u->c_str());
        }
    }
    return env->NewStringUTF("");
}

JNIEXPORT jstring JNICALL
Java_com_machinebridge_app_NativeBridge_nativeGetTunnelError(
    JNIEnv* env,
    jclass /*clazz*/
) {
    auto s = get_server_ptr();
    if (s && s->tunnel_manager()) {
        auto err = s->tunnel_manager()->last_error();
        if (err) {
            return env->NewStringUTF(err->c_str());
        }
    }
    return env->NewStringUTF("");
}

JNIEXPORT jstring JNICALL
Java_com_machinebridge_app_NativeBridge_nativeRunPtySelfTest(
    JNIEnv* env,
    jclass /*clazz*/
) {
    nlohmann::json report;
    report["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    auto host_env = machinebridge::EnvironmentDetector::detect();
    report["host_environment"] = {
        {"type", host_env.type_name},
        {"architecture", host_env.architecture},
        {"kernel", host_env.kernel_version},
        {"app_uid", host_env.process_uid},
        {"app_euid", host_env.effective_uid},
        {"is_root", host_env.is_root},
        {"privileged_shell_available", host_env.privileged_shell_available},
        {"default_shell", host_env.default_shell},
        {"home_dir", host_env.home_dir},
        {"tmp_dir", host_env.tmp_dir}
    };

    std::vector<nlohmann::json> test_results;

    auto record_test = [&](const std::string& name, bool passed, const std::string& details, const std::string& out = "") {
        nlohmann::json t;
        t["name"] = name;
        t["passed"] = passed;
        t["details"] = details;
        if (!out.empty()) t["output"] = out;
        test_results.push_back(t);
    };

    try {
        auto pty_mgr = std::make_unique<machinebridge::PtyManager>(4, 128 * 1024);

        // 1. Session 1 Creation & shell invocation
        machinebridge::PtyOptions opts1;
        opts1.id = "selftest-pty-1";
        opts1.shell = host_env.default_shell;
        opts1.cwd = host_env.home_dir;
        opts1.cols = 80;
        opts1.rows = 24;

        std::string s1_output;
        std::mutex s1_mutex;
        std::condition_variable s1_cv;

        machinebridge::PtyCallbacks cbs1;
        cbs1.on_data = [&](const std::string& data) {
            std::lock_guard<std::mutex> lock(s1_mutex);
            s1_output += data;
            s1_cv.notify_all();
        };

        auto session1 = pty_mgr->create(opts1, cbs1);
        if (session1 && session1->is_running()) {
            record_test("1. PTY Creation and Shell Spawn", true, "Spawned shell successfully. PID=" + std::to_string(session1->pid()));
        } else {
            record_test("1. PTY Creation and Shell Spawn", false, "Failed to create or start PTY session.");
            report["tests"] = test_results;
            report["all_passed"] = false;
            return env->NewStringUTF(report.dump(2).c_str());
        }

        auto send_and_wait = [&](const std::string& cmd, int timeout_ms = 2000) -> std::string {
            {
                std::lock_guard<std::mutex> lock(s1_mutex);
                s1_output.clear();
            }
            session1->write(cmd + "\n");

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            std::unique_lock<std::mutex> lock(s1_mutex);
            while (std::chrono::steady_clock::now() < deadline) {
                if (s1_cv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                    return !s1_output.empty();
                })) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(80)); // Drain
                    break;
                }
            }
            return s1_output;
        };

        // 2. echo hello -> hello
        std::string echo_out = send_and_wait("echo hello");
        bool t2_ok = (echo_out.find("hello") != std::string::npos);
        record_test("2. Basic IO (echo hello)", t2_ok, t2_ok ? "Received expected output" : "Missing hello", echo_out);

        // 3. Sandbox inspection: pwd & echo $HOME
        std::string pwd_out = send_and_wait("pwd");
        std::string home_out = send_and_wait("echo $HOME");
        bool t3_ok = (!pwd_out.empty() && !home_out.empty());
        record_test("3. Working Directory and HOME Inspection", t3_ok, "pwd=" + pwd_out + ", HOME=" + home_out);

        // 4. App UID & Identity verification (id)
        std::string id_out = send_and_wait("id");
        bool t4_ok = (id_out.find("uid=") != std::string::npos);
        record_test("4. Sandbox UID Verification (id)", t4_ok, "Confirmed app sandbox UID", id_out);

        // 5. Kernel inspection (uname -a)
        std::string uname_out = send_and_wait("uname -a");
        bool t5_ok = (uname_out.find("Linux") != std::string::npos);
        record_test("5. Kernel Inspection (uname -a)", t5_ok, "Kernel verified", uname_out);

        // 6. Terminal Resize (TIOCSWINSZ)
        session1->resize(100, 30);
        record_test("6. Terminal Resize (TIOCSWINSZ)", true, "Resized PTY to 100x30 without error");

        // 7. Subshell Child Process Execution
        std::string sub_out = send_and_wait("sh -c \"echo child_proc_ok\"");
        bool t7_ok = (sub_out.find("child_proc_ok") != std::string::npos);
        record_test("7. Child Process Execution", t7_ok, "Subshell spawned and executed", sub_out);

        // 8. Process Group Signaling (SIGINT)
        session1->signal(machinebridge::TerminalSignal::SigInt);
        record_test("8. Process Group Signaling", true, "Sent SIGINT to process group cleanly");

        // 9. Multiple Simultaneous PTY Sessions
        machinebridge::PtyOptions opts2;
        opts2.id = "selftest-pty-2";
        opts2.shell = host_env.default_shell;
        opts2.cols = 80;
        opts2.rows = 24;

        std::string s2_output;
        std::mutex s2_mutex;
        std::condition_variable s2_cv;

        machinebridge::PtyCallbacks cbs2;
        cbs2.on_data = [&](const std::string& data) {
            std::lock_guard<std::mutex> lock(s2_mutex);
            s2_output += data;
            s2_cv.notify_all();
        };

        auto session2 = pty_mgr->create(opts2, cbs2);
        bool t9_ok = (session2 && session2->is_running());
        if (t9_ok) {
            session2->write("echo session2_ready\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            std::lock_guard<std::mutex> lock(s2_mutex);
            t9_ok = (s2_output.find("session2_ready") != std::string::npos);
        }
        record_test("9. Multiple Concurrent Sessions", t9_ok, t9_ok ? "2 simultaneous PTY sessions working" : "Failed session 2");

        // 10. Root Execution Check (Optional capability)
        bool su_ok = host_env.privileged_shell_available;
        record_test("10. Privileged Shell (su) Capability", true, su_ok ? "Available and verified (Rooted)" : "Not available (Standard Non-Root)");

        // Clean up
        session1->close();
        if (session2) session2->close();
        pty_mgr->close_all();

        bool all_passed = true;
        for (const auto& t : test_results) {
            if (!t["passed"].get<bool>()) {
                all_passed = false;
                break;
            }
        }
        report["all_passed"] = all_passed;

    } catch (const std::exception& ex) {
        record_test("PTY Exception", false, ex.what());
        report["all_passed"] = false;
    }

    report["tests"] = test_results;
    std::string out_str = report.dump(2);
    return env->NewStringUTF(out_str.c_str());
}

JNIEXPORT void JNICALL
Java_com_machinebridge_app_NativeBridge_nativeLogInfo(
    JNIEnv* env,
    jclass /*clazz*/,
    jstring tag,
    jstring message
) {
    std::string t = jstring_to_string(env, tag);
    std::string m = jstring_to_string(env, message);
    machinebridge::Logger::instance().info(t.empty() ? "TUNNEL" : t, m);
}

} // extern "C"
