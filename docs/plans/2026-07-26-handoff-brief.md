# 实施交接简报 — cemu_android foundation 接入

> 本文是给接手实施的 AI 的开场简报。可直接作为首轮 prompt 使用。
> 读完本文后再读 spec 开工，**不要跳过 spec**。

---

## 一句话任务

在 `/Users/bytedance/workspace/cemu_android` 这个 Cemu 模拟器的 Android 移植 fork 上，完成阶段一：**同步官方 Cemu 最新代码 → 把 foundation 库正规化接入 → 做编译加速**，验收目标是 **macOS 桌面版能配置、能编译、能启动、能验证，且编译速度可接受**。

---

## 你需要知道的项目背景

**Cemu** 是 Wii U 模拟器（C++20，CMake + vcpkg）。本仓库是它的 **Android 移植分支 fork**，托管在内部镜像 `tencentmalos/CemuAndroid`，当前工作分支 `feature/malos/basic_version`。Android 工程在 `src/android`（Kotlin/Compose + JNI），核心 C++ 在 `src/`。

**foundation**（`spatial::*`）是一个内部私有库，从另一个模拟器项目 azahar 沉淀出来的通用设施：调试命令总线（debugbus）、OpenXR 框架、Android 平台工具等。它以 git submodule 形式挂在 `dependencies/foundation`。本项目是它的第二个消费方。

**长期目标**是让 Cemu 在 Android XR 头显上运行。当前进度：debugbus 已做过一次试点接入（commit `914917ce`，真机验证过），XR 尚未开始。

---

## 当前状态（2026-07-26 核对）

三件事你必须先知道，它们直接决定第一步做什么：

1. **上游落后约 3 个月。** `android-port` 落后官方 Cemu 镜像 `origin/main` **31 个提交**，merge-base 停在 2026-04-18。而且仓库**没有配置指向官方 `cemu-project/Cemu` 的 remote**，只有内部镜像。
2. **foundation 是 hack 接入的。** 现在的 `CMakeLists.txt:219-229` 伪造了两个空的 INTERFACE target 顶替 foundation 的真实依赖，然后只 `add_subdirectory` 了它的一个子模块。这挡住了后续所有扩展。
3. **mac 桌面构建没有基线。** 工作区里没有 `build/` 目录，没人验证过 mac 版当前能不能编译。**"mac 版可验"是待验证的目标，不是已知状态。**

另外：编译加速一项没做（`RelWithDebInfo` 还开着 LTO、无 ccache 且本机未安装、Unity Build 完全未用），而 foundation 自带的编译加速手册从未被消费过。

---

## 阶段一：你的任务

三个阶段，**顺序不可换**：

| | 阶段 | 做什么 | 为什么是这个顺序 |
| --- | --- | --- | --- |
| **C1** | 上游同步 | 追平 31 个提交，配置 upstream remote，转为常态前置 | 这批提交含 3 个 mac 构建修复（直接服务本阶段目标），且含 `ENABLE_OPENGL`/`ENABLE_VULKAN`/`ENABLE_LIBUSB`/SDL optional 四个开关——**它们就是 C3 裁剪构建图的现成工具，不用自己造** |
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

1. **不提交未跟踪的杂物。** 仓库现有未跟踪的 `AGENTS.md`、`skills/`、`_out/`。前次试点已因此中断过一次。每次提交前核对 `git status`。
2. **不 rebase 已推送的分支。** `android-port` 被多分支共享，上游汇入一律 merge。
3. **子模块 URL 必须指向 `git@github.com:tencentmalos/...`**，不能被上游改回官方 URL。改 `.gitmodules` 后必跑 `git submodule sync --recursive`。
4. **不直接改 `dependencies/` 下的子模块内容。** 要改 foundation，在 foundation 仓单独提交。
5. **不要在 cemu 侧继续伪造 foundation target。** 那正是 C2 要消灭的东西。
6. **编译加速每一刀独立提交、独立验证**，以 ninja build edges + `.ninja_log` 为准，wall time 只作参考。
7. **验证要真跑。** 退出标准里的命令必须实际执行并保留输出。跑不了就明说跑不了，**不要用推断替代验证结果**。

---

## 什么时候必须停下来问人

spec §3 列了 7 个决策点（D1–D7）。阶段一会遇到的是这三个：

- **D1** — 内部镜像 `origin/main` 落后于官方 `upstream/main` 时（需要镜像写权限，你没有）。
- **D2** — 上游 SDL3 迁移引入新子模块时，需要先在 tencentmalos 建镜像才能合。
- **D3** — C2 后如果 foundation 全量依赖导致构建时长/体积不可接受，是否推动 foundation 侧加模块开关。
- **D4** — C3 裁剪构建图时，哪些子系统必须保留默认开启。**这个不能凭代码猜，需要产品侧确认。**

遇到这些：停下来说明情况并提问，**不要自行选择、不要降级验证标准**。

---

## 你的第一个动作

**C1-T0：在同步之前，先建立 mac 构建基线。**

这一步的目的是拿到对照组——没有它，同步后出问题分不清是上游带来的还是本来就坏的。

```sh
brew install git cmake ninja nasm automake libtool boost
# 外加 MoltenVK privateapi，见 spec §1
cmake -S . -B build_baseline -DCMAKE_BUILD_TYPE=RelWithDebInfo -G Ninja
/usr/bin/time -p cmake --build build_baseline 2>&1 | tee /tmp/baseline_build.log
```

**基线构建失败也是合法结果** —— 记录下来即可，不要在这一步修它。上游那 3 个 mac 修复很可能正好修好它。

记录到 `docs/verification/<YYYYMMDD>-C1/baseline.md`，然后进 spec 的 C1-T1。

---

## 报告方式

每完成一个任务，报告：执行了哪些命令、实际输出是什么、退出标准逐条是否满足。**未全部满足就不要声称任务完成，也不要进入下一个任务。**
