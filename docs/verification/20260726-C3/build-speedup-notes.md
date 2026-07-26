# C3 编译加速验证记录

- 日期：2026-07-26
- 工作分支：`feature/malos/basic_version`
- 起点提交：`461fba4f`（共享依赖移出 vcpkg）
- 构建类型：macOS arm64 Ninja `RelWithDebInfo`

## T0：依赖收敛后的起点基线

阶段边界检查：

```text
upstream/main...origin/main = 0  0
main = upstream/main = origin/main = b8f2cf4b
main 是 feature/malos/basic_version 的祖先
```

执行：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target clean
/usr/bin/time -p cmake --build build -- -d stats
```

结果：

- `FinishCommand = 648`
- `real 77.38s`、`user 749.04s`、`sys 69.34s`
- Ninja 编译与链接规则包含 `-flto=thin`
- 最终链接 `bin/Cemu_relwithdebinfo`：18,649ms

`.ninja_log` 聚合耗时最高的组为 CemuCafe（295 条）、wx GUI（57 条）、
CemuInput（24 条）和 glslang（43 条）。聚合耗时受并行执行影响，只用于
确定后续 Unity Build 候选，不等同于 wall time。

该基线比 C2 的 559 条命令多，主要因为 fmt/glslang/zstd/libusb 已从预构建
vcpkg 包改为仓库内源码 target。这是依赖收敛的已知 clean-build 成本，不能
沿用 C2 的旧边数评价后续加速。

## T1：非 Release 构建关闭 LTO

实现：

- 增加可覆盖的 `ENABLE_LTO` CMake 选项。
- Release 默认 ON；RelWithDebInfo/Debug 默认 OFF。
- Android Gradle 显式传入 `-DENABLE_LTO=OFF`。

### RelWithDebInfo 对比

使用同一 `build` 目录重新配置、clean，并清空本轮 `.ninja_log` 后构建：

| 指标 | T0：LTO ON | T1：LTO OFF | 变化 |
| --- | ---: | ---: | ---: |
| Ninja `FinishCommand` | 648 | 648 | 0 |
| wall time | 77.38s | 69.72s | -7.66s（-9.9%，仅参考） |
| user time | 749.04s | 711.39s | -37.65s |
| 最终链接 | 18.649s | 0.505s | -18.144s（-97.3%） |

T1 的 `build.ninja` 中 `-flto` / `thinlto` / `object_path_lto` 命中数为 0。
边数不变，说明收益来自关闭 LTO，不是裁掉目标。

产物：

```text
bin/Cemu_relwithdebinfo
39,322,736 bytes
SHA-256 04263e73dd5070ab07122da2aacdcb8eae6d32f64763a47eaa095fdb9710c44e
```

GUI smoke test 中进程保持运行 8 秒，再由验证脚本主动终止。

### Release 护栏

在全新的 `/tmp/cemu-c3-release-461fba4f` 构建目录执行：

```sh
cmake -S . -B /tmp/cemu-c3-release-461fba4f -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/cemu-c3-release-461fba4f -- -d stats
```

结果：

- CMake cache：`ENABLE_LTO:BOOL=ON`
- Ninja 规则存在 `-flto=thin`
- `FinishCommand = 648`
- 全量构建成功；`real 63.54s`、`user 612.20s`、`sys 57.06s`
- `bin/Cemu_release`：31,406,464 bytes，SHA-256
  `61f30fce1cc6b351c085c9fe54659774a20c74b703ad6be212080287a419ee92`

Release 使用独立的新构建目录，时间不用于和 T0/T1 宣称性能差异；它只验证
Release 的默认 LTO 护栏和可构建性。

### Android 与测试

执行：

```sh
cd src/android
./gradlew assembleDebug
./gradlew testDebugUnitTest
ctest --test-dir ../../build --output-on-failure
```

结果：

- Android CMake cache：`ENABLE_LTO:BOOL=OFF`
- `assembleDebug`：`BUILD SUCCESSFUL in 1m 55s`，41 个任务中 19 executed
- `testDebugUnitTest`：`BUILD SUCCESSFUL in 3s`，28 个任务中 4 executed
- 桌面 `ctest` 返回成功，但当前构建图报告 `No tests were found`

构建继续包含已有的 WebP deployment target、Cemu 类型转换和第三方代码
warning，没有新增错误。

## T1 退出标准

- [x] 记录 RelWithDebInfo 的边数、wall time 和最终链接时间对比。
- [x] RelWithDebInfo 不含 LTO 标记，边数保持不变。
- [x] 全新 Release 构建默认开启 LTO且构建通过。
- [x] Android 显式关闭 LTO，`assembleDebug` 通过。
- [x] Android 单测和 macOS GUI smoke test 通过。

## T3：全局 Unity Build

> 按维护者指示，Unity Build 先于尚未实施的 C3-T2 ccache 落地。

实现：

- 增加全局 `CEMU_USE_UNITY_BUILD`，默认 ON，并用它初始化
  `CMAKE_UNITY_BUILD`。
- 将 foundation 遗留的 `CITRA_USE_UNITY_BUILD` 强制同步为同一值，避免
  Cemu 与 foundation 静默使用不同模式。
- `CemuCafe` 使用 batch size 8；由实际编译错误逐项收敛源文件和目标排除
  清单。
- 为 10 个原本缺少完整重复包含保护的头文件补充 `#pragma once`。

