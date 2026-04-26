#ifndef TEST_NATIVE_H
#define TEST_NATIVE_H

#include <string>

namespace scrcpy {

class TestNative {
public:
    static std::string Echo(const std::string &input);
    static std::string GetVersion();
    static int Add(int a, int b);

    // Simulates a processing pipeline: converts string to uppercase
    static std::string Process(const std::string &input);
};

}  // namespace scrcpy

#endif  // TEST_NATIVE_H
