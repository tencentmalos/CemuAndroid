# BotW v208 Host GPU Feedback Observer

这个 P0 probe 在 BotW JP v208 第一次 `GX2DrawDone`（返回 LR `0x031FAA14`）注册
`GuestFeedbackPolicy` v1。Host 仍执行完整 texture/query visibility，仅记录强制 readback
批次的 generation、job/byte 数与地址无关 signature。

它不能带来性能收益，也不能证明上一帧反馈语义正确。运行时必须检查
`guest_feedback_status`：

- `enabled=true`、`mode=observe_full_visibility`；
- `enqueued_boundaries == boundaries`；
- 稳定 gameplay 中 `jobs=3`、`bytes=10752`、`signature_matched=true`；
- `generation_scheduled == generation_published`、`generation_age=0`。

本 probe 与 `BotW v208 Host Readback Deferral` 互斥；后者默认关闭。删除精确 pack 目录并
重启标题即可回退。
