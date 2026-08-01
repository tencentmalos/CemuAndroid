---
name: cemu-guest-game-patching
description: Use when reverse-engineering Wii U Guest behavior for Cemu, auditing or authoring graphic-pack patch_*.asm files, validating moduleMatches/codecave/HLE imports, porting BetterVR-style hooks, or reproducing and verifying Guest patches on Android or desktop.
---

# Cemu Guest Game Patching

把 Guest 逆向、PPC patch、Host/HLE 桥接和真机验证拆成可证伪的步骤。不要把“补丁已解析/已应用”写成“功能已工作”。

## 先读什么

- 所有任务先读 `references/cemu-guest-patch-model.md`。
- 涉及 BotW、v208 或 BetterVR 时，再读 `references/botw-v208-bettervr.md`。
- 涉及二进制提取、IDA、动态 debugger、Guest profiler 或完整 Mod 生命周期时，阅读
  `../../docs/architecture/guest-reverse-debug-mod-pipeline.md`。
- 提取 RPX 或建立 IDA 数据库时使用 `../cemu-guest-executable-ida/SKILL.md`；真机断点时使用
  `../cemu-guest-runtime-debug/SKILL.md`；静态/动态地址联合校准时使用
  `../cemu-guest-ida-correlation/SKILL.md`。
- 涉及 Android 构建、warmup 或 Tracy 时，同时遵循仓库中的
  `skills/cemu-android-build-validation/SKILL.md`、
  `skills/cemu-android-analysis/SKILL.md` 和
  `skills/cemu-android-performance/SKILL.md`。

## 选择工作流

- 审计已有 graphic pack：先运行静态审计，再读有风险的 `.asm`。
- 制作新 patch：先建立版本/模块基线，再做最小、可观察、可回退的 patch。
- 迁移外部 mod：分别审计 Guest patch、Host 回调、renderer/XR 三层，不能只复制 graphic pack。
- 真机验证：先备份配置，使用隔离目录装载，完成 A/B 和恢复检查。

## 静态审计

从仓库根目录运行：

```sh
skills/cemu-guest-game-patching/scripts/audit_graphic_pack.py \
  /path/to/graphic-pack \
  --cemu-root . \
  --expected-title 00050000101C9300 \
  --expected-module 0x6267BFD0
```

脚本检查 `rules.txt`、`patch_*.asm`、未展开模板、`moduleMatches`、codecave、
绝对地址和 `import.*`。`native_source_evidence=false` 只表示当前 `src/` 没找到同名
实现；继续检查 Cemu 的导入映射和运行日志，不能仅凭字符串搜索下结论。

## 建立不可替代的版本基线

每次都记录：

1. title ID、region、title version、DLC version；
2. 实际加载的 RPX/RPL 模块名和 Cemu `checksum`；
3. RPX updated/base hash；
4. Cemu commit/构建类型、Host 架构和 renderer；
5. 未装 patch 时能否到达相同场景。

`moduleMatches` 必须来自运行日志或同一份 Guest 模块的计算结果。版本号相同不代表字节布局相同；地址、原指令和控制流至少交叉验证两项。

当前构建可在标题运行后用 `guest_modules` 和 `guest_dump_executable [updated|base]` 导出
模块清单、RPX 与 `cemu.guest-executable.v1` metadata。原始 RPX 只能放在本地分析目录，
不得提交或分发。经维护者明确授权的派生 IDA 数据库放入独立 private 仓库，由 Git LFS
管理，并在 `games/reverse/<game>/<platform-version>/ida-database` 以 SSH 子模块固定提交；
父仓同时提交 identity metadata、建库工具 manifest 和证据等级。不要尝试把新 LFS 对象
直接推入 GitHub public fork。IDA、GDB 和 patch 必须绑定同一个 RPX SHA-256 与 module
checksum。

## 制作最小 patch

1. 先选一个无副作用或可恢复的观测点，不从完整功能 hook 起步。
2. 给每个 group 写精确 `moduleMatches`，不要用宽泛 `rpx` 选择器掩盖版本差异。
3. 保存被覆盖的原指令、寄存器约定、LR/CTR、栈和浮点寄存器假设。
4. codecave 中先实现计数/日志/固定返回值，再逐步加入 Host 交互。
5. 对绝对地址记录静态证据、运行时命中证据和回退方式。
6. Guest 数据是 PPC big-endian；Host 指针、Guest 地址和 MPTR 不得混用。

不要提交或分发原始游戏 RPX、解密内容或其他版权资源；数据库提交仅限用户明确要求的
`games/reverse/` 研究资产，并保留可审计身份和访问边界。

BotW v208 Guest profiler 使用隔离暂存脚本，不复制整套 BetterVR：

```sh
stage_dir="$(mktemp -d)/BotW_v208_Guest_Profiler"
skills/cemu-guest-game-patching/scripts/stage_botw_v208_profiler_pack.sh "$stage_dir"
```

暂存后仍须运行本 skill 的静态审计，并在真机上从 `parsed` 逐级验证。

## 验证 Host/HLE 桥

Cemu 对未知函数导入可能生成 `0xFFD0` unsupported-import trampoline，因此：

