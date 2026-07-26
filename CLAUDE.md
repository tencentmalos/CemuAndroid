# CLAUDE.md

本文件为 Claude Code 提供本仓库的工作指引。通用规范以 `AGENTS.md` 为唯一事实来源，通过下面的导入引入，不要在两处重复维护同样的内容。

@AGENTS.md

## 仓库内参考文档

`skills/` 下存放本项目专用的工作流文档。它们不在 `.claude/skills/` 中，Claude Code 不会自动加载，需要在相关任务开始前主动用 Read 打开：

- 构建、提交前验证、故障排查：`skills/cemu-android-build-validation/SKILL.md`
- 构建失败、adb / logcat、native crash、tombstone、JNI、Gradle 问题分析：`skills/cemu-android-analysis/SKILL.md`

新增本项目专用工作流时，仍按 `AGENTS.md` 的约定放入 `skills/`，并在上面的列表中补一条入口。

## 进行中的计划

- foundation 植入与调整：
  - 交接简报（新接手先读这个）：`docs/plans/2026-07-26-handoff-brief.md`
  - 计划（背景、review 结论、阶段划分）：`docs/plans/2026-07-26-foundation-integration-plan.md`
  - 实施 spec（任务级、可直接执行）：`docs/plans/2026-07-26-foundation-integration-spec.md`
  - 当前状态：C1 已完成；本地/内部镜像 `main` 与官方 `upstream/main` 均为 `b8f2cf4b`，并已由 `fc884596` 完整 merge 到 `feature/malos/basic_version`。macOS 配置、编译、启动和 Android `assembleDebug` / `testDebugUnitTest` 已通过。下一步是 C2 foundation 合入正规化；foundation 仍钉在 `b01f41c`。
  - 改动构建系统、`dependencies/foundation` 相关代码、debugbus 或 XR 前先读它们，并按 `AGENTS.md` 的分支主线策略先确认官方 `main` 是否有新提交。

## 常用命令速查

```sh
# 依赖同步 + 桌面构建
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=release -G Ninja
cmake --build build

# Android
cd src/android && ./gradlew assembleDebug
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