### 排除清单

整目标关闭 Unity：

| 目标 | 原因 |
| --- | --- |
| `CemuWxGui` | 多个窗口实现重复使用 TU-local enum、列名和输入面板常量 |
| `cubeb` | 各平台后端分别定义私有 `cubeb` / `cubeb_stream` 结构；Android 实测冲突 |
| `glslang` | `SpvBuilder` 等内部实现依赖独立编译单元和声明顺序 |
| `ih264d` | C 源文件之间存在内部宏/枚举串扰 |
| `imguiImpl` | Dear ImGui 各源文件依赖 `IMGUI_DEFINE_MATH_OPERATORS` 的独立 include 顺序 |
| `libzstd_static` | dictBuilder 内部 `cover.h` 不支持同一 TU 重复包含 |

只排除单个源文件：

| 目标 | 源文件 | 原因 |
| --- | --- | --- |
| `CemuAudio` | `CubebInputAPI.cpp` | 与 `CubebAPI.cpp` 都定义 TU-local `state_cb` |
| `CemuCafe` | `Account/Account.cpp` | 与 `CafeSystem.cpp` 定义同名本地 FFL 兼容类型 |
| `CemuCafe` | `MetalCppImpl.cpp` | metal-cpp 实现宏必须在任何 Metal 头之前求值 |
| `CemuCafe` | 6 个 `BackendX64*.cpp` | JIT 后端含同名 TU-local emit helper |
| `CemuCafe` | `BackendAArch64.cpp` | 架构相关 JIT 后端保持独立，Android 已验证 |

头文件保护补齐：

```text
src/Cafe/Filesystem/fscDeviceHostFS.h
src/Cafe/HW/Espresso/Interpreter/PPCInterpreterHelper.h
src/Cafe/HW/Espresso/Recompiler/BackendX64/BackendX64.h
src/Cafe/HW/Espresso/Recompiler/PPCRecompilerIml.h
src/Cafe/HW/Latte/Renderer/Metal/MetalVoidVertexPipeline.h
src/Cafe/IOSU/legacy/iosu_mcp.h
src/Cafe/OS/RPL/rpl_symbol_storage.h
src/Cafe/OS/libs/nsyshid/Dimensions.h
src/Common/cpu_features.h
src/util/crypto/md5.h
```

### clean build 对比

最终状态重新配置、clean，并清空本轮 `.ninja_log` 后执行：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target clean
/usr/bin/time -p cmake --build build --target CemuBin -- -d stats
```

| 指标 | T1：Unity OFF | T3：全局 Unity ON | 变化 |
| --- | ---: | ---: | ---: |
| Ninja `FinishCommand` | 648 | 327 | -321（-49.5%） |
| wall time | 69.72s | 45.50s | -24.22s（-34.7%，仅参考） |
| user time | 711.39s | 431.96s | -279.43s |
| sys time | 69.64s | 42.05s | -27.59s |

中间的 `CemuCafe` 单目标方案为 401 条命令、`real 56.33s`。扩大到全局后，
最终构建图进一步降到 327 条命令。最终 `.ninja_log` 含 58 个 Unity 对象；
`CemuCafe` 自身为 36 个 Unity 对象、9 个独立源文件对象和 2 个 PCH。

wall time 受负载影响，只作参考；边数从 648 降到 327 是构建图的确定性变化。

### 关闭回退、运行与 Android

执行：

```sh
cmake -S . -B /tmp/cemu-c3-t3-global-unity-off -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCEMU_USE_UNITY_BUILD=OFF
cd src/android
./gradlew assembleDebug
./gradlew testDebugUnitTest
ctest --test-dir ../../build --output-on-failure
```

结果：

- 全局关闭时 `CEMU_USE_UNITY_BUILD=OFF` 且
  `CITRA_USE_UNITY_BUILD=OFF`，构建目录没有生成 Unity 文件。
- macOS GUI 在最终构建产物上保持运行 8 秒，再由验证脚本主动终止。
- Android cache 中两个 Unity 变量均为 ON，`assembleDebug` 通过；实际生成
  `foundation_debugbus`、`foundation_debugbus_dumpsys` 和 foundation
  RapidJSON 的 Unity 对象。
- `testDebugUnitTest` 通过。
- 桌面 `ctest` 返回成功，但当前构建图报告 `No tests were found`。

## T3 退出标准

- [x] clean build edges 与 wall time 已和 T1 对比。
- [x] 排除目标、排除源文件及原因已记录。
- [x] 全局开关默认 ON，OFF 回退不生成 Unity 文件。
- [x] foundation 与宿主使用同一开关，Android 中实际构建 Unity 对象。
- [x] macOS GUI、Android `assembleDebug` 和 Android 单测通过。

下一步：回到尚未实施的 C3-T2 ccache。
