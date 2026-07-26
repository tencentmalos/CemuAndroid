# 实施交接简报 — cemu_android foundation 接入

> 本文是给接手实施的 AI 的开场简报。可直接作为首轮 prompt 使用。
> 读完本文后再读 spec 开工，**不要跳过 spec**。

---

## 一句话任务

在 `/Users/bytedance/workspace/cemu_android` 这个 Cemu 模拟器的 Android 移植 fork 上，完成阶段一：**同步官方 Cemu 最新代码 → 把 foundation 库正规化接入 → 做编译加速**，验收目标是 **macOS 桌面版能配置、能编译、能启动、能验证，且编译速度可接受**。

---

## 你需要知道的项目背景

**Cemu** 是 Wii U 模拟器（C++20，CMake + vcpkg）。本仓库是它的 **Android 移植分支 fork**，托管在内部镜像 `tencentmalos/CemuAndroid`，Android 产品主线是 `feature/malos/basic_version`。本地 `main` 跟踪官方 `upstream/main`，同步后推到内部镜像 `origin/main`，再 merge 到 `basic_version`。旧 `android-port` 只作为 Android 改动的可选来源，不再作为并列主线。Android 工程在 `src/android`（Kotlin/Compose + JNI），核心 C++ 在 `src/`。

**foundation**（`spatial::*`）是一个内部私有库，从另一个模拟器项目 azahar 沉淀出来的通用设施：调试命令总线（debugbus）、OpenXR 框架、Android 平台工具等。它以 git submodule 形式挂在 `dependencies/foundation`。本项目是它的第二个消费方。

**长期目标**是让 Cemu 在 Android XR 头显上运行。当前进度：debugbus 已做过一次试点接入（commit `914917ce`，真机验证过），XR 尚未开始。

---

## 当前状态（2026-07-26 核对）

三件事你必须先知道，它们直接决定第一步做什么：

1. **C1 上游同步已经完成。** 本地 `main`、`origin/main` 和官方 `upstream/main` 均为 `b8f2cf4b`；`fc884596` 已把完整 main 历史 merge 到 `feature/malos/basic_version`，main 现在是该分支的祖先。同步前 mac 构建失败是合法基线；同步和兼容修复后 mac 配置、编译、启动以及 Android `assembleDebug` / `testDebugUnitTest` 均已通过。
2. **foundation 是 hack 接入的。** 现在的 `CMakeLists.txt:219-229` 伪造了两个空的 INTERFACE target 顶替 foundation 的真实依赖，然后只 `add_subdirectory` 了它的一个子模块。这挡住了后续所有扩展。
3. **mac 桌面构建基线已经建立。** 同步前失败、同步后成功的命令与结果在 `docs/verification/20260726-C1/`。这只证明 C1 的当前提交可验，不代表后续改动可以靠推断跳过复验。

另外：编译加速一项没做（`RelWithDebInfo` 还开着 LTO、无 ccache 且本机未安装、Unity Build 完全未用），而 foundation 自带的编译加速手册从未被消费过。

---

## 阶段一：你的任务

三个阶段，**顺序不可换**：

| | 阶段 | 做什么 | 为什么是这个顺序 |
| --- | --- | --- | --- |
| **C1（已完成）** | 上游同步 | 官方 main 更新 59 个提交，并把完整 main 历史 merge 到 `basic_version` | 这些提交含 mac 构建修复和 C3 所需的 `ENABLE_OPENGL`/`ENABLE_VULKAN`/`ENABLE_LIBUSB`/SDL optional 开关 |
| **C2** | foundation 合入正规化 | 改用根 CMake 接入，去掉伪造 target，加 API 版本护栏 | 必须在 C3 前：C2 会把 foundation 全量第三方依赖拉进构建图，先合入再加速，度量基线才有意义 |
| **C3** | 编译加速 | Dev 关 LTO → ccache → Unity Build → 裁剪构建图 | 同时是 C2 的兜底：如果全量接入把构建时长推爆，裁剪就是解法 |

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
5. **不要在 cemu 侧继续伪造 foundation target。** 那正是 C2 要消灭的东西。
6. **编译加速每一刀独立提交、独立验证**，以 ninja build edges + `.ninja_log` 为准，wall time 只作参考。
7. **验证要真跑。** 退出标准里的命令必须实际执行并保留输出。跑不了就明说跑不了，**不要用推断替代验证结果**。

---

## 什么时候必须停下来问人

spec §3 列了 7 个决策点（D1–D7）。阶段一仍可能遇到的是这四个：

- **D1** — 内部 `origin/main` 出现官方没有的提交，或更新镜像时 push 被权限拒绝。
- **D2** — 上游 SDL3 迁移引入新子模块时，需要先在 tencentmalos 建镜像才能合。
- **D3** — C2 后如果 foundation 全量依赖导致构建时长/体积不可接受，是否推动 foundation 侧加模块开关。
- **D4** — C3 裁剪构建图时，哪些子系统必须保留默认开启。**这个不能凭代码猜，需要产品侧确认。**

遇到这些：停下来说明情况并提问，**不要自行选择、不要降级验证标准**。

---

## 你的第一个动作

**从 C2-T1 开始：把 foundation 改为根 CMake 接入。**

先确认交接点没有漂移，再阅读现有 hack 和 foundation 接入指南：

```sh
git status --short --branch
git fetch upstream origin
git rev-list --left-right --count upstream/main...origin/main
git merge-base --is-ancestor main feature/malos/basic_version
sed -n '200,245p' CMakeLists.txt
sed -n '1,240p' dependencies/foundation/docs/guides/integrating-emulator-host.md
```

镜像差异按 spec C1-T1/T2 处理；ancestor 检查必须返回 0。若 main 有更新，
先按 C1-T6 merge 到 `basic_version`。随后执行 C2-T1，不要跳到编译加速。

---

## 报告方式

每完成一个任务，报告：执行了哪些命令、实际输出是什么、退出标准逐条是否满足。**未全部满足就不要声称任务完成，也不要进入下一个任务。**
