# BetterVR 本地参考入口

本目录保存 BotW-BetterVR 文档的仓库内快照，便于在不依赖外部工作区的情况下阅读和检索。
可执行代码与持续更新的逆向资料不复制到 `docs/`，而是固定为 `references/` 下的 Git 子模块。

## 参考仓库

| 路径 | 当前固定提交 | 用途 | 使用边界 |
| --- | --- | --- | --- |
| `references/BotW-BetterVR` | `0e1053d`（`main`） | BotW v208 graphic-pack、PPC patch、Cemu HLE hook、输入、Vulkan/OpenXR 与 UI 实现参考 | Windows/Cemu 2.6 架构不能直接照搬到 Android；按 Guest、Host/HLE、renderer/XR 三层分别验证 |
| `references/botw` | `7c65472`（`master`） | BotW C/C++ 逆向结果、符号、类型与引擎语义参考 | 目标是 **Switch v1.5.0**，不是 Wii U v208；地址、ABI、结构偏移和指令不能直接复用 |
| `games/reverse/botw/wiiu-v208` | RPX SHA 前缀 `ba58da5b95ce` | Wii U JP v208 identity、runtime 标注与 private IDA 子模块入口 | 只适用于 `00050000101C9300` / `0x6267BFD0`；`.i64` 在 `ida-database` 子模块内由 Git LFS 管理 |

两个子模块使用 `tencentmalos` fork，分别保留与
`Crementif/BotW-BetterVR`、`zeldaret/botw` 的 GitHub fork 关系。本仓库只固定经过审查的提交，
不会自动跟随上游分支移动。

## 文档快照

| 文件 | 来源 | 说明 |
| --- | --- | --- |
| `BotW-BetterVR-README.md` | `/Users/bytedance/workspace/BotW-BetterVR/README.md` | BetterVR 功能、安装、技术概览与构建说明的原文快照 |
| `BotW-BetterVR-Stereo-and-Hooking.md` | `/Users/bytedance/workspace/BotW-BetterVR/docs/` | 基于 `27262df`（v0.9.17）的中文立体渲染与 Hook 分析 |
| `botw-v208-guest-profiler-patch.md` | 本仓库 | v208 PPC profiler 的 20 个 detour、codecave wrapper、HLE/Foundation/Tracy 生效机制与地址表 |
| `../architecture/cemu-graphic-pack-asm.md` | 本仓库 | Cemu `patch_*.asm` 声明、PPC assembler、codecave、import/callback、能力边界与金手指差异 |
| `../architecture/botw-bettervr-android.md` | 本仓库 | Android 迁移边界与现有真机结论 |
| `../architecture/guest-reverse-debug-mod-pipeline.md` | 本仓库 | 从 Guest 二进制提取、IDA 标定、GDB 动态校准到 Mod/profiler 真机验证的通用链路 |
| `../verification/20260801-C5/bettervr-device-probe.md` | 本仓库 | BotW v208 真机 graphic-pack 探针证据 |

前两个文件于 2026-08-01 从外部工作区复制。其中分析文档在外部仓库中尚未提交，
所以特意以文档快照纳入本仓库；后续更新子模块不会覆盖这份快照。

## 初始化与审查更新

```sh
git submodule update --init references/BotW-BetterVR references/botw

git -C references/BotW-BetterVR fetch origin main
git -C references/BotW-BetterVR log --oneline HEAD..origin/main

git -C references/botw fetch origin master
git -C references/botw log --oneline HEAD..origin/master
```

只有在读完新增提交并确认引用价值后，才更新本仓库中的 gitlink。任何从
`references/botw` 转入 Wii U v208 patch 的符号或类型，都必须重新用 v208 RPX、运行日志或
动态探针验证；不得用 Switch 逆向结果替代 Wii U 验证证据。