- `Applying patch group` 只证明解析、匹配和写入完成；
- 必须开启受控日志或增加明确 counter/tag，证明目标分支实际执行；
- 必须确认回调是已注册 HLE，而不是 unsupported trampoline；
- 必须验证返回寄存器和 Guest 内存副作用符合调用方约定。

先用 no-op/只读回调验证 ABI，再加入 renderer、输入、XR 或物理状态。平台无关逻辑放在共享 C++/foundation 层，Android 只保留生命周期和设备接入。

跨 Guest 调度区间不要使用绑定 Host thread 的普通 RAII profiler scope。begin/end 应以
Guest thread ID 和 section ID 配对，并通过 Foundation `ProfilerTagBegin/End` 写入
`Cemu Guest 0xXXXXXXXX` 虚拟 lane；墙钟时间可能包含调度和等待。使用
`guest_profiler_status` 检查 unmatched/invalid/overflow，并确认
`timeline_tag_begin_count - timeline_tag_end_count == active_spans`，再解释 counter 或 Tracy
scope。不要只在 End 时补写历史 span，嵌套区间会破坏 Tracy 单线程时序。完整边界见
`docs/architecture/guest-profiler-tags.md`。

动态调试复用 Guest PowerPC GDB Remote：`guest_debugger_start [port]`、
`guest_debugger_status`、`guest_debugger_stop`。IDA EA 与 GDB PC 必须经导出 metadata 的
section 映射证明相等或完成换算；Android native LLDB/Build-ID 工具不能替代 Guest debugger。

AI 自动化优先使用 `spatial_debug_tool` 的 `guest_debug.*` MCP，而不是把裸 GDB 会话长时间
留在后台。标准顺序：

1. 完成 warmup 并截图确认 gameplay，再启动 Cemu Guest GDB server；
2. `start_session` 使用精确设备序列号和 session-owned ADB forward；
3. `control pause` 后保存当前 `stopEpoch`，再读 module/thread/register/memory；
4. `bind_ida_database` 同时校验 RPX、`.i64` 和 identity manifest SHA-256；
5. 只使用 `correlate_addresses` 返回的 proof 设置断点；
6. `continue` 后用 event cursor 等待更大的 `stopEpoch`，不要重复解释旧停止事件；
7. 先保存 PC/LR/CTR/r1/r3-r10、Guest thread 和原始内存，再让 IDA 解释；
8. 无论成功、超时或异常都调用 `stop_session`，确认断点为空、Guest 已恢复且只移除了本会话
   的 ADB forward。

所有停止状态都有有界 lease。默认 30 秒足够普通取样；需要多步寄存器/内存采集时可以显式
增加，但不得取消。租约自动恢复是安全行为，不是断点失败。任何读操作都要携带当前
`stopEpoch`；Guest 恢复后旧 epoch 结果只能作为历史证据，不能再控制新状态。Cemu 标题运行
期间不要调用 `guest_debugger_stop` 销毁 server；先清理 MCP 客户端，标题退出后再停 server。

## Android 真机探针

统一使用 `RelWithDebInfo` 和覆盖安装，不卸载应用、不清数据：

```sh
cd src/android
./gradlew assembleRelWithDebInfo
adb install -r app/build/outputs/apk/relWithDebInfo/app-relWithDebInfo.apk
```

用户数据通常在：

```text
/sdcard/Android/data/info.cemu.cemu/files
```

AYANEO Pocket DS 的 scoped storage 若拒绝直接写入，先执行
`adb -s <serial> shell xsu id`，只有返回 `uid=0(root)` 与 `u:r:xsud:s0` 才使用厂商 `xsu`
通道。它不是 Magisk。先把逐行审核过的文件推到 `/data/local/tmp/<exact-probe>`，再用若干
单条 `adb shell xsu mkdir/cp/chown/chmod` 写入精确 probe 目录；不要把网络内容管道给 root，
不要对应用数据根做宽范围删除。完整边界见 `AGENTS.md` 和架构文档第 10 节。

执行顺序：

1. 拉取 `settings.xml`、controller profile 和相关 graphic-pack 目录作为备份。
2. 把探针放入独立的 `graphicPacks/customGraphicPacks/<probe>`，不要覆盖用户原包。
3. 启动应用，执行 `open_last_game`，检查日志中的 title/version/checksum/激活记录。
4. 功能验证使用截图、日志和 counter；性能验证必须完成 `warmup_a` 并截图确认进入可操作 gameplay。
5. 测试结束后停止应用、删除本次创建的精确探针目录、恢复原配置并做一次无 mod 启动检查。

## 结果分级

报告必须明确当前最高证据级别：

1. `parsed`：规则和汇编被解析；
2. `matched`：目标模块 checksum 命中；
3. `applied`：patch/codecave 已写入；
4. `executed`：目标分支或 HLE 回调实际执行；
5. `semantic`：游戏行为符合预期且可回退；
6. `runtime`：目标设备完整场景稳定；
7. `performance`：完整 warmup 后有可比较的 CPU/GPU/帧数据。

逐项给出命令、日志片段、截图/trace 路径和失败边界。没有证据的层级标为“未验证”，不要用推断补齐。
