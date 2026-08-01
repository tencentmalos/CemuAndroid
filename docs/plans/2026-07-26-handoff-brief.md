# 实施交接简报 — cemu_android foundation 接入

> 本文是给接手实施的 AI 的开场简报。可直接作为首轮 prompt 使用。
> 读完本文后再读 spec 开工，**不要跳过 spec**。

---

## 一句话任务

在 `/Users/bytedance/workspace/cemu_android` 这个 Cemu 模拟器的 Android 移植 fork 上，沿 `basic_version + 官方 main` 主线继续完成 foundation 公共能力接入；当前先收尾 C5 性能工具，再进入 C6 XR 设计门槛。

---

## 你需要知道的项目背景

**Cemu** 是 Wii U 模拟器（C++20 + CMake）。本仓库是它的 **Android 移植分支 fork**，托管在内部镜像 `tencentmalos/CemuAndroid`，Android 产品主线是 `feature/malos/basic_version`。本地 `main` 跟踪官方 `upstream/main`，同步后推到内部镜像 `origin/main`，再 merge 到 `basic_version`。旧 `android-port` 只作为 Android 改动的可选来源，不再作为并列主线。Android 工程在 `src/android`（Kotlin/Compose + JNI），核心 C++ 在 `src/`；第三方库由仓库内钉住的 `tencentmalos` 子模块集中提供。

**foundation**（`spatial::*`）是一个内部私有库，从另一个模拟器项目 azahar 沉淀出来的通用设施：调试命令总线（debugbus）、Tracy profiler、ImGui layer、OpenXR 框架、Android 平台工具等。它以 git submodule 形式挂在 `dependencies/foundation`。本项目是它的第二个消费方。

**长期目标**是让 Cemu 在 Android XR 头显上运行。当前进度：debugbus 已完成
C4 单进程加固，C5 profiler 与 foundation ImGui 状态层已完成真机验证，XR 尚未开始。

---

## 当前状态（2026-07-26 核对）

三件事你必须先知道，它们直接决定第一步做什么：

1. **C1 上游同步已经完成，但已有下一次边界更新。** 本地 `main`、`origin/main` 为 `b8f2cf4b`；`fc884596` 已把完整 main 历史 merge 到 `feature/malos/basic_version`，main 现在是该分支的祖先。截至 2026-08-01，官方 `upstream/main` 领先 1 个 UI 提交 `1706e5f3`，应在当前依赖改造提交完成后按 `main → basic_version` 顺序同步。同步前 mac 构建失败是合法基线；当前 macOS 与 Android RelWithDebInfo 均已通过。
2. **C2 foundation 正规化与依赖收敛已经完成。** 根 CMake 通过 `add_subdirectory(dependencies/foundation EXCLUDE_FROM_ALL)` 注册真实 target，已删除伪造 target；Android 与桌面端都按真实消费者链接 debugbus/profiler/ImGui。Cemu 自带的 `dependencies/imgui` 已移除，核心 ImGui 只由 `spatial::foundation_imgui` 提供；Vulkan、Metal、OpenGL 共用 foundation layer 生命周期。以后 XR 也必须复用这套 layer/上下文，不得从 Cemu overlay 再分叉一套 UI 栈。core/network/profiler/XR 都是后续计划能力，必须保留。C2 后续参考 Azahar，把 Boost、SDL3、wxWidgets、curl/LibreSSL、libzip、zlib-ng、fmt、glslang、zstd、libusb、Crypto++ 等切到 `tencentmalos` 子模块，并复用 foundation 的 RapidJSON；vcpkg 子模块、manifest、toolchain helper、overlay ports 和 CI cache action 已完整移除。
3. **mac 桌面构建基线已经建立并复验。** C1 证据在 `docs/verification/20260726-C1/`，C2 接入取舍、前后度量和负向测试在 `docs/verification/20260726-C2/`。这些只证明对应提交可验，不代表后续改动可以靠推断跳过复验。

另外：C3 已在约定范围内完成。`RelWithDebInfo` 默认关闭 LTO，Release 仍默认
开启；全局 Unity Build 默认开启，并统一控制 Cemu 与 foundation；ccache
检测到时默认开启；PCH 默认关闭。PCH 关闭后 ccache 二次 clean build 的
264 次编译中命中 262 次（99.24%），wall time 为 10.17s。C3-T4 按维护者决定
暂缓，Vulkan、libusb、SDL 等现有默认构建项没有裁剪。

