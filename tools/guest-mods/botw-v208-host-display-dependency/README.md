# BotW v208 Host Display Dependency

这是 `docs/architecture/guest-host-synchronization.md` 中 P1 的隔离实验包。它只为
Host 注册 BotW JP v208 的 VSYNC 计数地址 `0x1046D420`，不直接改写计数器，也不替换
Guest 的 VSYNC callback。

身份约束：

- title ID：`00050000101C9300`；
- update：v208；
- `u-king` module checksum：`0x6267BFD0`；
- Cemu Host 必须存在 `gx2.hook_RegisterDisplayOrdinalCounter`。

生效后，只有同时满足以下条件的 `GX2SetGPUFence` 才会转换为 Cemu-only Host event wait：

- fence 地址与已注册地址相同；
- mask 为 `0xFFFFFFFF`；
- Guest compare op 为 `6`（GEQUAL）。

Host 在 Guest VSYNC callback 返回后单次读取计数并发布 ordinal；LatteThread 等待该 Host
event，而不再持续轮询 Guest RAM。其他 fence 完整回退为原 `WAIT_REG_MEM`。

静态审计：

```sh
skills/cemu-guest-game-patching/scripts/audit_graphic_pack.py \
  tools/guest-mods/botw-v208-host-display-dependency \
  --cemu-root . \
  --expected-title 00050000101C9300 \
  --expected-module 0x6267BFD0
```

运行时使用 `display_dependency_status` 检查 `emitted == consumed`、`notifications > 0`，并
确认 `fallback` 只来自不匹配 fence。删除精确 pack 目录并重启标题即可回退；GX2 driver
reset 也会关闭 Host 功能。
