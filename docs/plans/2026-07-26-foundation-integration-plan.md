# cemu_android foundation 植入与调整计划（修订版 v2）

- 日期：2026-07-26
- 上游依据：`dependencies/foundation/docs/specs/2026-07-05-emulator-common-facilities-design.md` §7（P0–P5 分期）
- 前身计划：azahar 仓 `docs/manual/self/plans/2026-07-05-p4-calibration-cemu-pilot.md` Task 3 / Task 4
- 前身执行记录：azahar 仓 `docs/manual/self/p4-pilot-notes.md`
- 接入指南：`dependencies/foundation/docs/guides/integrating-emulator-host.md`
- 编译加速手册：`dependencies/foundation/docs/guides/build-acceleration.md`
- 外部参考实现：`~/workspace/BotW-BetterVR`（塞尔达 BotW 的 PC-VR mod，见 §4）
- 实施 spec：`docs/plans/2026-07-26-foundation-integration-spec.md`
- 本文覆盖范围：cemu_android 仓内的接入与调整。foundation 侧改动仍在 foundation 仓单独提交。

阶段编号用 `C1…C7`，与 foundation 的 `P0…P5`、review 发现的 `S1…S9` 区分开。

> **v2 变更：** 按「第一步以整合官方最新修改、foundation 合入、编译加速为重点，mac 版可验为目标」重排。原 v1 的 C0（Android debugbus 加固）从第一位移到 C4——它需要真机、且与 mac 验收目标无关；foundation 合入与编译加速提前。

---

## 1. 现状核对（2026-07-26）

### 已完成

原计划 Task 3（debugbus 接入）已落地，提交 `914917ce`：

- foundation 作为私有子模块加入 `dependencies/foundation`，钉在 `b01f41c`（`git@github.com:tencentmalos/foundation.git`）。
- `CemuAndroid` 链接 `spatial::foundation_debugbus` + `spatial::foundation_debugbus_dumpsys`。
- `NativeDebugDump.cpp` 注册 `status/pause/resume/screenshot`，JNI 桥 + Kotlin `DebugDumpService`。
- 真机验证（设备 `PB3210PGL6170004G`，debug 包）：`assembleDebug` 通过，dumpsys 可达 native registry。

### 未完成

- foundation 只以 hack 方式接入了 debugbus 一个模块（见 S3）。
- **编译加速一项未做**，且 `dependencies/foundation/docs/guides/build-acceleration.md` 从未被消费。
- XR（原 Task 4）未开始，仓内无 XR 代码。

### 分支拓扑

- `upstream = https://github.com/cemu-project/Cemu.git` 是官方来源；本地 `main` 跟踪它，`origin/main` 是内部镜像。
- `feature/malos/basic_version` 是 Android 产品主线。`fc884596` 已把 `b8f2cf4b` 的完整 main 历史合入，main 是该分支祖先。
- `android-port` 是旧 Android 实现参考，只按需选择性引入，不再作为官方同步的中转站或并列主线。
- Android 最早从官方历史分出于 `8193ebf7`（2023-06-07，父提交 `ae4cb45c`）；本轮完整合流前最后一次 main merge 是 `0d6d0d47`（2026-04-19）。
- 历史惯例是 **merge 而非 rebase**（`0d6d0d47`、`f99e5b93`、`0af39208` 均为 merge commit）。

### C1 已完成

- 官方 main 从内部镜像旧基线快进 59 个提交到 `b8f2cf4b`，并推送 `origin/main`。
- `fc884596` 将 main 完整 merge 到 `feature/malos/basic_version`。
- macOS 配置、编译和启动成功；Android `assembleDebug` 与 `testDebugUnitTest` 成功。
- 详细证据见 `docs/verification/20260726-C1/`。

### C2 已完成

- foundation 根 CMake 以 `EXCLUDE_FROM_ALL` 注册，真实 target 可供后续功能按需链接；
  原有两个伪造 INTERFACE target 已删除。
- Android 当前只链接并构建 debugbus/dumpsys；foundation core、network、
  profiler、XR 均为后续计划能力，完整保留，但在出现真实消费者前不进入默认
  构建图。
