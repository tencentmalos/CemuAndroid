# cemu_android foundation 接入 — 实施 Spec

- 日期：2026-07-26
- 配套计划：`docs/plans/2026-07-26-foundation-integration-plan.md`（背景、review 结论、阶段划分与理由）
- 本文用途：**交给实施者（人或 AI）直接执行**。计划讲「为什么」，本文讲「做什么、怎么验、什么算完」。

## 阶段一目标（本文重点）

> **在 macOS 桌面上，同步官方 Cemu 最新代码后，带 foundation 根 CMake
> 按需接入，能配置、能编译、能跑起来、能验证，并且编译速度可接受。**

阶段一 = **C1 上游同步** → **C2 foundation 合入正规化** → **C3 编译加速**，顺序不可换（理由见计划 §3）。

Android 在阶段一**只要求不回归**（`assembleDebug` 仍通过），不需要真机。真机验证从 C4 开始。

C4–C7 见 §9，本文只给纲要，其中 C6/C7 含未决设计，**不要盲目开工**。

---

## 0. 交接说明

按顺序阅读 §1 → §2 → §3，再进入具体任务。任务之间有严格顺序依赖，**不要并行、不要跳过**。

每个任务都给了「退出标准」。**退出标准未全部满足时不要声称任务完成，也不要进入下一个任务。** 无法满足时停下来说明原因，不要绕过或降级验证。

所有事实基线（分支状态、commit hash、文件行号）都是 2026-07-26 核对的。开工前请重新核对，仓库可能已变化。

---

## 1. 环境与事实基线

### 仓库

| 项 | 值 |
| --- | --- |
| 工作目录 | `/Users/bytedance/workspace/cemu_android` |
| 当前分支 | `feature/malos/basic_version` |
| Android 产品主线 | `feature/malos/basic_version` |
| Android 旧实现参考 | `android-port`（只按需选择性引入） |
| 内部 remote | `origin = git@github.com:tencentmalos/CemuAndroid.git` |
| 官方 remote | `upstream = https://github.com/cemu-project/Cemu.git` |
| `origin/main` | 官方 Cemu main 的 tencentmalos 镜像 |
| 平台 | macOS（Darwin 25.5.0），Apple Silicon 假定 |

分支关系（2026-07-26，C1 完成后）：

- 本地 `main`、`origin/main`、`upstream/main` 均为 `b8f2cf4b`
- `fc884596` 的两个父提交为 `2588c674`（原 `basic_version`）和 `b8f2cf4b`（main）
- `main...feature/malos/basic_version` 为 `0 311`，即 main 独有提交为 0，完整 main 已成为产品分支祖先
- `android-port` 仍是产品分支祖先，但后续不再作为官方同步中转站

### mac 构建前置

`BUILD.md` §macOS 要求：

```sh
brew install git cmake ninja nasm automake libtool boost
```

外加 **MoltenVK privateapi 版**（brew 上的不行）：

```sh
curl -L -O https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.1/MoltenVK-macos-privateapi.tar
tar xf MoltenVK-macos-privateapi.tar
sudo mkdir -p /opt/homebrew/lib
sudo cp MoltenVK/lib/libMoltenVK.dylib /opt/homebrew/lib/
```

需要 Xcode 15+ 或 brew LLVM（C++20）。已同步的 `aa6e2c05` 提到 mac 还需 `pkgconf`，以后以当前 `BUILD.md` 为准。

**mac 构建基线已经建立。** 同步前失败、同步后成功的记录见
`docs/verification/20260726-C1/`。

### foundation 子模块

| 项 | 值 |
| --- | --- |
| 路径 | `dependencies/foundation` |
| URL | `git@github.com:tencentmalos/foundation.git`（私有） |
| 当前指针 | `b01f41c`（`heads/main`） |
| API 版本 | `SPATIAL_FOUNDATION_API_VERSION = 1`，定义于 `basic/underlying/core/public/spatial/core/FoundationApiVersion.h` |

可用 target（`modules/debugbus/CMakeLists.txt`）：

- `spatial::foundation_debugbus` — 核心 registry，无额外依赖
- `spatial::foundation_debugbus_tcp` — 需要 `spatial::foundation_module_network`
- `spatial::foundation_debugbus_dumpsys` — 仅 `if(ANDROID)` 下定义
- `spatial::foundation_debugbus_calibration`
- `spatial::foundation_debugbus_profiler` — 需要 `spatial::foundation_profiler`
- `spatial::foundation_xr`（`modules/xr/CMakeLists.txt`）— PUBLIC 依赖 `foundation_imgui`、`foundation_math`、`third_party_openxr_headers`、`vulkan-headers`；PRIVATE 依赖 `foundation_platform`、`foundation_profiler`

