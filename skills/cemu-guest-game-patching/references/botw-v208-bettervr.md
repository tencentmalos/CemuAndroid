# BotW v208 / BetterVR 已知基线

## 仓库内参考来源

- `references/BotW-BetterVR`：BetterVR 的 Guest patch、Host/HLE 与 renderer/XR 实现。
- `references/botw`：BotW C/C++ 逆向的符号、类型与引擎语义参考；目标为 Switch v1.5.0。
- `docs/bettervr/README.md`：复制文档、Android 迁移分析和真机证据的统一索引。

从 `references/botw` 借用的命名或结构必须重新映射并验证到 Wii U v208，不能复用其地址、
ABI、结构偏移或 AArch64 指令。BetterVR 的实现也必须按 Guest、Host/HLE、renderer/XR
三层分别确认，不能只因 graphic pack 成功应用就判定功能可用。

## 当前验证对象

| 项目 | 值 |
| --- | --- |
| Title ID（JP） | `00050000101C9300` |
| Title version | `v208` |
| DLC | `v80` |
| 主模块 | `u-king` |
| Cemu patch checksum | `0x6267BFD0` |
| RPX updated hash | `fb7911ad` |
| RPX base hash | `dcac9927` |

BetterVR 的 v208 groups 使用 `moduleMatches = 0x6267BFD0`，与当前 WUA 运行日志一致。版本匹配只通过了入口门槛，不代表 Host callback 或 XR renderer 已存在。

## BetterVR 资产分层

- `BreathOfTheWild_BetterVR`：30 个 `patch_*.asm`/group、352 个绝对 patch 地址、82 个唯一 import，其中静态审计识别出 66 个自定义 `hook_*`/`log_*` import。
- BetterVR Host DLL：上游文档统计注册 67 个 HLE callback，并维护相机、投影、输入、物理和 UI 状态。
- Vulkan/OpenXR：Windows Vulkan layer 截获 color/depth，再经 D3D12 interop 提交 OpenXR。
- `BreathOfTheWild_Graphics`：3 个 `.asm`、shader override 和分辨率 preset；仓库内原始 `rules.txt` 含 `__BETTERVR_*__`，必须由 launcher 生成实际数值后才能使用。

## 2026-08-01 Android 探针结论

设备为 AYANEO Pocket DS（Adreno 740，Android API 33），Cemu 使用 Android `RelWithDebInfo`。

核心包的最高证据级别是 `executed`，但未达到 `runtime`：

1. v208 checksum 命中；
2. 30 个 group 全部输出 `Applying patch group`；
3. 运行时实际触发 `hook_OSReportToConsole`、`hook_EndCameraSide`、
   `hook_BeginCameraSide`、`hook_InjectXRInput`、`hook_UpdateSettings`；
4. 这些调用全部落入 `Unsupported lib call`；
5. 完整 warmup 后仍卡在 BotW 启动画面，未进入 gameplay。

因此“原始 BetterVR 核心包可在当前 Android 设备工作”为否；准确表述是“Guest assembler/codecave 链路兼容，Host HLE/XR 链路缺失”。

详细证据见 `docs/architecture/botw-bettervr-android.md` 和
`docs/verification/20260801-C5/bettervr-device-probe.md`。

## 移植边界

- 不移植 Windows launcher、注册表、D3D12 interop 或外部 Vulkan layer。
- HLE callback 直接编译进 Cemu 共享 C++，使用 Cemu 自己的 `PPCInterpreter_t`。
- renderer/UI/XR 生命周期以 foundation 为基础，Android 只做系统生命周期和设备接入。
- 当前 flat BOTW 约 12 FPS；完整 BetterVR 会让 Guest render/draw translation 接近双份。在基础性能改善前，只推进可测的 patch/HLE 兼容性，不宣称 VR 可用性。
