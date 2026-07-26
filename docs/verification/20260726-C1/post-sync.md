# C1 官方同步与相关提交回填验证

- 日期：2026-07-26
- 工作分支：`feature/malos/basic_version`
- 已验证源码提交：`988a209dca0e49a6e5205b5a6ef0de5df7f9a1c7`
- 本地 `main`：`b8f2cf4b431df7c1669ec926a5ea8b9fc146f310`
- 官方 `upstream/main`：`b8f2cf4b431df7c1669ec926a5ea8b9fc146f310`
- 同步前 `origin/main`：`a02ba9d82b94c217cfad93f6cfbf6ba131db85e4`
- 结论：本地 `main` 已快进 59 个官方提交；相关构建、依赖、输入提交已回填
  `basic_version`；macOS 配置、编译和 GUI 窗口创建成功；Android
  `assembleDebug` 成功。

## 分支同步

执行：

```sh
git fetch upstream main
git branch --track main upstream/main
git rev-list --left-right --count main...upstream/main
git rev-list --left-right --count origin/main...main
```

同步后、推送前结果：

```text
main...upstream/main = 0 0
origin/main...main   = 0 59
```

这表示本地 `main` 与官方完全一致，并且相对内部镜像 `origin/main` 领先 59 个
提交。按用户要求，本次没有把 59 个功能提交整批合入 Android 工作分支，而是
回填阶段一直接依赖的 15 个构建、依赖、输入与 CI 提交。

## 回填提交

按官方提交顺序 cherry-pick；右列是解决 Android fork 冲突后的本地提交：

| 官方提交 | 本地提交 | 主题 |
| --- | --- | --- |
| `aa6e2c05` | `448a674a` | macOS Homebrew 构建依赖与 bundle 说明 |
| `f8fb588b` | `b9fe9404` | macOS CMake 修复 |
| `6f6c1299` | `aaa5d7e4` | logging 编译期格式检查 |
| `1c2b7d78` | `764b7482` | `ENABLE_LIBUSB` |
| `0d832c48` | `78b71ebd` | macOS CI fail-fast |
| `f3fecba3` | `5da9c27c` | SDL2 迁移 SDL3 |
| `0fc74035` | `8fbcd482` | `ENABLE_OPENGL` / `ENABLE_VULKAN` |
| `8e3e961b` | `4b49292b` | SDL 可选构建 |
| `ad73c1e0` | `9b1d58a7` | 输入映射并发访问修复 |
| `251662aa` | `8ae5d1d5` | vcpkg 与 SDL 3.4.10 |
| `b517fbc1` | `913e2bf4` | fmt 12.1 |
| `0e7e9ee6` | `29b7aa36` | macOS Homebrew CI 清理 |
| `145ad1e9` | `e1417029` | wxWidgets 3.3.3 |
| `acb105bd` | `9a1e73cf` | 提前链接 SDL3 |
| `f82598e4` | `0ba3dc8c` | 修复 `CMAKE_CXX_FLAGS` |

冲突解决后另有三个本地整理/兼容提交：

- `4fd4a746 build: clean up upstream cherry-pick whitespace`
- `d17d3bf6 build: fix SDL3 dependency integration`
- `988a209d mac: fix CPU brand name assignment`

`d17d3bf6` 移除迁移 SDL3 后残留的独立 `hidapi` 依赖和链接，并恢复
Android fork 原有的整文件 `HAS_SDL` 条件保护。Wiimote HID 调用现由 SDL3
提供；Android 构建同时显式关闭 SDL 和 HIDAPI。

`988a209d` 修复同步前基线发现的问题：Android 分支已经把
`m_cpuBrandName` 改为 `std::string`，macOS CPU 名代码却仍按字符数组调用
`strncpy`。修复后按实际字段类型直接赋值。

## 子模块与静态检查

执行：

```sh
git submodule sync --recursive
git submodule update --init --recursive
git submodule status --recursive
git diff --check
ruby -e 'require "yaml"; YAML.load_file(".github/workflows/build.yml"); puts "yaml ok"'
```

结果：

- `.gitmodules` 中全部子模块 URL 均为 `git@github.com:tencentmalos/...`。
- 所有递归子模块均已初始化，状态无 `-` 或 `+` 前缀。
- `dependencies/vcpkg` 指针为
  `9849e070ad4c3383890fb1c80ae0c54325e2484a`。
