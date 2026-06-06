## 项目结构说明

### Android 端（scrcpy）
- NDK 代码根目录：`server/src/main/cpp/`
- 子模块/依赖：
  - `dep_message/QtMirrorProto/` —— 与 PC 端共享的 proto 仓库
  - `deps/3rdparty/asio2/`、`deps/3rdparty/EventBus/` —— 第三方库
  - `deps/cpp_base_lib/` —— PC 端基础库（含 `yk_logger.h`，Windows 专用）

### PC 端（QtScrcpy）
- 路径：`G:/code/self/MivoxQtMirror/QtScrcpy/QtScrcpy/`
- 同样使用 `dep_message/QtMirrorProto/` 存放 proto 仓库

## 跨平台注意事项

- `msg_answer_cbk.cpp` 等共享文件需要在 `#ifdef __ANDROID__` 下做平台适配
- Android 端不使用 `yk_logger.h`（依赖 spdlog + Windows API），改用 `__android_log_print`
- 日志宏 `YK_LOGI(...)` 在 Android 下通过 `std::format` 格式化后传入 `__android_log_print`，保持 `{}` 占位符语法与 Windows 端一致
