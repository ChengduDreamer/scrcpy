#include "jni_helper.h"

#include <android/log.h>

#define TAG "scrcpy-native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern JavaVM *g_jvm;

std::string JniHelper::AsString(JNIEnv *env, jstring js) {
    if (!js) return {};
    const char *cstr = env->GetStringUTFChars(js, nullptr);
    std::string result(cstr);
    env->ReleaseStringUTFChars(js, cstr);
    return result;
}

jstring JniHelper::FromString(JNIEnv *env, const std::string &s) {
    return env->NewStringUTF(s.c_str());
}

std::vector<uint8_t> JniHelper::ByteArrayToVector(JNIEnv *env,
                                                    jbyteArray buffer) {
    if (!buffer) return {};
    jsize len = env->GetArrayLength(buffer);
    std::vector<uint8_t> result(len);
    env->GetByteArrayRegion(buffer, 0, len,
                            reinterpret_cast<jbyte *>(result.data()));
    return result;
}

JNIEnv *GetJniEnv() {
    JNIEnv *env = nullptr;
    jint ret = g_jvm->GetEnv(reinterpret_cast<void **>(&env),
                              JNI_VERSION_1_6);
    if (ret == JNI_OK) return env;
    if (ret == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        return env;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// MediaScanner bridge (native -> Java)
// ---------------------------------------------------------------------------

static jclass g_media_scanner_class = nullptr;
static jmethodID g_scan_files_method = nullptr;

void CacheMediaScannerJni(JNIEnv *env) {
    jclass cls = env->FindClass("com/genymobile/scrcpy/util/MediaScanner");
    if (!cls) {
        env->ExceptionClear();
        LOGE("CacheMediaScannerJni: MediaScanner class not found");
        return;
    }
    g_media_scanner_class = static_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);
    g_scan_files_method =
        env->GetStaticMethodID(g_media_scanner_class, "scanFiles",
                               "([Ljava/lang/String;)V");
    if (!g_scan_files_method) {
        env->ExceptionClear();
        env->DeleteGlobalRef(g_media_scanner_class);
        g_media_scanner_class = nullptr;
        LOGE("CacheMediaScannerJni: scanFiles method not found");
        return;
    }
    LOGI("CacheMediaScannerJni: MediaScanner.scanFiles cached");
}

void ScanMediaFiles(const std::vector<std::string> &paths) {
    if (paths.empty() || !g_media_scanner_class || !g_scan_files_method) {
        return;
    }
    JNIEnv *env = GetJniEnv();
    if (!env) {
        LOGE("ScanMediaFiles: no JNIEnv");
        return;
    }
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray jpaths = env->NewObjectArray(static_cast<jsize>(paths.size()),
                                              string_class, nullptr);
    env->DeleteLocalRef(string_class);
    if (!jpaths) {
        env->ExceptionClear();
        LOGE("ScanMediaFiles: NewObjectArray failed");
        return;
    }
    for (size_t i = 0; i < paths.size(); ++i) {
        jstring jpath = env->NewStringUTF(paths[i].c_str());
        env->SetObjectArrayElement(jpaths, static_cast<jsize>(i), jpath);
        env->DeleteLocalRef(jpath);
    }
    env->CallStaticVoidMethod(g_media_scanner_class, g_scan_files_method,
                              jpaths);
    env->DeleteLocalRef(jpaths);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("ScanMediaFiles: scanFiles threw");
    }
}