另外：C4 已按维护者决策实施。Android Debug/Release 都采用与 Azahar 相同
方向的单 App 进程模型，不再为 Release 单独创建 `:EmulationProcess`；游戏退出
只关闭 title，不结束 App 进程。debugbus 跟随 App 生命周期并在 Release 保留，
Service 不导出且受 signature permission 保护，R8 keep 规则已补齐。Debug 真机
已用临时 WUHB 完成 20 轮 pause/resume，Release 已验证 manifest、R8/JNI 和
空闲态命令；设备未安装正式游戏，因此 Release 的运行中 title 控制仍需以后
随正式游戏补验。证据见 `docs/verification/20260726-C4/debugbus-hardening.md`。

---

## 阶段一：你的任务

三个阶段，**顺序不可换**：

| | 阶段 | 做什么 | 为什么是这个顺序 |
| --- | --- | --- | --- |
| **C1（已完成）** | 上游同步 | 官方 main 更新 59 个提交，并把完整 main 历史 merge 到 `basic_version` | 这些提交含 mac 构建修复和 C3 所需的 `ENABLE_OPENGL`/`ENABLE_VULKAN`/`ENABLE_LIBUSB`/SDL optional 开关 |
| **C2（已完成）** | foundation 合入正规化 | 根 CMake 按需接入，去掉伪造 target，加 API 版本护栏 | 先建立真实 target 关系与度量基线；`EXCLUDE_FROM_ALL` 保证未使用组件不污染默认构建图 |
| **C2-F（已完成）** | 依赖收敛 | 复用 Azahar / `tencentmalos` 子模块并完整移除 vcpkg | 保留完整 foundation 能力，同时让 macOS/Android 使用同一套版本钉住的依赖图 |
| **C3（已完成约定范围）** | 编译加速 | Dev 关 LTO → Unity Build → ccache → 默认关 PCH；构建图裁剪暂缓 | foundation 的未使用组件已按需排除；保留现有产品能力默认值 |

**主要验收面是 mac 桌面**，不依赖 Android 真机。Android 构建回归统一使用 `assembleRelWithDebInfo`，保证 Native 是优化且带调试信息的构建，同时 APK 保持 debuggable/run-as。

阶段二 C4（Android debugbus 加固）已完成实现并完成当前设备可用范围内的真机
验证。C5 的 profiler 命令、共享 CPU/Vulkan GPU Tracy 埋点、性能 counters、FPS
概要面板和桌面 TCP transport 已实现；macOS 多客户端/重连实测通过，Android
RelWithDebInfo 已用 `adb install -r` 覆盖安装并在 BOTW v208 / DLC v80 上完成
Tracy CPU+Vulkan GPU 实采。foundation `Statistics` layer 状态面板已在游戏内肉眼
验证，显示 FPS、帧时间、CPU、RAM、renderer 与 draw calls；目前固定显示，旧
`Summary=false` 或位置 Disabled 不会隐藏它，Disabled 回退到左上角，显隐控制留待
后续单独实现。C5-T1 的 debugbus
截图命令仍未实现；C6/C7 是 XR，含未决设计，**不要盲目实施**。

---

## 必读文档（按此顺序）

1. **`docs/plans/2026-07-26-foundation-integration-spec.md`** — 实施 spec，任务级步骤、验证命令、退出标准。**这是你的主要工作文档。**
2. `docs/plans/2026-07-26-foundation-integration-plan.md` — 计划，讲背景与"为什么这么排"。遇到不理解的取舍时查它。
3. `AGENTS.md` / `CLAUDE.md`（仓库根）— 项目规范，优先级高于本文。
4. `dependencies/foundation/docs/guides/build-acceleration.md` — C3 的操作手册，逐条对照执行。
5. `dependencies/foundation/docs/guides/integrating-emulator-host.md` — C2 的接入指南。
6. `BUILD.md` §macOS — mac 构建前置（注意需要 MoltenVK **privateapi** 版，brew 上的那个不行）。

`skills/` 下还有构建验证、故障分析和性能采集等仓库专用文档，Claude Code 不会自动加载，需要时主动 Read。

---

## 硬约束（违反会导致返工）

