---
name: cemu-guest-game-patching
description: Use when reverse-engineering Wii U Guest behavior for Cemu, auditing or authoring graphic-pack patch_*.asm files, validating moduleMatches/codecave/HLE imports, porting BetterVR-style hooks, or reproducing and verifying Guest patches on Android or desktop.
---

# Cemu Guest Game Patching

把 Guest 逆向、PPC patch、Host/HLE 桥接和真机验证拆成可证伪的步骤。不要把“补丁已解析/已应用”写成“功能已工作”。

## 先读什么

- 所有任务先读 `references/cemu-guest-patch-model.md`。
- 涉及 BotW、v208 或 BetterVR 时，再读 `references/botw-v208-bettervr.md`。
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

## 制作最小 patch

1. 先选一个无副作用或可恢复的观测点，不从完整功能 hook 起步。
2. 给每个 group 写精确 `moduleMatches`，不要用宽泛 `rpx` 选择器掩盖版本差异。
3. 保存被覆盖的原指令、寄存器约定、LR/CTR、栈和浮点寄存器假设。
4. codecave 中先实现计数/日志/固定返回值，再逐步加入 Host 交互。
5. 对绝对地址记录静态证据、运行时命中证据和回退方式。
6. Guest 数据是 PPC big-endian；Host 指针、Guest 地址和 MPTR 不得混用。

不要提交或分发游戏 RPX、解密内容或版权资源；只保存 patch、符号、哈希和复现步骤。

## 验证 Host/HLE 桥

Cemu 对未知函数导入可能生成 `0xFFD0` unsupported-import trampoline，因此：

- `Applying patch group` 只证明解析、匹配和写入完成；
- 必须开启受控日志或增加明确 counter/tag，证明目标分支实际执行；
- 必须确认回调是已注册 HLE，而不是 unsupported trampoline；
- 必须验证返回寄存器和 Guest 内存副作用符合调用方约定。

先用 no-op/只读回调验证 ABI，再加入 renderer、输入、XR 或物理状态。平台无关逻辑放在共享 C++/foundation 层，Android 只保留生命周期和设备接入。

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
