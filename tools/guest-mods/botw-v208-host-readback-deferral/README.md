# BotW v208 Host Readback Deferral

这个实验把 BotW JP v208 每帧第一次 `GX2DrawDone`（返回地址 `0x031FAA14`）注册为
“只等待 submission retirement，暂不强制发布 Guest RAM”。第二次、post-swap
`GX2DrawDone`（返回地址 `0x031FAB24`）仍执行完整 texture/query visibility barrier。

它与 BetterVR 在 `0x031FAA10` 直接 NOP 的区别是：第一次 DrawDone 的 command flush、
TCL timestamp 和 Guest retirement wait 全部保留，只延后当前 Cemu Vulkan 路径额外插入的
全量 async readback force-finish。Host 代码不硬编码 callsite；地址和 v208 identity 由本 pack
注册，标题或版本不匹配时 Host 拒绝启用。

运行时必须检查 `draw_done_visibility_status`：`enabled=true`、`guest_lr=0x031FAA14`，并且
稳定 gameplay 中 `deferred` 与 `normal` 都按每帧一次增长。Tracy 需要同时比较
`latte.sync.async_readback`、`vulkan.completion.wait.readback_visibility`、batch jobs/bytes、
Guest DrawDone retirement、FPS 和画面。删除精确 pack 目录并重启标题即可完整回退。
