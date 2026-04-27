#include <android/log.h>
#include <jni.h>

#include "http_server.h"
#include "jni_helper.h"
#include "test_native.h"

#define TAG "scrcpy-native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

JavaVM *g_jvm = nullptr;

static scrcpy::HttpServer g_httpServer;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void * /* reserved */) {
    g_jvm = vm;
    LOGI("JNI_OnLoad: scrcpy_native library loaded");
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