foundation 根 CMake 会注册 `third_party` + `basic` + `modules` 的完整 target
集合。Cemu 侧以 `add_subdirectory(... EXCLUDE_FROM_ALL)` 接入，因此配置阶段可见
这些真实 target，但默认构建只沿宿主明确链接的依赖闭包执行。`third_party`
包括 imgui、imgui-websocket、libevent、luascript、lz4、mimalloc、msgpack-c、
nlohmann_json、openxr、rapidjson、tinyxml2、yalantinglibs、yaml-cpp、zip；当前
Android debugbus/dumpsys 不依赖这些组件。

### cemu 构建加速现状

| 手法 | 现状 | 位置 |
| --- | --- | --- |
| Dev 构建关 LTO | **已完成**，RelWithDebInfo/Debug 默认 OFF，Release 默认 ON | `ENABLE_LTO` + C3 验证记录 |
| ccache | **已完成**，检测到时默认 ON | `CEMU_USE_CCACHE`；支持 `NDK_CCACHE` 与自动发现 |
| Unity Build | **已完成**，全局默认 ON，保留明确排除清单 | `CEMU_USE_UNITY_BUILD` + C3 验证记录 |
| 预编译头 | **默认 OFF**，可显式开启回退 | `CEMU_USE_PRECOMPILED_HEADERS` |
| 裁剪构建图 | **按维护者决定暂缓** | 保留现有默认构建项 |

主要 target：`CemuCommon`、`CemuCafe`、`CemuComponents`、`CemuConfig`、`CemuInput`、`CemuAudio`、`CemuUtil`、`CemuResource`、`CemuGui`(INTERFACE)、`CemuBin`。

### Android 工程（阶段一只需保证不回归）

| 项 | 值 |
| --- | --- |
| Gradle 根 | `src/android` |
| 包名 | `info.cemu.cemu`，debug 变体 `info.cemu.cemu.debug` |
| minSdk / ABI | 30 / 仅 arm64-v8a |
| native target | `CemuAndroid`（`src/android/app/src/main/cpp/CMakeLists.txt`） |
| `buildConfig` | 已启用（`build.gradle.kts:140-141`） |
| release minify | `isMinifyEnabled = true` |
| 进程模型 | C4 后 Debug/Release 都使用单 App 进程；原 `src/release/AndroidManifest.xml` 已删除 |

### 已落地的 foundation 接入（commit `914917ce`）

| 文件 | 内容 |
| --- | --- |
| `CMakeLists.txt:219-229` | 伪造 `spatial::foundation_module_network` / `spatial::foundation_profiler` 为空 INTERFACE，再 `add_subdirectory(dependencies/foundation/modules/debugbus)` |
| `CMakeLists.txt:42` 附近 | vcpkg overlay 改为 `VCPKG_TARGET_ANDROID` 优先 |
| `src/android/app/src/main/cpp/NativeDebugDump.cpp` | registry + JNI 桥，注册 `status/pause/resume/screenshot` |
| `src/android/app/src/main/cpp/CMakeLists.txt` | 链接两个 debugbus target |
| `.../nativeinterface/NativeDebugDump.kt` | JNI 声明 |
| `.../utils/DebugDumpService.kt` | Service.dump 分发 |
| `src/main/AndroidManifest.xml:59-61` | Service 声明，`exported="true"`，无 permission |
| `CemuApplication.kt:41-42` | `initializeDebugDump()` + `DebugDumpService.start(this)`，无条件执行 |

---

## 2. 不可违反的约束

1. **不提交未跟踪的杂物。** 仓库里现有未跟踪的 `AGENTS.md`、`skills/`、`_out/`、`docs/`（部分）。提交前逐个核对 `git status`，只加本任务相关文件。前次试点已因此中断过一次。
2. **不直接修改 `dependencies/` 下的子模块内容。** foundation 需要改时，在 foundation 仓单独提交，再更新 cemu 侧指针。
3. **子模块 URL 必须指向 `git@github.com:tencentmalos/...`。** 改 `.gitmodules` 后必跑 `git submodule sync --recursive`，并用 `git submodule status --recursive` 核对。
4. **签名只走环境变量** `ANDROID_STORE_FILE` / `ANDROID_KEY_STORE_PASSWORD` / `ANDROID_KEY_ALIAS`，不得提交密钥或本地配置。
5. **不 rebase 已推送的分支。** 官方更新先快进本地 `main` 并推送 `origin/main`，再 merge 到 `feature/malos/basic_version`。`android-port` 只作为可选 Android 改动来源，不得成为同步前置。
6. **编码风格**：C/C++ 遵循 `CODING_STYLE.md` 与 `.clang-format`（成员 `m_` 前缀、静态 `s_` 前缀、变量 lowerCamelCase、类/函数 UpperCamelCase、固定宽度类型 `uint32`/`sint64`、大括号单独成行）。**不要对整个文件跑格式化**，只格式化改动行。Kotlin 遵循官方 Conventions 并贴近现有风格。
7. **提交粒度**：一个提交解决一个问题，标题短小祈使句带作用域（`build: ...`、`android: ...`）。**编译加速的每一刀独立提交、独立验证。**
8. **验证要真跑。** 退出标准里的命令必须实际执行并保留输出。跑不了就明说，不要用推断替代。

