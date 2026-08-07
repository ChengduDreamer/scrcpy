#ifndef JNI_HELPER_H
#define JNI_HELPER_H

#include <jni.h>

#include <cstdint>
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

// Cache the MediaScanner class and method (call once from JNI_OnLoad, where the
// calling thread's class loader sees the server classes).
void CacheMediaScannerJni(JNIEnv *env);

// Ask Android to media-scan the given files (batched by the caller).
// Never throws; no-op when the Java class could not be cached.
void ScanMediaFiles(const std::vector<std::string> &paths);

#endif  // JNI_HELPER_H
