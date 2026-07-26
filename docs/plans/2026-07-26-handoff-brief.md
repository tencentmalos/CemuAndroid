# 实施交接简报 — cemu_android foundation 接入

> 本文是给接手实施的 AI 的开场简报。可直接作为首轮 prompt 使用。
> 读完本文后再读 spec 开工，**不要跳过 spec**。

---

## 一句话任务

在 `/Users/bytedance/workspace/cemu_android` 这个 Cemu 模拟器的 Android 移植 fork 上，完成阶段一：**同步官方 Cemu 最新代码 → 把 foundation 库按需正规化接入 → 做编译加速**，验收目标是 **macOS 桌面版能配置、能编译、能启动、能验证，且编译速度可接受**。

---

## 你需要知道的项目背景

**Cemu** 是 Wii U 模拟器（C++20，CMake + vcpkg）。本仓库是它的 **Android 移植分支 fork**，托管在内部镜像 `tencentmalos/CemuAndroid`，Android 产品主线是 `feature/malos/basic_version`。本地 `main` 跟踪官方 `upstream/main`，同步后推到内部镜像 `origin/main`，再 merge 到 `basic_version`。旧 `android-port` 只作为 Android 改动的可选来源，不再作为并列主线。Android 工程在 `src/android`（Kotlin/Compose + JNI），核心 C++ 在 `src/`。

**foundation**（`spatial::*`）是一个内部私有库，从另一个模拟器项目 azahar 沉淀出来的通用设施：调试命令总线（debugbus）、OpenXR 框架、Android 平台工具等。它以 git submodule 形式挂在 `dependencies/foundation`。本项目是它的第二个消费方。

**长期目标**是让 Cemu 在 Android XR 头显上运行。当前进度：debugbus 已做过一次试点接入（commit `914917ce`，真机验证过），XR 尚未开始。

---

## 当前状态（2026-07-26 核对）

三件事你必须先知道，它们直接决定第一步做什么：

1. **C1 上游同步已经完成。** 本地 `main`、`origin/main` 和官方 `upstream/main` 均为 `b8f2cf4b`；`fc884596` 已把完整 main 历史 merge 到 `feature/malos/basic_version`，main 现在是该分支的祖先。同步前 mac 构建失败是合法基线；同步和兼容修复后 mac 配置、编译、启动以及 Android `assembleDebug` / `testDebugUnitTest` 均已通过。
2. **C2 foundation 正规化与依赖收敛已经完成。** 根 CMake 通过 `add_subdirectory(dependencies/foundation EXCLUDE_FROM_ALL)` 注册真实 target，已删除伪造 target；Android 当前只构建实际链接的 debugbus/dumpsys。core/network/profiler/XR 都是后续计划能力，必须保留，只是不进入当前默认构建闭包。C2 后续已参考 Azahar，把 fmt、glslang、zstd、libusb、Crypto++ 切到 `tencentmalos` 子模块，并复用 foundation 的 RapidJSON；Crypto++ 没有塞进 vcpkg。
3. **mac 桌面构建基线已经建立并复验。** C1 证据在 `docs/verification/20260726-C1/`，C2 接入取舍、前后度量和负向测试在 `docs/verification/20260726-C2/`。这些只证明对应提交可验，不代表后续改动可以靠推断跳过复验。

另外：C3 已在约定范围内完成。`RelWithDebInfo` 默认关闭 LTO，Release 仍默认
开启；全局 Unity Build 默认开启，并统一控制 Cemu 与 foundation；ccache
检测到时默认开启；PCH 默认关闭。PCH 关闭后 ccache 二次 clean build 的
264 次编译中命中 262 次（99.24%），wall time 为 10.17s。C3-T4 按维护者决定
暂缓，Vulkan、libusb、SDL 等现有默认构建项没有裁剪。

---

## 阶段一：你的任务

三个阶段，**顺序不可换**：

| | 阶段 | 做什么 | 为什么是这个顺序 |
| --- | --- | --- | --- |
| **C1（已完成）** | 上游同步 | 官方 main 更新 59 个提交，并把完整 main 历史 merge 到 `basic_version` | 这些提交含 mac 构建修复和 C3 所需的 `ENABLE_OPENGL`/`ENABLE_VULKAN`/`ENABLE_LIBUSB`/SDL optional 开关 |
| **C2（已完成）** | foundation 合入正规化 | 根 CMake 按需接入，去掉伪造 target，加 API 版本护栏 | 先建立真实 target 关系与度量基线；`EXCLUDE_FROM_ALL` 保证未使用组件不污染默认构建图 |
| **C2-F（已完成）** | 依赖收敛 | 复用 Azahar / `tencentmalos` 子模块，先移出 6 个 vcpkg 直接依赖项 | 保留完整 foundation 能力，同时先消除已有可靠镜像、版本可钉住的重复包管理路径 |
| **C3（已完成约定范围）** | 编译加速 | Dev 关 LTO → Unity Build → ccache → 默认关 PCH；构建图裁剪暂缓 | foundation 的未使用组件已按需排除；保留现有产品能力默认值 |

