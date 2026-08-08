# BotW v208 Structured Draw Fast Path

这是 `docs/architecture/guest-host-structured-draw-fast-path.md` 的隔离实验包。它不改写
BotW 的 246 个 GX2 draw callsite，只在 JP v208 模块入口调用一次 Cemu 自定义 HLE export，
启用 Host GX2 HLE 中的结构化 draw 编码。

身份约束：

- title ID：`00050000101C9300`；
- update：v208；
- `u-king` module checksum：`0x6267BFD0`；
- Cemu Host 必须存在 `gx2.hook_EnableStructuredDrawFastPath`。

静态审计：

```sh
skills/cemu-guest-game-patching/scripts/audit_graphic_pack.py \
  tools/guest-mods/botw-v208-structured-draw-fast-path \
  --cemu-root . \
  --expected-title 00050000101C9300 \
  --expected-module 0x6267BFD0
```

该包 `default = 1`，仅用于 A/B 探针。安装前备份配置，验证结束后删除精确 pack 目录即可
回退；Host 功能默认关闭，并会在 GX2 driver reset 时恢复关闭。运行时必须用
`structured_draw_status` 验证 emitted/consumed，而不能只用 “patch applied” 日志判断生效。
