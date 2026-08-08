# BotW v208 Host GPU Feedback

这个 pack 为 BotW JP v208 第一次 `GX2DrawDone`（返回 LR `0x031FAA14`）注册 guarded
previous-generation GPU feedback policy。

该路径目前仍是未通过真机退出标准的实验项，默认关闭。2026-08-07 的 Android 首轮启动
在标题开始执行后触发 `SIGSEGV`；完成 native 队列生命周期与信号链归因前，不得用它替换
已验证的 `Host Readback Deferral` 日常性能基线。

只有同时满足以下条件时，Host 才延迟当前 texture feedback 到第二次 DrawDone：

- 已有上一 generation 发布到 Guest RAM；
- 当前批次精确匹配 3 jobs / 10,752 bytes；
- 三个 job 分别是 `64×8 RGBA32F`、`64×3 RGBA16F` 和 `64×1 RGBA32F`；
- generation 连续。

首帧、加载阶段、signature 变化或 generation gap 都执行完整 texture/query visibility。
query 在快速路径中也保持完整同步。运行时检查 `guest_feedback_status`，稳定 gameplay 应看到
`mode=guarded_previous_generation`、`fast_path` 持续增长、`generation_age` 为 0 或 1；
`fallback` 必须有可解释原因。

它与 observer 和旧 readback deferral pack 互斥。设备验证只安装其中一个精确目录。
