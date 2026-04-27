package com.genymobile.scrcpy;

/**
 * JNI bridge into native C++ code (libscrcpy_native.so).
 * Test methods for verifying NDK/CMake integration.
 */
public final class NativeBridge {

    static {
        //System.loadLibrary("scrcpy_native");

        System.load("/data/local/tmp/libscrcpy_native.so");
    }

    // Echo the input string back
    public static native String echo(String input);

    // Return the native library version
    public static native String getVersion();

    // Add two integers (native side)
    public static native int add(int a, int b);

    // Process string: convert to uppercase
    public static native String process(String input);

    // Start the test HTTP server on given port
    public static native boolean startHttpServer(int port);

    // Stop the test HTTP server
    public static native void stopHttpServer();

    // Check if HTTP server is running
    public static native boolean isHttpServerRunning();

    private NativeBridge() {}
}