---

## 3. 需要人工决策的点 — 遇到请停下来问，不要自行选择

| 编号 | 决策点 | 出现在 |
| --- | --- | --- |
| D1 | `origin/main` 出现官方没有的提交，或更新镜像时 push 被权限拒绝 | C1-T1/T2 |
| D2 | 上游 SDL3 迁移引入新子模块时，镜像到 tencentmalos 的操作由谁做 | C1-T4 |
| D3 | 后续启用 foundation core/network/profiler/XR 时，若传递依赖导致构建时长/产物体积不可接受，是否推动 foundation 侧进一步组件化 | C5/C6 |
| D4 | C3 裁剪构建图时，哪些子系统必须保留默认开启（需要产品侧确认，不能凭代码猜） | C3-T4 |
| D5 | **已决定：采用单 App 进程。** 删除 Release `:EmulationProcess`，游戏退出只结束 title；Service、Activity、native registry 同进程 | C4 |
| D6 | **已决定：release 保留 debugbus。** Service 不导出，增加 signature permission 与 R8 keep 规则 | C4 |
| D7 | C6 帧路径选型（Surface vs Vulkan），受「是否要做立体 VR」影响 | C6 |

---

## 4. C1：同步官方上游

> **首轮已完成。** 2026-07-26 的结果见
> `docs/verification/20260726-C1/post-sync.md` 与
> `docs/verification/20260726-C1/main-merge.md`。以下步骤保留为后续常态同步流程。

> **首轮当时必须最先做。** 当时待同步的 31 个提交含 mac 构建修复、SDL3
> 迁移和 C3 所需开关；该风险现已消化。未来轮次仍在阶段边界先同步。

### C1-T0 建立 mac 构建基线（同步前）

**目的：** 拿到「同步前」的对照组。没有它，同步后出问题分不清是上游带来的还是本来就坏的。

```sh
brew install git cmake ninja nasm automake libtool boost
# MoltenVK privateapi：见 §1
cmake -S . -B build_baseline -DCMAKE_BUILD_TYPE=RelWithDebInfo -G Ninja
/usr/bin/time -p cmake --build build_baseline 2>&1 | tee /tmp/baseline_build.log
```

**记录：** 配置是否成功、编译是否成功、失败时的首个错误、build edges 数（`cmake --build build_baseline -- -d stats`）、wall time。

**退出标准：** 基线状态已记录到 `docs/verification/<YYYYMMDD>-C1/baseline.md`。**基线构建失败也是合法结果**——记录下来即可，不要在此任务里修，上游同步可能自带修复。

### C1-T1 核对官方与内部镜像

```sh
git remote get-url upstream
git fetch upstream origin
git rev-list --left-right --count upstream/main...origin/main
```

- 输出格式 `A<TAB>B`：A 是 upstream 独有（镜像落后量），B 是 origin 独有。
- A 不为 0：正常进入 C1-T2，把官方更新快进到本地与内部镜像 main。
- **B 不为 0 → 触发 D1，停下来问。** 内部 main 应是官方镜像，不能自行丢弃额外历史。
- C1-T2 的 push 若因权限失败，同样触发 D1；不要绕过内部镜像直接长期合入业务分支。

**退出标准：** B == 0；A == 0 时镜像已新鲜，A 不为 0 时继续 C1-T2。

### C1-T2 更新内部 main 镜像

```sh
git switch main
git merge --ff-only upstream/main
git push origin main
```

**退出标准：** `main`、`upstream/main`、`origin/main` 指向同一提交。

### C1-T3 冲突高危清单

