# BotW v208 Guest Profiler

这是一个隔离的 Guest profiler graphic pack 包装层。PPC 汇编的唯一来源是固定子模块
`references/BotW-BetterVR` 中的
`resources/BreathOfTheWild_BetterVR/patch_debug_PPC_Profiling.asm`；仓库不复制整套
BetterVR，也不引入其 renderer/XR 生命周期。

适用基线：

- Wii U BotW JP/EU/US title ID；
- update v208；
- `u-king` module checksum `0x6267BFD0`；
- Host 已注册 `coreinit.hook_ProfileSectionBegin/End`。

先暂存再审计：

```sh
stage_dir="$(mktemp -d)/BotW_v208_Guest_Profiler"
skills/cemu-guest-game-patching/scripts/stage_botw_v208_profiler_pack.sh "$stage_dir"
skills/cemu-guest-game-patching/scripts/audit_graphic_pack.py \
  "$stage_dir" --cemu-root . \
  --expected-title 00050000101C9300 \
  --expected-module 0x6267BFD0
```

真机上必须先备份配置，再把暂存目录放入独立的
`graphicPacks/customGraphicPacks/BotW_v208_Guest_Profiler`。该包 `default = 1`，装入后应视为
会自动启用；验证结束删除这个精确目录并恢复配置。

`games/reverse/botw/wiiu-v208/symbols/botw-v208-profiler-symbols.csv` 是 IDA 和 GDB 共用的
地址语义表。表中名称来自 BetterVR v208 patch；只有本次 AYANEO gameplay 中实际命中的
hook 才标为 `runtime_ayaneo_20260801`，其余仍保持参考证据等级。
