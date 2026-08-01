# CLAUDE.md

本文件为 Claude Code 提供本仓库的工作指引。通用规范以 `AGENTS.md` 为唯一事实来源，通过下面的导入引入，不要在两处重复维护同样的内容。

@AGENTS.md

## 仓库内参考文档

`skills/` 下存放本项目专用的工作流文档。它们不在 `.claude/skills/` 中，Claude Code 不会自动加载，需要在相关任务开始前主动用 Read 打开：

- 构建、提交前验证、故障排查：`skills/cemu-android-build-validation/SKILL.md`
- 构建失败、adb / logcat、native crash、tombstone、JNI、Gradle 问题分析：`skills/cemu-android-analysis/SKILL.md`
- Android/桌面 CPU + Vulkan GPU Tracy 采集、FPS 概要面板和 debugbus 验证：`skills/cemu-android-performance/SKILL.md`
- Wii U 标题版本检查、汉化资源烘焙和 WUA 打包：`skills/cemu-wua-packaging/SKILL.md`
- Guest 游戏逆向、graphic-pack `patch_*.asm`、codecave/HLE hook 制作与真机 A/B 验证：`skills/cemu-guest-game-patching/SKILL.md`

BotW/BetterVR 工作还应直接使用仓库内固定的 reference 子模块和文档入口：

- BetterVR graphic-pack、PPC patch、HLE hook、输入和 Vulkan/OpenXR 实现：`references/BotW-BetterVR`
- BotW C/C++ 逆向符号、类型和引擎语义：`references/botw`（Switch v1.5.0，不得直接套用 Wii U v208 地址、ABI 或偏移）
- BetterVR 文档快照、Android 迁移分析与真机证据索引：`docs/bettervr/README.md`

新增本项目专用工作流时，仍按 `AGENTS.md` 的约定放入 `skills/`，并在上面的列表中补一条入口。

## 进行中的计划

- foundation 植入与调整：
  - 交接简报（新接手先读这个）：`docs/plans/2026-07-26-handoff-brief.md`
  - 计划（背景、review 结论、阶段划分）：`docs/plans/2026-07-26-foundation-integration-plan.md`
  - 实施 spec（任务级、可直接执行）：`docs/plans/2026-07-26-foundation-integration-spec.md`
  - 当前状态：C1、C2、C3 已在约定范围内完成；本地/内部镜像 `main` 为 `b8f2cf4b`，并已由 `fc884596` 完整 merge 到 `feature/malos/basic_version`。截至 2026-08-01，官方 `upstream/main` 新增了 1 个尚未同步的 UI 提交 `1706e5f3`，应在当前依赖改造提交完成后的阶段边界先进入 `main`，再 merge 到产品分支。foundation 已更新到 `be67a11`，根 CMake 已正规化接入；该历史包含 TCP/debugbus/profiler 修复与 ImGui 全局 context 生命周期修复。依赖收敛已把 Boost、SDL3、wxWidgets、curl/LibreSSL、libzip、zlib-ng、fmt、glslang、zstd、libusb、Crypto++ 等改为 `tencentmalos` 子模块，复用 foundation 的 RapidJSON，并完整移除 vcpkg 子模块、manifest、toolchain helper、overlay ports 和 CI cache action；没有删除后续要用的 core、network、profiler、ImGui、XR 能力。Cemu 自带 `dependencies/imgui` 已移除，renderer status 与未来 XR 只使用 foundation layer。C3 最终默认组合为 RelWithDebInfo/Debug 关闭 LTO、全局 Unity Build 开启、ccache 可用时开启、PCH 关闭；PCH 关闭后 ccache 的二次 clean build 命中 262/264（99.24%），10.17s。C3-T4 按维护者决定暂缓，Vulkan、libusb、SDL 等默认构建项未裁剪。物理移除 vcpkg 后，macOS `RelWithDebInfo` 与 Android `assembleRelWithDebInfo` 已复验通过。C4 已完成单进程实现：各 variant 不再拆分游戏进程，游戏退出只结束 title，debugbus 跟随 App 生命周期并在 Release 以非导出 + signature permission + R8 keep 方式保留。Debug 真机已完成 20 轮 pause/resume；RelWithDebInfo 已运行正式 BOTW，Release 变体的正式游戏 pause/resume 仍是独立验证边界。验证见 `docs/verification/20260726-C4/debugbus-hardening.md`。
  - C5 当前状态：profiler 命令、共享 CPU/Vulkan GPU Tracy 埋点、性能 counters、foundation ImGui FPS 状态层和桌面 TCP transport 已实现。Foundation 已成为无关闭宏/无 fallback 的跨平台硬依赖。Android RelWithDebInfo 已完成 dumpsys、Profiler MCP Tracy CPU+Vulkan GPU 实采、`open_last_game`、warmup 与 BOTW v208 面板可见性验证；Metal/OpenGL/Vulkan 桌面构建路径均通过。BOTW 最长帧、Guest/Host/GPU 交互和新增瓶颈标签见 `docs/architecture/cemu-frame-performance.md`；AOT 成本与 eager JIT 建议见 `docs/architecture/cemu-aot-assessment.md`。C5-T1 debugbus screenshot 仍未实现，calibration 推迟到 C6 之后。验证见 `docs/verification/20260801-C5/performance-profiler.md`。
  - 改动构建系统、`dependencies/foundation` 相关代码、debugbus 或 XR 前先读它们，并按 `AGENTS.md` 的分支主线策略先确认官方 `main` 是否有新提交。
