#ifndef JNI_HELPER_H
#define JNI_HELPER_H

#include <jni.h>
#include <string>
#include <vector>

class JniHelper {
public:
    static std::string AsString(JNIEnv *env, jstring js);
    static jstring FromString(JNIEnv *env, const std::string &s);
    static std::vector<uint8_t> ByteArrayToVector(JNIEnv *env,
                                                   jbyteArray buffer);
};

// Get JNIEnv from any thread
JNIEnv *GetJniEnv();

#endif  // JNI_HELPER_H