- C2 初始提交没有向 vcpkg 添加 Crypto++。后续依赖收敛按维护者决定复用
  Azahar 已验证的 `cryptopp` / `cryptopp-cmake` 子模块，并与 fmt、glslang、
  zstd、libusb 一起集中在 Cemu 的单一依赖适配层；RapidJSON 直接复用
  foundation target。
- API 版本与子模块缺失负向护栏均已实际验证；macOS RelWithDebInfo、
  Android Debug/Release 和单测结果见 `docs/verification/20260726-C2/`。
- `vcpkg.json` 的直接依赖项由 33 降到 27。Boost、SDL3、curl/OpenSSL 及
  GUI/平台依赖暂留 vcpkg，原因与版本差异见
  `docs/verification/20260726-C2/dependency-convergence.md`。

### C3 进行中

- C3-T0 已在依赖收敛后的真实构建图上重建基线：RelWithDebInfo + LTO 为
  648 条 Ninja 完成命令、`real 77.38s`。
- C3-T1 已完成：RelWithDebInfo/Debug 默认关闭 LTO，Release 默认保留 LTO，
  Android 显式关闭。RelWithDebInfo 最终链接由 18.649s 降到 0.505s，边数保持
  648；完整记录见 `docs/verification/20260726-C3/build-speedup-notes.md`。
- C3-T3 按维护者指示提前完成：`CEMU_USE_UNITY_BUILD` 默认 ON，统一控制整个
  构建图与 foundation 的遗留开关；macOS clean build 降到 327 条命令、
  `real 45.50s`，macOS GUI、Android Debug 与单测均通过。
- 下一步回到尚未实施的 C3-T2 ccache。

### 编译加速现状

| 手法 | cemu 现状 | 依据 |
| --- | --- | --- |
| Dev 构建关 LTO | **已完成**，RelWithDebInfo/Debug 默认 OFF，Release 默认 ON | `ENABLE_LTO` + C3 验证记录 |
| ccache | **未接入**，且本机未安装 | 无 `COMPILER_LAUNCHER` 相关代码；`which ccache` 空 |
| Unity Build | **已完成**，全局默认 ON，保留明确排除清单 | `CEMU_USE_UNITY_BUILD` + C3 验证记录 |
| 预编译头 | **已做** | `src/Common/CMakeLists.txt:69`、`src/CMakeLists.txt:47-49` |
| 裁剪构建图 | 未系统做 | — |

主要 C++ target：`CemuCommon`、`CemuCafe`、`CemuComponents`、`CemuConfig`、`CemuInput`、`CemuAudio`、`CemuUtil`、`CemuResource`、`CemuGui`(INTERFACE)、`CemuBin`。

---

## 2. Review 结论

原计划从 Task 3 直接跳 Task 4。核对代码后，Task 3 的产物有若干问题会被后续阶段放大。按严重度：

| 编号 | 问题 | 证据 | 落在 |
| --- | --- | --- | --- |
| S1 | **release 构建下 debugbus 打不到模拟器进程。** `EmulationActivity` 在 release 被放进 `:EmulationProcess`，`DebugDumpService` 无 process 属性落主进程，native registry 是进程内 static，`pause/resume` 静默作用于无模拟器的进程 | `src/android/app/src/release/AndroidManifest.xml:5-8`、`src/main/AndroidManifest.xml:59-61` | C4 |
| S2 | **命令在 binder 线程直接改模拟器状态。** 违反 foundation 指南 §2「handler 自行 Post 到目标线程，不阻塞 dumpsys 线程」 | `src/android/app/src/main/cpp/NativeDebugDump.cpp:25-32`；`src/Cafe/CafeSystem.cpp:915` | C4 |
| S3 | **CMake 接入绕过 foundation 根构建并伪造 target。** 空 INTERFACE 顶替 `foundation_module_network` / `foundation_profiler`，tcp/profiler 一旦链接即哑火；XR 所需 imgui/math/platform/openxr 全在根 CMake 内，现结构无法扩展；`if(EXISTS)` 在 submodule 未 init 时静默降级 | `CMakeLists.txt:219-229` | **C2** |
| S4 | **exported Service 无 permission 且进 release 包**，`onCreate` 无条件启动；release 还开了 `isMinifyEnabled`，R8 可能影响 JNI 方法 | `src/main/AndroidManifest.xml:59-61`；`CemuApplication.kt:41-42`；`build.gradle.kts` | C4 |
| S5 | **注册了不可用的命令**：`screenshot` 恒返回 unavailable，自动化按 help 列表调用会得到假成功 | `NativeDebugDump.cpp:33-37` | C4 |
| S6 | **无 API 版本护栏**，submodule 指针漂移导致的不兼容只在编译期偶然暴露 | foundation `FoundationApiVersion.h`（= 1）未被引用 | **C2** |
| S7 | **`SetDumpsysRegistry` 设了但从未使用**，JNI 自己直调 `registry.Handle()`，形成双路径死代码 | `NativeDebugDump.cpp:38` vs `:71` | C4 |
| S8 | **计划与验收证据全在 azahar 仓**，cemu 仓零记录，`_out/` 未跟踪 | 本文即为修复 | — |
| S9 | **原 Task 4 粒度过粗**，把构建接入、loader、Gradle manifest、帧路径选型压进一步 | 见 C6 拆分 | C6 |