- BotW VR on Android 方案：`docs/plans/2026-07-26-botw-vr-android-plan.md`
  - 性质是方案规划而非实施 spec。2026-08-01 已完成 V0 的真机 graphic-pack 探针：BotW v208 `moduleMatches=0x6267BFD0`、codecave、绝对地址 patch 和展开后的 BetterVR Graphics 子包可进入 gameplay；BetterVR Core 的 30 个 group 虽可 applied，但自定义 HLE 全部缺失，运行时落入 unsupported trampoline 并卡在启动画面。flat 基线仍只有约 12 FPS，投影矩阵语义定位也未完成，所以 V0 总门槛仍未通过，**V0 通过前不启动 V2+ 工程**。架构与证据见 `docs/architecture/botw-bettervr-android.md` 和 `docs/verification/20260801-C5/bettervr-device-probe.md`。
  - 对上面那份 foundation 计划有一个直接输入：只要 BotW VR 是目标，C6 的帧路径选型（D7）应直接选 Vulkan path，Surface path 给不了 per-eye 渲染目标与 multiview。
  - BetterVR 实现和 BotW 逆向参考已固定在 `references/BotW-BetterVR`、`references/botw`；分析文档入口为 `docs/bettervr/README.md`。DolphinXR 对照文档仍位于 `~/workspace/emulations/wii/dolphinxr/docs/DolphinXR-OpenXR-Stereo-and-Frame-Pacing.md`。
- 跨平台成本评估（Wii U / 3DS / NS）与逆向工具链：`docs/plans/2026-07-26-emulator-vr-platform-cost-assessment.md`
  - 含 Cemu 补丁基础设施说明、BetterVR 逆向流程反推、IDA 各平台格式支持与授权门槛。
  - 内有一条独立改进建议：用 IDA 签名替代硬编码地址，把「换游戏版本 = 全部重做」降级为「重跑签名匹配」。

## 常用命令速查

```sh
# 依赖同步 + 桌面构建
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=release -G Ninja
cmake --build build

# Android
cd src/android && ./gradlew assembleRelWithDebInfo
cd src/android && adb install -r app/build/outputs/apk/relWithDebInfo/app-relWithDebInfo.apk
cd src/android && ./gradlew testDebugUnitTest
cd src/android && ./gradlew connectedDebugAndroidTest

# ARM 目标
BUILD_TYPE=release ./build_arm.sh
```

## Claude Code 特定注意事项

- 默认串行执行，不要主动拆分并发 subagent 或启动 workflow，除非用户明确要求（与 `AGENTS.md` 的 Agent 约束一致）。
- 修改代码前先阅读相关模块、脚本和历史提交；沿用现有架构与命名，不要顺手做无关重构或整文件格式化。
- 不要直接改动 `dependencies/` 下的子模块内容；改 `.gitmodules` 后必须 `git submodule sync --recursive` 并用 `git submodule status --recursive` 核对指针。
- 只在用户明确要求时才 commit / push。签名相关只使用环境变量 `ANDROID_STORE_FILE`、`ANDROID_KEY_STORE_PASSWORD`、`ANDROID_KEY_ALIAS`，不得写入仓库。
- 汇报结果时给出实际执行过的命令和输出；没跑成的验证要说明原因和替代检查，不要用推测代替结论。
