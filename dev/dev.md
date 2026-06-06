

start server...
AdbProcessImpl::out:../../../QtScrcpy/QtScrcpyCore/src/third_party/scrcpy-server: 1 file pushed, 0 skipped. 213.8 MB/s (656155 bytes in 0.003s)


start server params: serial= "emulator-5554" server_local= "../../../QtScrcpy/QtScrcpyCore/src/third_party/scrcpy-server" server_remote= "/data/local/tmp/scrcpy-server.jar" server_version= "3.3.4"


AdbProcessImpl::out:[server] INFO: Device: [OPPO] OPPO PHY110 (Android 12)


ReadDeviceInfo timeout


update devices...
adb run
AdbProcessImpl::out:[server] DEBUG: Controller stopped
[server] DEBUG: Device message sender stopped
[server] DEBUG: Using video encoder: 'c2.android.avc.encoder'
[server] DEBUG: Screen streaming stopped


AdbProcessImpl::error:[server] ERROR: dlopen failed: "/data/local/tmp/libscrcpy_native.so" is for EM_AARCH64 (183) instead of EM_X86_64 (62)
java.lang.UnsatisfiedLinkError: dlopen failed: "/data/local/tmp/libscrcpy_native.so" is for EM_AARCH64 (183) instead of EM_X86_64 (62)
	at java.lang.Runtime.load0(Runtime.java:936)
	at java.lang.System.load(System.java:1620)
	at com.genymobile.scrcpy.NativeBridge.<clinit>(NativeBridge.java:12)
	at com.genymobile.scrcpy.NativeBridge.startHttpServer(Native Method)
	at com.genymobile.scrcpy.Server.scrcpy(Server.java:169)
	at com.genymobile.scrcpy.Server.internalMain(Server.java:278)
	at com.genymobile.scrcpy.Server.main(Server.java:222)
	at com.android.internal.os.RuntimeInit.nativeFinishInit(Native Method)
	at com.android.internal.os.RuntimeInit.main(RuntimeInit.java:378)


qprocess start error:../../../QtScrcpy/QtScrcpyCore/src/third_party/adb/win/adb.exe -s emulator-5554 reverse --remove localabstract:scrcpy_00001da7


AdbProcessImpl::out:List of devices attached
emulator-5554	device

这个错误是架构错误， 因为我用的是模拟器测试的，所以要用x86-64平台的so文件




adb logcat -s scrcpy-poco-ws:V

adb logcat | findstr scrcpy



