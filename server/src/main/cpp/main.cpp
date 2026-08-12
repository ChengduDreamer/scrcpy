#include <android/log.h>
#include <jni.h>

#include "http_server.h"
#include "jni_helper.h"
#include "poco_websocket_server.h"
#include "test_native.h"
#include "file_transfer_plugin.h"
#include "mirror_message.pb.h"
#include "notification/notification_relay.h"

#define TAG "scrcpy-native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

JavaVM *g_jvm = nullptr;

static scrcpy::HttpServer g_httpServer;
static scrcpy::PocoWebsocketServer g_wsServer;
static scrcpy::NotificationRelay g_notificationRelay;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void * /* reserved */) {
    g_jvm = vm;
    LOGI("JNI_OnLoad: scrcpy_native library loaded");

    // Cache the MediaScanner bridge here: at load time the calling Java
    // thread's class loader sees the server classes.
    if (JNIEnv *env = GetJniEnv()) {
        CacheMediaScannerJni(env);
    }

    try {
        auto* plugin = static_cast<tc::FileTransferPlugin*>(GetInstance());
        plugin->Create([](const std::string& stream_id,
                          const std::vector<uint8_t>& data) -> bool {
            (void)stream_id;
            if (!g_wsServer.HasConnection()) {
                LOGE("SendProtoMessage: no WebSocket connection");
                return false;
            }
            return g_wsServer.SendBinary(data.data(), data.size());
        });
    } catch (const std::exception& e) {
        LOGE("JNI_OnLoad: plugin->Create failed: %s", e.what());
        return JNI_ERR;
    } catch (...) {
        LOGE("JNI_OnLoad: plugin->Create failed: unknown exception");
        return JNI_ERR;
    }

    // Capture plugin pointer to avoid repeated GetInstance() calls
    auto* plugin = static_cast<tc::FileTransferPlugin*>(GetInstance());
    g_wsServer.SetOnBinaryMessage([plugin](const uint8_t* data, size_t len) {
        auto msg = std::make_shared<tc::Message>();
        if (!msg->ParseFromArray(data, static_cast<int>(len))) {
            LOGE("Failed to parse proto message");
            return;
        }
        const auto type = msg->type();

        // Application-layer heartbeat. Avoids POCO WebSocket PING/PONG which has
        // been observed to put some clients into a tight receive-loop spin.
        if (type == tc::kHeartBeat) {
            auto resp = std::make_shared<tc::Message>();
            resp->set_type(tc::kOnHeartBeat);
            resp->set_stream_id(msg->stream_id());
            if (msg->has_heartbeat()) {
                auto* on = resp->mutable_on_heartbeat();
                on->set_index(msg->heartbeat().index());
            }
            plugin->SendProtoMessage(msg->stream_id(), resp);
            return;
        }

        // File transfer messages: enum 260-320 (// file transfer begin ~ end)
        if (type >= tc::kFileOperationEvent && type <= tc::kFileTransSaveFileException) {
            plugin->OnMessage(msg);
        } else if (type == tc::kNotificationControl) {
            // PC -> agent APK: forward the raw envelope bytes over the relay
            // IPC; the relay re-frames them (4B BE length) for the APK.
            g_notificationRelay.SendControl(data, len);
        } else {
            LOGI("Ignored non-file-transfer message type: %d", static_cast<int>(type));
        }
    });

    g_wsServer.SetOnConnectionStateChanged([plugin](bool connected) {
        if (!connected) {
            LOGI("WebSocket disconnected, cancelling file transfers");
            plugin->OnConnectionLost();
        } else {
            LOGI("WebSocket connected");
        }
    });

    // Notification relay: forward verified agent frames to the PC over the
    // existing WebSocket channel. No WS connection -> drop (relay logs it,
    // throttled).
    g_notificationRelay.SetSendToPcHandler([](const uint8_t* data, size_t len) {
        if (!g_wsServer.HasConnection()) {
            return false;
        }
        return g_wsServer.SendBinary(data, len);
    });

    return JNI_VERSION_1_6;
}

// ---------------------------------------------------------------------------
// com.genymobile.scrcpy.NativeBridge
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jstring JNICALL
Java_com_genymobile_scrcpy_NativeBridge_echo(JNIEnv *env, jclass /* clazz */,
                                              jstring input) {
    std::string s = JniHelper::AsString(env, input);
    std::string result = scrcpy::TestNative::Echo(s);
    LOGI("echo: '%s' -> '%s'", s.c_str(), result.c_str());
    return JniHelper::FromString(env, result);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_genymobile_scrcpy_NativeBridge_getVersion(JNIEnv *env,
                                                    jclass /* clazz */) {
    std::string version = scrcpy::TestNative::GetVersion();
    LOGI("getVersion: %s", version.c_str());
    return JniHelper::FromString(env, version);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_genymobile_scrcpy_NativeBridge_add(JNIEnv * /* env */,
                                             jclass /* clazz */, jint a,
                                             jint b) {
    int result = scrcpy::TestNative::Add(a, b);
    LOGI("add: %d + %d = %d", a, b, result);
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_genymobile_scrcpy_NativeBridge_process(JNIEnv *env,
                                                 jclass /* clazz */,
                                                 jstring input) {
    std::string s = JniHelper::AsString(env, input);
    std::string result = scrcpy::TestNative::Process(s);
    LOGI("process: '%s' -> '%s'", s.c_str(), result.c_str());
    return JniHelper::FromString(env, result);
}

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jboolean JNICALL
Java_com_genymobile_scrcpy_NativeBridge_startHttpServer(JNIEnv * /*env*/,
                                                         jclass /*clazz*/,
                                                         jint port) {
    bool ok = g_httpServer.Start(port);
    LOGI("startHttpServer(%d): %s", port, ok ? "started" : "failed");
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_genymobile_scrcpy_NativeBridge_stopHttpServer(JNIEnv * /*env*/,
                                                        jclass /*clazz*/) {
    LOGI("stopHttpServer");
    g_httpServer.Stop();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_genymobile_scrcpy_NativeBridge_isHttpServerRunning(
    JNIEnv * /*env*/, jclass /*clazz*/) {
    return g_httpServer.IsRunning() ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------------------
// Poco WebSocket server
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jboolean JNICALL
Java_com_genymobile_scrcpy_NativeBridge_startWebSocketServer(
    JNIEnv * /*env*/, jclass /*clazz*/, jint port) {
    bool ok = g_wsServer.Start(port);
    LOGI("===>0 startWebSocketServer(%d): %s", port, ok ? "started" : "failed");
    // Notification relay shares the WebSocket server's lifecycle (Server.java
    // starts/stops both through this single JNI pair). A relay failure must
    // not break mirroring, so it is logged but not propagated.
    bool relayOk = g_notificationRelay.Start();
    LOGI("startNotificationRelay: %s", relayOk ? "started" : "failed");
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_genymobile_scrcpy_NativeBridge_stopWebSocketServer(
    JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("stopWebSocketServer");
    g_notificationRelay.Stop();
    g_wsServer.Stop();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_genymobile_scrcpy_NativeBridge_isWebSocketServerRunning(
    JNIEnv * /*env*/, jclass /*clazz*/) {
    return g_wsServer.IsRunning() ? JNI_TRUE : JNI_FALSE;
}
