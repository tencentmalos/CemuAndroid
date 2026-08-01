---
name: cemu-android-analysis
description: Use when analyzing Cemu Android build failures, adb runs, logcat output, native crashes, tombstones, JNI issues, Gradle failures, or Android emulator/device behavior.
---

# Cemu Android Analysis

用于 Android 侧调试和验证。优先基于 Gradle、adb、logcat 和 native 符号信息形成结论，避免只凭猜测修复。

## 项目事实

- Android project: `src/android`
- App namespace/application id: `info.cemu.cemu`
- 可运行验证包名：`info.cemu.cemu`
- Gradle `relWithDebInfo` variant：APK 可调试/run-as，Native CMake 类型为 `RelWithDebInfo`
- Debug 包名后缀：`info.cemu.cemu.debug`，仅用于 debug-only 测试任务
- Native CMake 入口：仓库根目录 `CMakeLists.txt`
- Android Native 入口目录：`src/android/app/src/main/cpp`
- 当前 Gradle NDK 版本：`29.0.14206865`
- 当前配置 ABI：`arm64-v8a`

## 构建与安装

从仓库根目录开始：

```sh
cd src/android
./gradlew assembleRelWithDebInfo
./gradlew installRelWithDebInfo
```

真机功能、性能、游戏启动和 native 崩溃复现统一使用 release APK；该 APK 内的
Native 库由 `RelWithDebInfo` 构建。不要用 debug APK 形成性能结论。

单元测试：

```sh
./gradlew testDebugUnitTest
```

设备或模拟器测试：

```sh
./gradlew connectedDebugAndroidTest
```

## Logcat 流程

复现前尽量先清空日志：

```sh
adb logcat -c
```

运行 app 或安装/构建命令后，收集聚焦日志：

```sh
adb logcat -d | rg -i 'cemu|info.cemu|fatal|crash|tombstone|signal|vulkan|jni|exception'
```

如果 `rg` 不可用，用 `grep -Ei` 和相同关键词替代。保留第一个 fatal signal、Java exception 或 JNI abort，并带足够上下文定位来源。

## App 与设备检查

检查连接设备：

```sh
adb devices
```

检查用于验证的 app 是否存活：

```sh
adb shell pidof info.cemu.cemu
```

显式启动验证包：

```sh
adb shell monkey -p info.cemu.cemu 1
```

调试生命周期问题时，记录 app 是崩溃、停留后台，还是进入了预期 Activity。

## Native 崩溃符号化

查找未裁剪符号的 Android native 库：

```sh
find src/android/app/build/intermediates/cxx -path '*arm64-v8a/*.so' -print
```

查找 NDK 符号化工具：

```sh
find "$HOME/Library/Android/sdk/ndk" -name llvm-addr2line -print
```

符号化 PC 地址：

```sh
/path/to/llvm-addr2line -f -C -e path/to/unstripped.so 0xADDR
```

符号化前必须根据 tombstone 或 logcat backtrace 确认地址属于哪个 `.so`。

## 规则

- 未复现或未与失败关联前，不要把 warning 当作根因。
- 修复前先区分 Java/Kotlin 异常、JNI abort、native signal、Vulkan/设备错误和 Gradle 配置失败。
- 一次只修一个根因，避免把崩溃、构建和格式化变更混在一起。
- 已知相关路径后，优先使用仓库 Gradle/CMake 流程，而不是临时拼接命令。
