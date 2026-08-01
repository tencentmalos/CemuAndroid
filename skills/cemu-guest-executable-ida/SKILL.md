---
name: cemu-guest-executable-ida
description: Export the exact decrypted Wii U Guest RPX currently running in Cemu, validate its identity metadata and hashes, load or convert it for IDA Pro, build and annotate an IDA database, and publish the finalized database through the private reverse-assets submodule. Use for Guest executable extraction, RPX/RPL parsing, IDA database creation or rebuild, version/checksum gating, or BotW Wii U static reverse-engineering setup.
---

# Cemu Guest 可执行文件与 IDA

建立“运行中的字节 → 身份清单 → IDA 数据库”的可复现链路。不要从文件名、游戏版本说明或
外部符号推断二进制身份。

## 先读取

- 阅读 `../../docs/architecture/guest-reverse-debug-mod-pipeline.md` 第 3–7 节。
- 涉及 BotW v208 时读取 `../../games/reverse/botw/wiiu-v208/README.md`。
- 需要构建或启动 Android 时同时使用 `../cemu-android-build-validation/SKILL.md`。
- 后续真机断点与静态关联分别切换到 `../cemu-guest-runtime-debug/SKILL.md` 和
  `../cemu-guest-ida-correlation/SKILL.md`。

## 1. 固定运行基线

记录 title ID、区域、title/update/DLC version、模块名、`moduleMatches`、Cemu commit、构建
variant、设备和 renderer。Android 统一使用 native `RelWithDebInfo` 且 APK
`android:debuggable=true`；覆盖安装，不卸载、不清数据。

必须让目标标题实际运行。提取入口使用 Cemu 已挂载并解密的虚拟文件系统，不能另写一套
title key/update overlay 逻辑。

## 2. 从运行中的 Cemu 导出

先列模块，再导出 updated 主程序：

```sh
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService guest_modules
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService guest_dump_executable updated
```

只有需要比较本体时才额外导出 `base`。从命令输出的精确绝对路径拉取同 stem 的 `.rpx` 和
`.json` 到 `_out/guest-analysis/<game-version>/`。原始 RPX 只留在本地分析目录，不提交、
不上传、不分发。

用本 skill 的校验脚本拒绝身份漂移：

```sh
skills/cemu-guest-executable-ida/scripts/verify_guest_reverse_identity.py \
  --metadata /path/to/guest-executable.json \
  --rpx /path/to/exported.rpx \
  --expected-title-id 00050000101C9300 \
  --expected-title-version 208 \
  --expected-module u-king \
  --expected-module-matches 0x6267BFD0
```

必须同时满足 schema、title/version、RPX size/SHA、模块名、module checksum 和 section
映射。任一项不符即停止建库或复用既有地址。

## 3. 让 IDA 正确识别 RPX

RPX 是 32-bit big-endian PowerPC ELF 变体，包含压缩 section、自定义 relocation 和
imports/exports。首选 `decaf-emu/ida_rpl_loader` 直接加载原始 RPX；`wiiurpxtool -d` 只做
独立解析或 `readelf` 交叉检查。

当前已验证组合：IDA Professional 9.1 arm64、`ida_rpl_loader` commit
`c70dfdde9a98a0bce842e1f28cc551c3021cd5fc`、IDA SDK 9.2 commit
`7acfc0f417a116775012f0f154deb43b62d5a43d`。把 loader 放到 `~/.idapro/loaders/` 并做
ad-hoc codesign 后加载 RPX。

只解压并修改 ELF type 但得到零 segment 不算成功。继续前必须看到 `.text/.rodata/.data/
.bss/.externs` 等有效 segment。

## 4. 建库与静态标定

按以下顺序执行：

1. 选择 32-bit big-endian PowerPC；
2. 对照 metadata 验证 entry point、text/data ranges 和至少三个已知 patch 地址；
3. 完成 auto-analysis，记录 segment/function/string 数；
4. 导入与当前 RPX SHA/module checksum 绑定的符号 CSV；
5. 外部文档/BetterVR/Switch 逆向只标为 `reference`，Wii U 静态控制流支持后升为 `static`；
6. 保存最终 `.i64` 并计算 size/SHA-256；
7. 更新 `identity/analysis-manifest.json`，再次运行校验脚本并传入 `--analysis-manifest` 与
   `--ida-database`。

禁止直接迁移 Switch 地址、ABI、结构 offset 或指令。禁止用函数名相似代替字节和 section
映射验证。

## 5. 保存到 private 子模块

最终数据库进入独立 private GitHub 仓库，由该子仓 Git LFS 管理；父仓只固定 SSH 子模块：

```text
games/reverse/<game>/<platform-version>/ida-database
```

不要向 GitHub public fork 直接推送新 LFS 对象。子仓只提交最终 `.i64`、最小说明和 LFS
属性；不要提交 RPX、解密内容或 `.id0/.id1/.nam/.til`。更新后执行：

```sh
git submodule sync --recursive
git submodule status --recursive
```

报告 private 子仓 commit、父仓 gitlink、RPX SHA、IDB SHA、工具版本和仍未验证的证据等级。