1. **不提交未跟踪的杂物。** 当前未跟踪的 `_out/`、`build_baseline/` 不属于任务提交。每次提交前核对 `git status`。
2. **以 `basic_version` + 官方 main 为主线。** 官方更新先进入本地/内部镜像 `main`，再 merge 到 `feature/malos/basic_version`；不 rebase 已推送历史。`android-port` 只按需选择性 cherry-pick 或合并，不能成为同步前置或平行版本。
3. **子模块 URL 必须指向 `git@github.com:tencentmalos/...`**，不能被上游改回官方 URL。改 `.gitmodules` 后必跑 `git submodule sync --recursive`。
4. **不直接改 `dependencies/` 下的子模块内容。** 要改 foundation，在 foundation 仓单独提交。
5. **不要在 cemu 侧继续伪造或删除 foundation 能力。** core/network/profiler/ImGui/XR 后续都会使用；Cemu 不再保留独立 ImGui core，未来 XR 必须复用 foundation layer。当前应靠 `EXCLUDE_FROM_ALL` 控制构建闭包，而不是因暂时未链接就删代码或 target。
6. **编译加速每一刀独立提交、独立验证**，以 ninja build edges + `.ninja_log` 为准，wall time 只作参考。
7. **验证要真跑。** 退出标准里的命令必须实际执行并保留输出。跑不了就明说跑不了，**不要用推断替代验证结果**。

---

## 什么时候必须停下来问人

spec §3 列了 7 个决策点（D1–D7）。D5/D6 已决定；后续仍可能遇到的是：

- **D1** — 内部 `origin/main` 出现官方没有的提交，或更新镜像时 push 被权限拒绝。
- **D2** — 上游 SDL3 迁移引入新子模块时，需要先在 tencentmalos 建镜像才能合。
- **D3** — 后续启用 core/network/profiler/XR 等 foundation 组件时，如果其传递依赖导致构建时长/体积不可接受，是否推动 foundation 侧进一步组件化。
- **D4** — 以后恢复 C3-T4 构建图裁剪时，哪些子系统必须保留默认开启。当前
  已决定暂缓裁剪；恢复前仍必须重新确认，不能凭代码猜。
- **D7** — C6 帧路径选型。BotW 立体 VR 目标要求 Vulkan path，进入实现前仍
  要完成 V0 可行性门槛和正式确认。

遇到这些：停下来说明情况并提问，**不要自行选择、不要降级验证标准**。

---

## 你的第一个动作

**从尚未完成的 C5-T1 screenshot 命令开始；若不做该命令，则先进入 C6 的设计
门槛，不要直接写 XR 实现。不要回退单进程、Release debugbus 或 foundation-only
ImGui 决策。**

先确认交接点没有漂移，再阅读 C4 任务与 C3 最终验证：

```sh
git status --short --branch
git fetch upstream main
git fetch origin main feature/malos/basic_version
git rev-list --left-right --count upstream/main...origin/main
git merge-base --is-ancestor main feature/malos/basic_version
sed -n '1,300p' docs/verification/20260726-C3/build-speedup-notes.md
sed -n '1,260p' docs/verification/20260726-C4/debugbus-hardening.md
sed -n '1,280p' docs/verification/20260801-C5/performance-profiler.md
sed -n '1,840p' docs/architecture/cemu-frame-performance.md
sed -n '1,420p' docs/architecture/cemu-aot-assessment.md
sed -n '550,585p' docs/plans/2026-07-26-foundation-integration-spec.md
```

镜像差异按 spec C1-T1/T2 处理；ancestor 检查必须返回 0。若 main 有更新，
先按 C1-T6 merge 到 `basic_version`。D5 已决定采用单 App 进程，D6 已决定
Release 保留 debugbus；不要重新引入独立游戏进程。先提交当前依赖/性能/UI
改造，并按阶段边界同步上述 1 个官方 UI 提交。Android 运行验证统一使用 Native
RelWithDebInfo、APK debuggable 的变体并覆盖安装；不要卸载或清数据。C5 profiler、
warmup 与 FPS 面板已有真机证据，不要重复写成待验证。

---

## 报告方式

每完成一个任务，报告：执行了哪些命令、实际输出是什么、退出标准逐条是否满足。**未全部满足就不要声称任务完成，也不要进入下一个任务。**
