#include "test_native.h"

#include <algorithm>
#include <cctype>

namespace scrcpy {

std::string TestNative::Echo(const std::string &input) { return input; }

std::string TestNative::GetVersion() { return "scrcpy-native 0.1.0"; }

int TestNative::Add(int a, int b) { return a + b; }

std::string TestNative::Process(const std::string &input) {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

}  // namespace scrcpy