**验收面是 mac 桌面**，不是 Android 真机。选 mac 的理由：这三项工作的失败模式全在构建系统层面，桌面即可暴露，且不依赖设备、迭代快。Android 在阶段一**只要求不回归**（`assembleDebug` 仍通过）。

阶段二（C4，Android debugbus 加固）才需要真机。C6/C7 是 XR，含未决设计，**现在别碰**。

---

## 必读文档（按此顺序）

1. **`docs/plans/2026-07-26-foundation-integration-spec.md`** — 实施 spec，任务级步骤、验证命令、退出标准。**这是你的主要工作文档。**
2. `docs/plans/2026-07-26-foundation-integration-plan.md` — 计划，讲背景与"为什么这么排"。遇到不理解的取舍时查它。
3. `AGENTS.md` / `CLAUDE.md`（仓库根）— 项目规范，优先级高于本文。
4. `dependencies/foundation/docs/guides/build-acceleration.md` — C3 的操作手册，逐条对照执行。
5. `dependencies/foundation/docs/guides/integrating-emulator-host.md` — C2 的接入指南。
6. `BUILD.md` §macOS — mac 构建前置（注意需要 MoltenVK **privateapi** 版，brew 上的那个不行）。

`skills/` 下还有两个仓库专用文档（构建验证、故障分析），Claude Code 不会自动加载，需要时主动 Read。

---

## 硬约束（违反会导致返工）

1. **不提交未跟踪的杂物。** 当前未跟踪的 `_out/`、`build_baseline/` 不属于任务提交。每次提交前核对 `git status`。
2. **以 `basic_version` + 官方 main 为主线。** 官方更新先进入本地/内部镜像 `main`，再 merge 到 `feature/malos/basic_version`；不 rebase 已推送历史。`android-port` 只按需选择性 cherry-pick 或合并，不能成为同步前置或平行版本。
3. **子模块 URL 必须指向 `git@github.com:tencentmalos/...`**，不能被上游改回官方 URL。改 `.gitmodules` 后必跑 `git submodule sync --recursive`。
4. **不直接改 `dependencies/` 下的子模块内容。** 要改 foundation，在 foundation 仓单独提交。
5. **不要在 cemu 侧继续伪造或删除 foundation 能力。** core/network/profiler/XR 后续都会使用；当前应靠 `EXCLUDE_FROM_ALL` 控制构建闭包，而不是因暂时未链接就删代码或 target。
6. **编译加速每一刀独立提交、独立验证**，以 ninja build edges + `.ninja_log` 为准，wall time 只作参考。
7. **验证要真跑。** 退出标准里的命令必须实际执行并保留输出。跑不了就明说跑不了，**不要用推断替代验证结果**。

---

## 什么时候必须停下来问人

spec §3 列了 7 个决策点（D1–D7）。阶段一仍可能遇到的是这四个：

- **D1** — 内部 `origin/main` 出现官方没有的提交，或更新镜像时 push 被权限拒绝。
- **D2** — 上游 SDL3 迁移引入新子模块时，需要先在 tencentmalos 建镜像才能合。
- **D3** — 后续启用 core/network/profiler/XR 等 foundation 组件时，如果其传递依赖导致构建时长/体积不可接受，是否推动 foundation 侧进一步组件化。
- **D4** — 以后恢复 C3-T4 构建图裁剪时，哪些子系统必须保留默认开启。当前
  已决定暂缓裁剪；恢复前仍必须重新确认，不能凭代码猜。

遇到这些：停下来说明情况并提问，**不要自行选择、不要降级验证标准**。

---

## 你的第一个动作

**从 C4 的 D5/D6 决策开始，不要直接写实现。**

先确认交接点没有漂移，再阅读 C4 任务与 C3 最终验证：

```sh
git status --short --branch
git fetch upstream main
git fetch origin main feature/malos/basic_version
git rev-list --left-right --count upstream/main...origin/main
git merge-base --is-ancestor main feature/malos/basic_version
sed -n '1,300p' docs/verification/20260726-C3/build-speedup-notes.md
sed -n '550,575p' docs/plans/2026-07-26-foundation-integration-spec.md
```

镜像差异按 spec C1-T1/T2 处理；ancestor 检查必须返回 0。若 main 有更新，
先按 C1-T6 merge 到 `basic_version`。随后先确认 D5（Service 进程归属）与
D6（release 是否保留 debugbus），得到维护者决定后再实施 C4。

---

## 报告方式

每完成一个任务，报告：执行了哪些命令、实际输出是什么、退出标准逐条是否满足。**未全部满足就不要声称任务完成，也不要进入下一个任务。**
