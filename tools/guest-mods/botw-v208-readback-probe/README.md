# BotW v208 Readback Purpose Probe

这是一个只用于归因的 BotW JP v208 Graphic Pack，不是产品功能。它把 BetterVR 标注的
AutoExposure enable gate 固定为 false，用来验证每帧强制发布到 Guest RAM 的
`64x8 RGBA32F -> 64x3 RGBA16F -> 64x1 RGBA32F` readback 链是否属于自动曝光。

身份约束：

- title ID：`00050000101C9300`；
- update：v208；
- `u-king` module checksum：`0x6267BFD0`；
- patch address：`0x039D99A4`，原指令 `clrlwi r3, r11, 24`。

验证时必须与未安装本 probe 的相同 gameplay、相同 warmup 做 A/B，并同时检查
`surface_scale_families`、`cemu.readback.visibility_batch_*`、画面和 FPS。验证结束后删除
设备上的精确 probe 目录并恢复采集前的 `settings.xml`；不能把“补丁已应用”当成功结论。