- `dependencies/foundation` 指针保持
  `b01f41c`，未修改子模块内部内容。
- `git diff --check` 通过，GitHub Actions YAML 可解析。

## macOS 配置

最终执行：

```sh
/usr/bin/time -p \
  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -G Ninja
```

结果：

- 成功，退出码 0。
- 配置 `6.1s`、生成 `0.1s`，wall time `6.21s`。
- 成功找到 `/opt/homebrew/lib/libMoltenVK.dylib`。
- 完整成功输出：`post-sync-configure.log`。
- 首轮新 vcpkg 基线安装 105 个包后，生成阶段发现残留
  `hidapi::hidapi` 目标；修复记录和完整输出在
  `post-sync-configure-attempt1.log`。

## macOS 编译

最终执行：

```sh
/usr/bin/time -p cmake --build build -- -d stats
```

结果：

- 成功，退出码 0，生成 `bin/Cemu_relwithdebinfo`。
- 初始构建图为 557 条 Ninja build edges。
- 最终增量成功轮完成 402 条命令，wall time `56.57s`。
- Mach-O 产物为 arm64，大小约 31 MiB。
- SHA-256：
  `0a8ea842eb12a703dfb8e3e8608009db28f96316360c7198b4777b7696028d48`。
- 完整成功输出：`post-sync-build.log`。

同步后的首轮构建仍复现基线 `cpu_features.cpp` 错误，修复后第二轮暴露
SDL 条件保护冲突；两轮完整失败输出分别归档为
`post-sync-build-attempt1.log` 和 `post-sync-build-attempt2.log`。最终轮
成功说明两个错误均已实际越过，而不是根据源码推断。

## macOS GUI 启动

执行：

```sh
cd bin
./Cemu_relwithdebinfo
```

启动 10 秒后，进程检查返回：

```text
29361 ./Cemu_relwithdebinfo
```

通过 CoreGraphics 的窗口清单按 PID 查询，返回：

```text
owner=Cemu_relwithdebinfo title= bounds={
    Height = 455;
    Width = 960;
    X = 504;
    Y = 283;
}
```

因此已验证进程未立即崩溃，并且创建了一个在屏幕上的 960×455 GUI 窗口。
验证后由测试命令发送 Ctrl-C 主动结束进程。

截图没有伪造：当前终端没有 macOS 屏幕录制和辅助访问权限，
`screencapture` 返回 `could not create image from display`，
`System Events` 返回“不允许辅助访问”。本次以进程存活和 CoreGraphics
窗口元数据作为可复现替代证据；若需要满足截图退出项，须在系统设置授权后
重跑 `screencapture`。

## Android 不回归

执行：

```sh
cd src/android
/usr/bin/time -p ./gradlew assembleDebug
```

结果：

- 成功，`BUILD SUCCESSFUL in 8m 53s`。
- 41 个任务：19 executed、22 up-to-date。
- arm64-v8a native CMake 配置和编译均实际执行。
- 产物：`src/android/app/build/outputs/apk/debug/app-debug.apk`，约 86 MiB。
- SHA-256：
  `b72672e7e9c23f749280e233a46ca3db5055bd44243cebb471920fd043870068`。
- 完整输出：`android-assemble-debug.log`。

Gradle 报告本机 NDK 28 package id 路径不一致，但本次实际使用 NDK
29.0.14206865，属于环境警告，没有阻止配置、native 编译或 APK 打包。

## 与 C1-T0 基线对照

| 检查项 | 同步前基线 | 同步与兼容修复后 |
| --- | --- | --- |
| macOS CMake | 成功，610.9s（首次装依赖） | 成功，6.21s（复用缓存） |
| macOS 编译 | 失败于 `cpu_features.cpp` | 成功，生成 arm64 Mach-O |
| GUI | 未达到 | 进程存活并创建可见窗口 |
| Android | 未执行 | `assembleDebug` 成功 |

## C1 当前退出标准

- [x] 本地 `main` 与官方 `upstream/main` 完全一致。
- [x] 阶段一相关提交已按顺序回填 `basic_version`。
- [x] macOS 配置成功。
- [x] macOS 编译成功。
- [x] GUI 进程和窗口创建已实际验证。
- [x] Android `assembleDebug` 通过。
- [x] 完整配置、编译和 Android 输出已归档。
- [ ] GUI 截图：受当前 macOS 权限阻止，已记录命令、错误和替代验证。