原则调整：

- **每个阶段的退出标准都必须包含 release 变体（或多进程配置）的验证**，不能只验 debug 包。S1 正是只验 debug 的直接后果。
- **动工前先同步上游**，且阶段进行中不同步（见 C1-T6）。

---

## 3. 阶段总览

### 阶段一：mac 版可验基线（C1 + C2 + C3）

**共同目标：在 macOS 桌面上，同步官方最新代码后，带 foundation 根 CMake 按需接入，能配置、能编译、能跑起来、能验证，并且编译速度可接受。**

选 mac 桌面而非 Android 真机作为第一步的验收面，是因为它不依赖设备、迭代快，且这三项工作（上游同步、foundation 根 CMake 接入、编译加速）的失败模式全部在构建系统层面，桌面即可暴露。Android 在阶段一只要求「不回归」（`assembleDebug` 仍通过），真机验证推迟到 C4。

| 阶段 | 内容 | 前置 | 风险 |
| --- | --- | --- | --- |
| C1（已完成） | 同步官方 Cemu 上游，并把完整 main 历史 merge 到 `basic_version`，转为每阶段前置 | 无 | 中–高 |
| C2（已完成） | foundation 合入正规化：根 CMake 按需接入、去伪造 target、加 API 版本护栏（S3、S6） | C1 | 中 |
| C2-F（已完成） | 依赖收敛：复用 Azahar / `tencentmalos` 子模块，减少 vcpkg 交叉构建面 | C2 | 中 |
| C3 | 编译加速：dev 关 LTO、ccache、unity build、裁剪构建图 | C2 | 低–中 |

三者顺序不可换：

- **C1 首轮必须最先且现已完成** —— 当时待同步的 31 个提交里含 `f8fb588b build: macOS CMake fixes`、`0fc74035 ENABLE_OPENGL/ENABLE_VULKAN`、`1c2b7d78 ENABLE_LIBUSB`、`8e3e961b SDL optional`。后三者是 C3 裁剪构建图的**现成工具**。
- **C2 在 C3 之前** —— 先消除伪造 target，建立真实、可测的依赖关系，再优化才不会对错误的构建图调参。Azahar 对照和实测后采用 `EXCLUDE_FROM_ALL`：根 CMake 负责注册，只有宿主明确链接的组件进入构建。
- **C2-F 收敛依赖但不裁功能** —— foundation 的后续组件全部保留；已有精确镜像和可验证版本的公共库先转为集中管理的子模块，版本或镜像尚不匹配的依赖继续留在 vcpkg，避免一次性替换导致平台漂移。
- **C3 收尾** —— C2 已把当前没有消费者的 foundation 组件排除出默认构建，C3 聚焦 Cemu 自身的 LTO、ccache、Unity Build 和平台子系统裁剪；每一刀都以最新验证记录为基线。

### 阶段二及以后

| 阶段 | 内容 | 前置 | 风险 |
| --- | --- | --- | --- |
| C4 | Android debugbus 加固（S1、S2、S4、S5、S7），恢复真机验证 | 阶段一 | 低 |
| C5 | 命令面扩展：screenshot 实装、profiler、calibration | C4 | 低 |
| C6 | XR 前置：loader 选型 + 构建 + Gradle manifest + 帧路径选型，纯色纹理跑通 session | C4 | 高 |
| C7 | XR 输入 adapter + 游戏画面上屏（**平面画面，非立体 VR**，范围界定见 §4） | C6 | 高 |

