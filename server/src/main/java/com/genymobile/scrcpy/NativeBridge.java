package com.genymobile.scrcpy;

/**
 * JNI bridge into native C++ code (libscrcpy_native.so).
 * Test methods for verifying NDK/CMake integration.
 */
public final class NativeBridge {

    static {
        System.loadLibrary("scrcpy_native");
    }

    // Echo the input string back
    public static native String echo(String input);

    // Return the native library version
    public static native String getVersion();

    // Add two integers (native side)
    public static native int add(int a, int b);

    // Process string: convert to uppercase
    public static native String process(String input);

    private NativeBridge() {}
}
