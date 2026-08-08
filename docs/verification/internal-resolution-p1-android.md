# Cemu Internal Resolution P1 Android 验证

## 结论

P1 的统一 1x resolution model 已完成 Android Vulkan gameplay 与 Graphic Pack 固定尺寸
回归。`LatteTexture` 的 guest/host extent、Vulkan/Metal 分配、sample scale、MRT、copy、
readback 判断和诊断现在消费同一份 `LatteSurfaceResolutionInfo`；生产 policy 仍固定为
1x，没有增加全局倍率入口。

在 AYANEO Pocket DS 上覆盖安装 RelWithDebInfo APK 后，BotW v208 / DLC v80 能进入初始
神庙 gameplay。StatusLayer 与 debugbus 均报告：

```text
Internal  Native (1x)
Source    1280x720 -> 1280x720
Output    1920x1080
```

本轮只验证行为等价与尺寸决策，没有连接 Tracy，因此 12.3 FPS / 81.00 ms 只作为画面身份，
不形成新的性能结论。

## 实现覆盖

| P1 项 | 本轮结果 |
| --- | --- |
| backend-independent resolver | `LatteSurfaceResolutionPolicy::Resolve()`；active/configured factor 固定 1 |
| Graphic Pack 输入 | 固定 width/height/depth 解析为 `GraphicPackFixed`，不与全局倍率叠乘 |
| Vulkan/Metal 分配 | 两后端统一读取 `GetHostExtent()` |
| guest/host API | `GetGuestExtent()`、`GetHostExtent()`、`GetResolutionInfo()`、`ScaleGuestRectToHost()` |
| sampled scale | 由 guest/host extent 计算，不再直接读取 overwrite width/height |
| 比例兼容 | 对应 mip 的宽、高分别用 `uint64` 交叉相乘，返回结构化 mismatch 原因 |
| rect/scissor | 起点 floor、终点 ceil；copy 与 scissor 不再分别截断宽高 |
| MRT | 只变宽或只变高都会报告 mismatch，修正原先 `&&` 判断 |
| cache/diagnostics | 兼容字段由 host extent 填充；Graphic Pack 来源从 resolution info 读取 |
| readback | 仍保留 P2 前的 resized 拒绝，但判断统一改为 host 与 guest 是否真正不同 |

Header 内的 compile-time policy 测试覆盖：

- 只变宽；
- 只变高；
- `1280x720 -> 1920x1080` 与 `640x360 -> 960x540` 等比例；
- 非整数 Graphic Pack extent `1921x1081`；
- Graphic Pack depth override；
- `3x3` 尾部 mip；
- `3x3 -> 5x5` 的边界 rect 映射。

## 构建验证

### macOS 共享 C++ 编译单元

使用现有 `build_no_vcpkg` RelWithDebInfo 配置，定向编译包含以下实现的 Unity object：

```text
SurfaceResolutionDiagnostics.cpp
LatteTexture.cpp / LatteTextureLegacy.cpp
LatteRenderTarget.cpp / LatteSurfaceCopy.cpp
Vulkan LatteTexture / surface copy
Metal LatteTexture / renderer copy
```

结果全部成功。输出只有仓库既有的 deprecated `sprintf`、foundation template 与 profiler
macro redefine warning，没有本轮新增编译错误。

### Android RelWithDebInfo

```sh
cd src/android
./gradlew assembleRelWithDebInfo
```

结果：`BUILD SUCCESSFUL`。APK：

```text
src/android/app/build/outputs/apk/relWithDebInfo/app-relWithDebInfo.apk
SHA-256 2061ddb930aa7108a39de2d245080de3cb78719115c5e00e851eab16c5740b8b
```

使用 `adb install -r` 覆盖安装成功；没有卸载 App、清数据或修改 `settings.xml`。

## BotW 1x gameplay

设备：AYANEO Pocket DS，serial `01108YHE01017563`。执行：

```text
open_last_game
warmup_a
```

默认 warmup 参数为：标题/controller ready 后延迟 15 秒，6 次 A、间隔 5 秒、按住
250 ms，最后稳定等待 60 秒。第一张截图仍在黑屏 loading，因此没有误判；
`warmup_state=completed` 后第二张截图确认 Link 已进入初始神庙可操作场景。

截图显示：

| 项目 | 实测值 |
| --- | ---: |
| 游戏版本 | v208 |
| Renderer | Vulkan |
| Internal | Native (1x) |
| Source | 1280x720 -> 1280x720 |
| Output | 1920x1080 |
| 截图瞬时 FPS | 12.3 |
| 截图瞬时 frame time | 81.00 ms |
| draws | 3,849（2,836 fast） |

本机截图：

```text
_out/internal-resolution-p1/android-p1-final-1x-gameplay.png
```

## Surface 与 Graphic Pack 回归

无 fixture 的 1x 诊断：

```text
configured_factor=1
active_factor=1
families_scaled=0
copy_scale_conflicts=0
resized_readbacks=0
readback_failures=0
tv_guest_extent=1280x720x1
tv_host_extent=1280x720x1
```

`verify_surface_scale_p0.sh` 返回 `surface scale P0 verification passed`。

随后临时安装仓库内同尺寸 `TextureRedefine` fixture，重启 App、重新进入 gameplay 并完成
相同 warmup。实测：

```text
graphic_pack_fixed_surfaces=13
families_scaled=0
copy_scale_conflicts=0
resized_readbacks=0
readback_failures=0
tv_guest_extent=1280x720x1
tv_host_extent=1280x720x1
```

`REQUIRE_GRAPHIC_PACK=1` 验证同样通过。fixture 随后从设备删除，并重启 App；没有留在
用户的 Graphic Pack 目录。

这份 fixture 证明 Graphic Pack 固定来源、family/conflict 诊断和 1x 优先级链路仍工作；
非整数 extent、depth 与尾部 mip 由 compile-time policy 测试覆盖。它不代表 2x 已实现。

## 后续边界

- 当前 resolver 只会得到 Native 或 GraphicPackFixed；global 2x 尚未进入 production。
- resized readback/native companion、跨倍率 copy、Vulkan/Metal failure fallback 属于 P2。
- Android 1080p swapchain 的 5 张 image 只证明约 39.55 MiB 常驻缓冲，不代表每帧渲染
  5 次 1080p；详见 [`../architecture/renderdoc-graphics-analysis.md`](../architecture/renderdoc-graphics-analysis.md)。
- BotW Guest tag 驱动的 Host 快速通道候选已记录在
  [`../architecture/cemu-frame-performance.md`](../architecture/cemu-frame-performance.md)，本轮没有实施。