---

## 4. 外部参考实现：BotW-BetterVR

**本节为 2026-07-26 补充。此前的计划（含 azahar 侧原始 P4 计划）从未引用该项目。**

位置：`~/workspace/BotW-BetterVR`（`git@github.com:Crementif/BotW-BetterVR.git`，v0.9.17）。

### 它是什么

塞尔达《旷野之息》的 **PC-VR mod**，在 Windows x64 上给 Cemu 加 VR 模式。机制与本计划完全不同：

| | BotW-BetterVR | 本计划（cemu_android + foundation） |
| --- | --- | --- |
| 接入方式 | 外部 **Vulkan layer 拦截**（`vkroots.h`，`src/hooking/layer.cpp`），不改 Cemu 源码 | 进程内直接接入，改 cemu 源码 |
| 平台 | Windows x64，PC-VR（串流到头显） | Android arm64，头显本机运行 |
| Cemu 版本 | 外挂在 Cemu 2.6 release 上 | 本仓 fork |
| 游戏耦合 | **深度耦合 BotW**：`include/game_structs.h`、`src/hooking/{camera,skeleton,weapon,bow,entity_controller}.cpp` 直接改游戏内存结构 | 游戏无关，foundation 明令禁止含游戏语义 |
| 立体渲染 | 真双眼独立视锥 + 6DOF + 手臂/武器注入 | C7 终点是平面 layer |

### 可借鉴的部分

1. **`src/rendering/`（最有价值）** — `vulkan.cpp` / `swapchain.cpp` / `texture.cpp` / `openxr.cpp` 解决的是 C6 帧路径选型的同一个问题：Cemu 的 Vulkan 输出 → OpenXR swapchain。纹理格式与布局转换、swapchain 与 Cemu 帧节奏的同步、时序处理可直接对照。
2. **`src/utils/controller_bindings.h` + `src/hooking/controls.cpp`** — VR 手柄到 Wii U GamePad 的映射实践结论，供 C7 参考。
3. **`src/hooking/rumble.cpp`** — 震动反馈映射。
4. **`src/rendering/vulkan_imgui.cpp` + `src/hooking/imgui_menus.cpp`** — VR 内 ImGui overlay。
5. **README 的 Known Issues** — 记录了黑屏、爬梯子等真实踩坑，是验收用例的现成来源。

### 不可借鉴 / 需明确区隔的部分

- **`src/hooking/` 的游戏结构 hook 一律不能进 foundation**，也不建议进 cemu 核心。它是 BotW 专属的内存布局假设，换游戏即失效。
- Vulkan layer 拦截路径在 Android 上不适用（无外部 layer 注入机制），架构不可照搬。
- 它依赖 SteamVR/ALVR/Virtual Desktop 串流，与 Android 本机运行的性能模型完全不同，其性能数据不可迁移。

### 对计划范围的影响（重要）

本计划 C7 的终点是**平面画面 + 手柄映射**，而 BotW-BetterVR 交付的是**立体 6DOF VR**。这是两个不同的产品，中间隔着双眼独立视锥（需在模拟层面改游戏摄像机投影矩阵，或对 GX2/Latte 做双次渲染）、头动 6DOF 驱动游戏内摄像机、手部/武器实体注入（游戏专属）。

**若产品目标是「塞尔达 VR on Android」，C7 完成后还需要一个 C8 阶段**，其难度和不确定性都高于 C1–C7 之和，且大概率需要引入游戏专属逻辑——这与 foundation 的「不含游戏语义」原则冲突，届时需明确该逻辑的落点（cemu 侧 graphic pack？独立 mod 层？）。**这个决策建议在 C6 帧路径选型时就一并考虑**，因为立体渲染的需求会反过来约束帧路径的选择：如果最终要双眼独立渲染，Surface path 基本出局。

---

## 5. 通用约束

