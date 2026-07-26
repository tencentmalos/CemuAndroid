---
name: cemu-android-build-validation
description: Use when validating Cemu Android repository changes through submodule sync, CMake builds, Android Gradle builds, unit tests, instrumentation tests, or branch-ready verification.
---

# Cemu Android Build Validation

用于承载 `AGENTS.md` 中不适合展开的构建与验证流程。默认串行执行，便于用户观察每一步命令输出和失败位置。

## 基线检查

从仓库根目录开始：

```sh
git status --short --branch
git submodule update --init --recursive
git submodule status --recursive
```

如果 `.gitmodules` 有变更，构建前先同步本地子模块配置：

```sh
git submodule sync --recursive
git submodule update --init --recursive
```

本分支的子模块 URL 应使用 `git@github.com:tencentmalos/...`。

## 桌面端 CMake 构建

默认 Release 构建：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=release -G Ninja
cmake --build build
```

Debug 构建：

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=debug -G Ninja
cmake --build build-debug
```

显式指定编译器时遵循 `BUILD.md`，优先使用 Clang 15+ 或支持 C++20 的 GCC 工具链。

## ARM 构建脚本

Debian ARM64 toolchain 路径使用仓库脚本：

```sh
BUILD_TYPE=release ./build_arm.sh
```

该脚本会配置 `build_arm`，关闭 vcpkg 和若干桌面端特性，并用 Ninja 构建。失败时在结果中保留第一个 CMake 配置错误和失败的编译命令。

## Android Gradle 构建与测试

Android 工程根目录是 `src/android`：

```sh
cd src/android
./gradlew assembleDebug
./gradlew testDebugUnitTest
./gradlew connectedDebugAndroidTest
```

当前 `src/android/app/build.gradle.kts` 配置为 `arm64-v8a`。除非任务明确要求，不要扩大 ABI 范围。

Release 签名通过环境变量提供：

```sh
ANDROID_STORE_FILE=...
ANDROID_KEY_STORE_PASSWORD=...
ANDROID_KEY_ALIAS=...
```

不得提交 keystore 或本地签名文件。

## 验证选择

- 仅文档变更：检查文件内容、路径链接和 `git diff`。
- CMake 或 Native 源码变更：至少运行相关 CMake 配置/构建。
- Android Kotlin、Gradle、资源、JNI 或打包变更：运行 `assembleDebug` 和最接近的测试任务。
- 子模块变更：检查 `.gitmodules`、`git submodule sync --recursive`，可行时验证递归 clone/update。

如果工具链、SDK、设备或依赖缺失导致命令无法运行，明确报告阻塞原因，并说明已经完成的最强替代检查。