| 上游提交 | 冲突面与处理要点 |
| --- | --- |
| `f8fb588b` build: macOS CMake fixes (#1883) | **直接服务于阶段一目标**，优先保上游版本 |
| `aa6e2c05` build.MD: mac brew deps + bundle flag (#1882) | 同步后以新版 `BUILD.md` 为准更新 §1 的 mac 前置 |
| `0d832c48` CI: Disable fail-fast for macOS builds (#1901) | CI 配置，冲突面小 |
| `f3fecba3` Migrate from SDL2 to SDL3 3.4.2 (#1847) | 改 `dependencies/` 子模块与 vcpkg 依赖。本分支 `38dc527d` 已把 `.gitmodules` 指向 tencentmalos 镜像 → **新增/变更的子模块必须先有镜像**，否则合完拉不下来。触发 D2 |
| `8e3e961b` build+input: Make SDL optional (#1895) | 同上，且触及 Android 输入路径。**C3 裁剪要用的开关** |
| `0fc74035` build+Latte: ENABLE_OPENGL / ENABLE_VULKAN | 与 C2 改同一批文件，且是 **C3 裁剪的主力开关**（mac 上 OpenGL 可关） |
| `1c2b7d78` build: Add ENABLE_LIBUSB option (#1886) | 与 android-port 的 `a4c911a2 Implement emulated usb devices` 重叠。**C3 裁剪开关** |
| `6f6c1299` Logging: Add compile-time format checks (#1885) | 大概率打断 Android 日志代码（`NativeLogging.cpp`、`ih264d` 相关）。编译错误可能很多，逐个修，**不要关掉检查** |
| `ad73c1e0` Input: Fix race condition in button mapping access (#1900) | 与 `8e5c2057`、`e82f99cd` 重叠。**同时是 C7 手柄 adapter 的前置** |
| `e2a69bec` debugger: Rework PPC debugger | 大范围重构 |
| `c080b738` Latte: Rework interval tree for vertex/uniform cache | 大范围重构 |
| `414aa9d6` GX2+Latte: Rework GX2CopySurface | 大范围重构 |
| `a02ba9d8` / `a29a160b` / `dfb9a99c` Vulkan 系列 | 与 `50b79401`、`685f7cac` 重叠 |

### C1-T4 子模块处理

```sh
git submodule sync --recursive
git submodule update --init --recursive
git submodule status --recursive
```

**要点：**

- `.gitmodules` 冲突时：**保留 foundation 条目**（上游不知道它存在），**保留 tencentmalos 镜像 URL 重写**（`38dc527d` 的意图不能被上游覆盖回官方 URL）。
- 上游引入新子模块 → 触发 D2：先在 tencentmalos 建镜像 → `.gitmodules` 指向镜像 → 再验证拉取。

**退出标准：** `git submodule status --recursive` 无 `-`（未初始化）或 `+`（指针不符）前缀；`.gitmodules` 中所有 URL 均为 tencentmalos。

### C1-T5 同步后验收 —— **mac 可验是本阶段的核心退出标准**

```sh
# mac 桌面：配置 + 编译
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -G Ninja
/usr/bin/time -p cmake --build build 2>&1 | tee /tmp/c1_build.log

# mac 桌面：能跑起来
./build/bin/Cemu_relwithdebinfo   # 实际产物名以构建输出为准
```

「能跑起来」的判定：**GUI 能启动、主窗口正常显示、不立即崩溃、日志无致命错误**。不要求能真跑游戏（需要游戏镜像，不作为退出标准）。

Android 不回归：

```sh
cd src/android && ./gradlew assembleDebug
```

**退出标准：**

- mac 配置成功、编译成功、GUI 可启动
- Android `assembleDebug` 通过
- 与 C1-T0 基线的对照结论已记录（哪些原本坏的被上游修好了 / 哪些是新引入的）
- 证据（完整命令 + 输出 + 启动截图）归档 `docs/verification/<YYYYMMDD>-C1/`

### C1-T6 汇入 Android 产品主线

```sh
git switch feature/malos/basic_version
git merge --no-ff main
```

解冲突时对照 C1-T3 清单，不要机械 `--theirs` / `--ours`。保留 Android 平台接入，同时采用 main 已替换的新接口和已删除旧路径的决定。

**退出标准：** `git merge-base --is-ancestor main feature/malos/basic_version` 返回 0；C1-T5 全套验收在产品分支上复跑一遍全绿。

`android-port` 不参与这条必经路径。只有在维护者确认其中某项 Android 改动仍有价值时，才审阅后选择性 cherry-pick 或显式 merge；引入后仍必须复跑上述验收。

### C1-T7 转为常态节奏 + 更新基线记录

- **每个阶段开工前**执行：`git fetch upstream origin`，先令本地/内部镜像 `main` 与 `upstream/main` 对齐，再执行 `git merge-base --is-ancestor main feature/malos/basic_version`。返回非 0 则先走 C1-T6。
- **阶段进行中不同步。** 中途合入上游会让该阶段的验收基线漂移，无法判断问题来自本阶段改动还是上游。同步只在阶段边界做。
- 若某阶段跨度超过两周，在阶段边界补一次同步。
- 更新计划文档 §5 的「当前上游基线」与「mac 构建基线」两行。

**提交：** `docs: update upstream sync and mac build baseline`

---

## 5. C2：foundation 合入正规化

前置：C1 完成。**本阶段的验收面同样是 mac 桌面。**

> **已完成。** 根接入结果见 `docs/verification/20260726-C2/integration.md`，
> 后续依赖收敛见
> `docs/verification/20260726-C2/dependency-convergence.md`。
> Azahar 对照和 Cemu 双平台实测后，最终采用根 CMake
> `EXCLUDE_FROM_ALL` 的按需构建方式。C2 初始提交未向 vcpkg 新增 Crypto++；
> 维护者随后确认 core/network/profiler/XR 都会使用，因此通过 Azahar 已验证的
> `cryptopp` / `cryptopp-cmake` 子模块预备其真实依赖，仍不把 Crypto++ 加入
> vcpkg。

### C2-T1 改为根 CMake 接入（修 S3）

**问题：** `CMakeLists.txt:219-229` 把 `spatial::foundation_module_network` 和 `spatial::foundation_profiler` 伪造成空 INTERFACE 库，再只 `add_subdirectory(modules/debugbus)`。后果：

- 一旦链接 `foundation_debugbus_tcp` / `_profiler` 就会链到空实现 → 链接错误或运行期哑火（挡住 C5）
- XR 需要的 `foundation_imgui`/`math`/`platform`/`third_party_openxr_headers` 全在根 CMake 里，现结构无法扩展（挡住 C6）
- `if(EXISTS ...)` 静默降级：submodule 未 init 时构建照过，只是没有 debugbus

**做什么：**

- 用 `add_subdirectory(dependencies/foundation EXCLUDE_FROM_ALL)` 替换现有的
  stub + 子目录接入。根目录负责注册真实 target，`EXCLUDE_FROM_ALL` 保证只有
  Cemu 明确链接的组件进入默认构建
- 把 `if(EXISTS ...)` 改成显式选项 `CEMU_ENABLE_FOUNDATION`；开启但子模块缺失时 `message(FATAL_ERROR "foundation submodule missing, run: git submodule update --init --recursive")`
- **mac 与 Android 都要能配过。** mac 当前不链接 foundation 组件，默认构建图
  不应因此膨胀；Android 当前只链接 debugbus/dumpsys。core/network/profiler/XR
  都是后续范围，不得因为当前没有消费者而删除；依赖可提前注册，但不得让它们
  无条件进入默认构建闭包
- foundation 里的 `CITRA_USE_UNITY_BUILD` 是从 azahar 带过来的变量名；C2
  当时不定义，已在 C3 统一到全局宿主开关
- 后续组件一旦成为真实消费者，其依赖应由对应 foundation target 自洽带入；
  若依赖闭包不可接受再触发 D3，**不要在 Cemu 侧伪造 target 或引入第二套依赖层**

**退出标准：**

- mac：配置成功、`cmake --build build` 成功、GUI 可启动
- Android：`assembleDebug` + `assembleRelease` 通过
- 故意重命名 `dependencies/foundation` 目录后配置报出上述 FATAL_ERROR（验证后改回）
- **记录接入前后的 build edges 数、编译 wall time、产物体积对比**，写进 `docs/verification/<YYYYMMDD>-C2/`——这是 C3 的度量输入
- Cemu `vcpkg.json` 不因 foundation 后续组件增加依赖；若出现第三方库
  重复/冲突，优先复用 Azahar 已验证或 `tencentmalos` 已镜像的子模块并记录
  版本，不能靠删除 foundation 能力规避

### C2-T2 API 版本护栏（修 S6）

在一处集中的 foundation 接入头（或 `NativeDebugDump.cpp`）加：

```cpp
#include <spatial/core/FoundationApiVersion.h>
static_assert(SPATIAL_FOUNDATION_API_VERSION >= 1, "foundation submodule too old");
```

**退出标准：** 临时把断言改成 `>= 999` 能触发编译失败（验证生效后改回）。

### C2-T3 提交

`build: link foundation through its root CMake`

### C2-T4 依赖收敛（C2 后续，已完成）

维护者确认 foundation 新增组件后续都会使用，并要求尽量移除 vcpkg。执行时：

- 所有新增子模块继续使用 `git@github.com:tencentmalos/...`，版本钉在已验证
  tag/commit。
- fmt 12.1.0、glslang 15.1.0、zstd 1.5.7、libusb 1.0.26 由统一
  `cmake/CemuBundledDependencies.cmake` 注册。
- Cemu 的 RapidJSON 消费改为复用
  `spatial::third_party_rapidjson`。
- Crypto++ 采用 Azahar 已验证的 `cryptopp` / `cryptopp-cmake` 指针，为
  foundation core 等后续真实消费者准备 target；未被链接时仍不构建。
- 暂不迁移没有精确版本对齐或完整跨平台验证的依赖。尤其 Boost 当前 vcpkg 为
  1.88、Azahar 镜像已到 1.90；Cemu 要求 SDL 3.4.10，而现有镜像 tag 只到
  3.4.8。不得为了降低依赖数量而做无验证升级。

**退出标准：**

- vcpkg 直接依赖项减少，且未向 vcpkg 添加 Crypto++。
- macOS 配置/编译通过；Android `assembleDebug`、`assembleRelease`、
  `testDebugUnitTest` 通过。
- `git submodule status --recursive` 无 `-` 或 `+`，所有 URL 指向
  `tencentmalos`。
- foundation core/network/profiler/XR target 和源码仍保留。

**提交：** `build: migrate shared dependencies out of vcpkg`

---

## 6. C3：编译加速

前置：C2 完成（先建立真实按需依赖图与度量基线，再做加速）。

**手册：** `dependencies/foundation/docs/guides/build-acceleration.md`。以下按该手册的收益/风险排序裁剪到 cemu 现状。

### C3-T0 度量方法（每一刀都用它）

> **已完成。** 依赖收敛后的起点为 648 条 Ninja 完成命令、
> `real 77.38s`；详细热点和命令见
> `docs/verification/20260726-C3/build-speedup-notes.md`。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target clean && rm -f build/.ninja_log
/usr/bin/time -p cmake --build build -- -d stats
```

- **以 build edges 数 + `.ninja_log` 为准**，wall time 只作参考（受机器负载波动）。
- 逐目标热点：按 `.ninja_log` 聚合耗时找最重的库，再决定下一刀。
- **每一刀独立提交、独立验证。**

**退出标准：** C2 完成后的度量数据已记录，作为 C3 的起点基线。

### C3-T1 Dev 构建关闭 LTO（收益大、零风险，先做这个）

> **已完成。** RelWithDebInfo 边数保持 648，最终链接由 18.649s 降到
> 0.505s；全新 Release 目录确认默认 `ENABLE_LTO=ON` 并构建通过；Android
> Debug 与单测通过。详细记录见 C3 验证文档。

**现状：** `CMakeLists.txt:85-86` 对 `Release` 与 `RelWithDebInfo` 都开了 IPO/LTO。日常开发用 `RelWithDebInfo`，LTO 主要吃链接时间与内存。

**做什么：** 按手册 §1，Release 保留 LTO，Debug/RelWithDebInfo 默认关闭，走可覆盖的 `option`：

```cmake
if (NOT MSVC AND CMAKE_BUILD_TYPE STREQUAL Release)
    set(DEFAULT_ENABLE_LTO ON)
else()
    set(DEFAULT_ENABLE_LTO OFF)
endif()
option(ENABLE_LTO "Enable link time optimization" ${DEFAULT_ENABLE_LTO})
```

Android 侧在 gradle 各 buildType 显式传 `-DENABLE_LTO=OFF`，防外部默认值漂移。

**退出标准：** mac `RelWithDebInfo` 链接时间对比数据；`Release` 仍开 LTO 且构建通过；Android `assembleDebug` 通过。

**提交：** `build: disable LTO for non-release builds`

### C3-T2 ccache 接入（增量/切分支收益大）

> **已完成，commit `02bf06b3`。** 本机安装 ccache 4.13.6；支持
> `NDK_CCACHE` 和 `find_program` 两条路径，坏 launcher 会警告并安全降级，
> `CEMU_USE_CCACHE=OFF` 可显式关闭。PCH 开启时第二轮只命中 100/278 次编译，
> 促成后续独立的 PCH 调整。

```sh
brew install ccache
```

按手册 §2 加 CMake 接入，统一 `NDK_CCACHE` 环境变量与本机 `find_program` 两条路径，并做可运行性校验（避免坏 launcher 卡死构建）。cemu 侧选项名建议 `CEMU_USE_CCACHE`。

**实施时的基线注意项：** cemu 当时使用 PCH
（`src/Common/precompiled.h`，多 target `REUSE_FROM`）。手册 §2 明确提示
配 PCH 时注意 ccache sloppiness，且 **`pch_defines` 不能进 sloppiness**
（azahar 上游 `19cc8e626` 的历史坑）。后续 T2A 已把 PCH 默认关闭。

**退出标准：**

- 首次构建后 `ccache -s` 显示有写入
- clean 后二次构建命中率显著（记录 hit rate 与 wall time 对比）
- **正确性验证**：ccache 命中的构建产物能正常启动 GUI（PCH + ccache 配错会产生诡异的运行期问题，不只是编译失败）

**提交：** `build: use ccache as compiler launcher when available`

### C3-T2A 默认关闭 PCH（维护者追加）

> **已完成，commit `39a06b25`。**

全局 `CEMU_USE_PRECOMPILED_HEADERS` 默认 OFF，并通过
`CMAKE_DISABLE_PRECOMPILE_HEADERS` 覆盖完整构建图。Cemu 源文件仍依赖
`precompiled.h` 的隐式包含契约，因此 PCH 关闭时使用编译器的普通强制包含，
不生成预编译产物；第三方 target 不再生成 PCH。显式设为 ON 时恢复原 PCH
路径，MSVC 上除 `CemuCommon` 外的 target 继续复用 `CemuCommon` PCH。

结果：macOS/Android Ninja 命令均无 `cmake_pch`；macOS 第二次 clean build
的 264 次编译中命中 262 次（99.24%），`real 10.17s`。PCH=ON 的独立全量
回退构建也通过。关闭 PCH 会牺牲无缓存冷构建性能，取舍与完整数据见 C3
验证文档。

**提交：** `build: disable precompiled headers by default`

### C3-T3 Unity Build（clean build 收益大，需排除名单）

> **已完成。** 按维护者指示先于 C3-T2 实施。全局
> `CEMU_USE_UNITY_BUILD` 默认 ON，并同步控制 foundation 的
> `CITRA_USE_UNITY_BUILD`；macOS clean build 从 648 条命令、69.72s 降到
> 327 条、45.50s。目标/源文件排除原因、关闭回退、macOS GUI 和 Android
> 实测见 C3 验证文档。

**原现状：** 完全未用。

按手册 §3，对大目标开 `UNITY_BUILD`，选项名建议 `CEMU_USE_UNITY_BUILD`。候选目标按 `.ninja_log` 热点排序，`CemuCafe`（Latte/PPC）大概率最重。

**要点：**

- **排除名单从空开始，编译报错（符号重定义/宏串扰）一个加一个。** JIT 编译器源（PPCRec 相关）、`*_platform.cpp` 这类平台分支文件基本必进名单，用 `SKIP_UNITY_BUILD_INCLUSION`。
- foundation 各模块已带 `if (CITRA_USE_UNITY_BUILD) set_target_properties(... UNITY_BUILD ON)`。最终决定由全局 `CEMU_USE_UNITY_BUILD` 强制同步该遗留变量，确保宿主与 foundation 不会静默使用不同模式。

**退出标准：** clean build edges 与 wall time 对比；**排除名单及每一项的原因记录在案**；mac GUI 可启动；Android `assembleDebug` 通过。

**提交：** 按目标分批提交，如 `build: enable unity build for CemuCafe`

### C3-T4 裁剪默认构建图（针对性强、需逐项验证）

> **按维护者 2026-07-26 决定暂缓。** 本轮不裁剪 Vulkan、OpenGL、libusb、
> SDL 或其他默认构建项，也不删除任何 foundation 能力。以后恢复该任务时
> 仍必须先触发 D4，不能把“本轮暂缓”误写成产品侧保留/删除结论。

**原则（手册 §5）：优先移除无用目标，而不是加一堆默认开关。**

C1 同步带来的上游开关是现成工具，**先用它们，不要自己造**：

| 开关 | 来源 | mac 上的裁剪机会 |
| --- | --- | --- |
| `ENABLE_OPENGL` / `ENABLE_VULKAN` | `0fc74035` | mac 走 MoltenVK/Vulkan，OpenGL 后端可关 |
| `ENABLE_LIBUSB` | `1c2b7d78` | 按是否需要实体 USB 设备决定 |
| SDL optional | `8e3e961b` | 按输入需求决定 |

foundation 当前没有消费者的 `third_party` 已由 C2 的 `EXCLUDE_FROM_ALL`
排除，但相关 target 和源码必须保留，不是 C3 的删除对象。后续链接
core/network/profiler/XR 后若出现过大的传递依赖，
再触发 D3；**若需要改 foundation，走 foundation 仓单独提交**，不要在 Cemu
侧 hack。

**约束先行 → 触发 D4：** 明确哪些子系统必须保留默认开启，写进裁剪记录，避免后人误删。**这个不能凭代码猜，需要产品侧确认。**

**退出标准：** 每一刀独立提交独立验证；build edges 从 `.ninja_log` 确认目标真的消失；mac GUI 可启动 + Android `assembleDebug` 通过；裁剪记录（含「必须保留」清单）归档。

### C3-T5 阶段一收尾

汇总 C1/C2/C3 的度量数据，写一份 `docs/verification/<YYYYMMDD>-C3/build-speedup-notes.md`：

- 各刀的 build edges / wall time / ccache hit rate 前后对比
- 剩余热点清单（供后续继续优化）
- unity 排除名单及原因
- 裁剪状态（本轮 T4 暂缓，现有默认项原样保留）

**提交：** `docs: record build speedup measurements`

### 阶段一整体退出标准

- [x] mac：`cmake` 配置成功、编译成功、GUI 可启动
- [x] mac：编译耗时与 build edges 相对 C2 完成时有可量化改善，数据归档
- [x] Android：`assembleDebug` 与 `assembleRelease` 均通过（不回归）
- [x] foundation 以根 CMake + `EXCLUDE_FROM_ALL` 按需接入，无伪造 target，有 API 版本护栏
- [x] `main = upstream/main = origin/main`，且 main 是
      `feature/malos/basic_version` 的祖先
- [x] 全部证据归档在 `docs/verification/` 下

---

## 7. C4：Android debugbus 加固

前置：阶段一完成。**本阶段起恢复 Android 真机验证。**

> **实现状态（2026-07-26）：** D5/D6 已按维护者决定落地。代码和当前设备
> 可用范围内的验证已完成；证据见
> `docs/verification/20260726-C4/debugbus-hardening.md`。

修 S1、S2、S4、S5、S7。实施结果：

- **S1 单进程归属（D5）** — 删除 Release manifest 中的
  `android:process=":EmulationProcess"`，Debug/Release 均为单 App 进程。
  游戏退出先调用 `CafeSystem::ShutdownTitle()`，然后关闭 Activity，不再
  `exitProcess(0)`。Service、Activity 与 native registry 因此天然同进程；
  Service 在 Application 初始化时启动，并在 Activity started 时幂等恢复，
  生命周期不再绑定某一局游戏。退出标准：两变体只出现一个 App PID；Debug
  运行中 title 的 pause/resume 返回并反映实际状态；退出 title 后 App PID 不变。
- **S2 线程模型** — `DebugDumpRequestExecutor` 把 binder 请求 Post 到 Android
  主控制线程，2 秒超时，拒绝、超时、中断和 handler 异常都有明确错误串。
  退出标准：单测覆盖直跑、Post、拒绝、超时和异常；Debug 真机连续
  pause/resume ≥20 轮不崩溃、不错乱。
- **S4 Release 保留（D6）** — Service 留在 main manifest，但
  `exported=false` 并受 `${applicationId}.permission.DEBUG_DUMP` signature
  permission 保护；ProGuard/R8 显式保留 Service 与 JNI bridge。退出标准：
  两个 APK 的 merged manifest 均含受保护 Service、均无独立 process；
  Release `help`/`status`/空闲态 pause/resume 经 R8 后可用。
- **S5 空壳命令** — `screenshot` 在 C5 实装前已摘掉注册。
- **S7 双路径** — JNI 统一调用 foundation `HandleDumpsysRequest()`。
- **验证边界** — 设备未安装正式游戏。Debug 使用临时 WUHB 验证了运行中
  title 控制；Release 已完成构建、manifest/R8/JNI 和空闲态命令验证，但
  Release 运行中正式游戏的 pause/resume 仍需在具备游戏时补验，不能用推断
  代替。

提交建议：`android: use a single process for emulation debug control`。

---

## 8. C5：命令面扩展

前置：C4。

- **C5-T1 screenshot 实装** — Kotlin 侧提供 capture 回调（PixelCopy 或复用 cemu 现有截图路径），native 注册为 host action。返回落盘路径供 `adb pull`。
- **C5-T2 profiler 命令** — 链接 `spatial::foundation_debugbus_profiler`（依赖 C2 提供的真 `spatial::foundation_profiler`），`RegisterProfilerCommands(registry)`。退出标准：`profiler_backend` / `profiler_endpoint` / `profiler_port` 真机可用，输出与 `integrating-emulator-host.md` §5 一致。
- **C5-T3 calibration — 推迟到 C6 之后**（无 XR 时标定图没有验收对象）。
- **C5-T4 TCP transport — 按需**。

---

## 9. C6 / C7：不要盲目实施

这两个阶段含未决设计，**在 D7 决策落地前不要写实现代码**。先做的是设计工作，产出结论文档，不是代码：

1. **OpenXR loader 选型（C6 前置）** — foundation 只提供 headers（`third_party/openxr` → `spatial::third_party_openxr_headers`），**不含 loader**。需确定来源：厂商 AAR / 系统 loader / 静态 loader。原计划完全没提这一项。
2. **帧路径选型（即 D7）** — cemu Android 当前走 `SurfaceTexture` + `ANativeWindow`（`src/android/app/src/main/cpp/NativeEmulation.cpp:167-201`）。Surface path 改动小但受中转开销与时序限制；Vulkan path 需拿到 cemu Vulkan renderer 的输出纹理与队列/同步对象，改动大但延迟更优。
3. **产品范围确认** — C7 的终点是「平面画面 + 手柄映射」的 **XR 影院模式**，不是立体 6DOF VR。若目标是后者（对标 `~/workspace/BotW-BetterVR`），需追加 C8，且立体渲染需求会**反向否决 Surface path**。这个问题必须在 D7 之前回答。

**参考实现：** `~/workspace/BotW-BetterVR`（详见计划 §4）。可借鉴 `src/rendering/`（Cemu Vulkan 输出 → OpenXR swapchain）、`src/utils/controller_bindings.h` + `src/hooking/controls.cpp`（VR 手柄 → Wii U GamePad 映射）、`src/hooking/rumble.cpp`（震动）。**不可借鉴** `src/hooking/` 的 BotW 游戏结构 hook，以及 Vulkan layer 拦截架构（Android 无外部 layer 注入机制）。

C7 手柄 adapter 的前置是 C1 同步带入的 `ad73c1e0 Input: Fix race condition in button mapping access`——adapter 写法取决于修复后的加锁语义。

---

## 10. 通用完成定义

每个任务在声称完成前，逐条确认：

- [ ] 任务的「退出标准」全部满足，且每条都实际执行过验证命令
- [ ] 验证输出已归档到 `docs/verification/<YYYYMMDD>-<阶段>/`（命令 + 完整输出，不是摘要）
- [ ] 阶段一：mac 配置 + 编译 + GUI 启动都验过；Android `assembleDebug` 不回归
- [ ] C4 起：Android arm64-v8a debug **与** release 双变体都验过
- [ ] 编译加速类改动：build edges / `.ninja_log` 数据已记录，每一刀独立提交
- [ ] `git status` 干净，无夹带 `_out/`、`AGENTS.md`、`skills/` 等未跟踪文件
- [ ] 提交聚焦、标题为祈使句、无格式化噪声、无意外子模块指针变化
- [ ] 若发现 foundation 文档（`integrating-emulator-host.md`、`build-acceleration.md`）与实际不符，记录偏差，回写由 foundation 仓单独提交

跑不通、验不了、或需要 §3 的人工决策时：**停下来说明情况，不要自行降级验证标准或猜测决策。**
