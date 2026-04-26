#include "jni_helper.h"

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
