# 项目规则

## 换行符规则（必须遵守）

**修改任何文件时，严禁改变原文件的换行符格式（LF / CRLF）。**

### 原因
项目使用 TortoiseGit 管理版本，换行符变化会导致 git diff 显示满屏改动（所有行都被标记为修改），严重影响代码审查。

### 操作规范
1. **修改前**：先用 Python 检测文件当前换行符类型
   ```python
   with open('file', 'rb') as f:
       data = f.read()
   crlf = data.count(b'\r\n')
   lf = data.count(b'\n') - crlf
   ```
2. **修改后**：立即验证换行符是否被改变
3. **修复**：若 `StrReplaceFile` / `WriteFile` 等工具意外将 CRLF 改为 LF（或反之），用 Python 恢复：
   ```python
   # 统一为目标换行符（例如 CRLF）
   data = data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
   ```
4. **验证**：最终 `git diff` 应只显示功能性改动，不能有因换行符导致的满屏变化

### 项目环境
- 操作系统：Windows
- Git 配置：`core.autocrlf=true`（工作目录文件通常为 CRLF，仓库 blob 为 LF）
- IDE：Android Studio / VS Code

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