- **分支主线与上游同步节奏：** `feature/malos/basic_version` + 官方 main 是唯一必经主线。每阶段开工前先让本地/内部镜像 `main` 与 `upstream/main` 对齐，再确认 `git merge-base --is-ancestor main feature/malos/basic_version`；返回非 0 先 merge main。`android-port` 只按需选择性引入。阶段进行中不同步，否则验收基线会漂移。
- **验证矩阵：**
  - 阶段一（C1–C3）：**mac 桌面为主验收面**（配置 + 编译 + 运行 + 编译耗时度量）；Android 仅要求 `assembleDebug` 不回归。
  - C4 起：Android arm64-v8a debug **与** release 双变体都要验；只验 debug 是 S1 漏网的直接原因。涉及 native 行为改动时另跑 PC 侧构建确认未破坏桌面。
- **提交纪律：** cemu 仓与 foundation 仓分开聚焦提交；不夹带 `_out/`、`build_baseline/` 等未跟踪文件。
- **子模块：** 本分支子模块 URL 指向 `git@github.com:tencentmalos/...`；改 `.gitmodules` 后必须 `git submodule sync --recursive` + `git submodule status --recursive` 核对。
- **验收证据落点：** 每阶段的命令与输出归档到 `docs/verification/<YYYYMMDD>-<阶段>/`，不再只留在未跟踪的 `_out/`。截图/manifest/日志以文件为准，文档内摘录标注为人工摘录。
- **编译加速度量纪律：** wall time 受机器负载波动，按 foundation 手册 §7，以 **ninja build edges 数 + `.ninja_log`** 为准，每一刀独立提交独立验证。
- **当前依赖版本：** foundation `b01f41c`，`SPATIAL_FOUNDATION_API_VERSION = 1`。升级子模块指针时更新此行并复跑已完成阶段的退出标准。
- **当前上游基线：** `main = upstream/main = origin/main = b8f2cf4b`；`fc884596` 已将其 merge 到 `feature/malos/basic_version`。
- **mac 构建基线：** 已建立；同步前失败、同步后配置/编译/启动成功，见 `docs/verification/20260726-C1/`。

---

## 6. 与原计划的差异摘要

| 原计划 | 本修订 | 原因 |
| --- | --- | --- |
| 无上游同步环节 | 新增 C1，并转为每阶段前置 | 制定 v2 时 android-port 落后官方 31 个提交/约 3 个月；SDL3 迁移与构建选项重构直接压在后续要改的文件上 |
| Task 3 完成即进 Task 4 | 插入 debugbus 加固（C4）与构建正规化（C2） | Task 3 产物存在 S1–S7，带进 XR 会放大 |
| 无编译加速环节 | 新增 C3，消费 foundation `build-acceleration.md` | 该手册从未被消费；C2 已先建立按需构建的真实基线，C3 再优化 Cemu 自身热点 |
| 第一步验收面是 Android 真机 | 阶段一改为 **mac 桌面可验** | 三项工作的失败模式全在构建系统层面，桌面即可暴露且不依赖设备；上游同步本身带来 3 个 mac 构建修复 |
| Android debugbus 加固排第一 | 移到 C4 | 需真机、与 mac 验收目标无关，且它要改的文件会被上游同步冲突波及 |
| 验证只覆盖 debug 包 | C4 起双变体验证 | S1 是只验 debug 的直接后果 |
| Task 4 Step 1 = 链接 + XrHost | 拆为 C6 多步，新增 OpenXR loader 选型与帧路径选型 | 原步骤含多个独立失败点，且遗漏 loader 前置 |
| calibration 与 debugbus 同期 | 推迟到 XR 之后 | 无 XR 时标定图没有验收对象 |
| 未引用 BotW-BetterVR | 新增 §4，纳入 C6 / C7 参考 | 该项目已在 PC 上解决 Cemu→OpenXR 供帧与手柄映射 |
| 未界定「XR 影院」与「立体 VR」 | C7 明确范围界定，§4 提出可能需要的 C8 | 两者是不同产品，且立体渲染需求会反向约束 C6 的帧路径选型 |
| 计划与证据存于 azahar 仓 | 计划落 cemu 仓，证据落 `docs/verification/` | cemu 侧接手者此前无从查阅 |

---

## 7. 任务级拆分

各阶段的任务级拆分、验证命令与退出标准见实施 spec：`docs/plans/2026-07-26-foundation-integration-spec.md`。
