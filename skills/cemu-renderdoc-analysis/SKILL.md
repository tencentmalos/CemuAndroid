---
name: cemu-renderdoc-analysis
description: Use when capturing or analyzing Cemu Android Vulkan frames with RenderDoc for Pico, especially for internal-resolution, render-target, viewport, texture, pass, draw, bandwidth, or final-presentation investigations that require a warmed gameplay frame and reproducible MCP evidence.
---

# Cemu RenderDoc 图形分析

用于在 Android 真机抓取 Cemu Vulkan 帧，并通过 RenderDoc MCP 做可复现的结构化分析。
性能或画质结论必须来自进入实际 gameplay 后的帧；菜单、启动画面和加载卡顿帧只能作为
对照，不能替代目标场景。

## 固定原则

- Cemu Android 统一使用 `relWithDebInfo` APK：Native 为 `RelWithDebInfo`，APK 保持
  debuggable。覆盖安装使用 `adb install -r`，不得卸载、清数据或重建用户配置。
- 抓帧前执行 `open_last_game` 和 warmup，并截图确认角色已进入可操作场景。BotW 默认用
  `warmup_a 10 15000 5000 250 60000`；只看到 `warmup_state=completed` 不足以证明有效。
- RenderDoc server 安装、GPU debug layer 配置和清理由脚本管理。不需要 Root；不得为抓帧
  直接调用裸 `xsu`。若其他工程正在使用设备，先停下来协调。
- macOS 不能本地回放 Android Adreno Vulkan 帧。出现 `APIHardwareUnsupported` 时不要改成
  软件推断，使用 RenderDoc Android remote replay。
- MCP 先调用 `capture_status`，打开后先 `frame_overview` 与 `schema_describe`，再执行 SQL。
  event/resource ID 一律使用十进制字符串。
- replay controller 的当前 event 是共享可变状态。`event_select`、`pipeline_get`、
  `pipeline_outputs` 等查询必须串行，不能并发抽查多个 draw。
- Cemu 的 Guest frame 与 Android WSI present 解耦。性能结构分析必须使用
  `--guest-frame-capture`；普通 present-delimited capture 只适合 WSI/present 专项，不能
  因为 RDC 能打开就称为“完整 Cemu 帧”。
- 明确区分“RDC 直接证明”“StatusLayer/截图交叉证明”和“仍待补抓”。资源存在不等于本帧
  对它发生写入，不能用 1920x1080 swapchain 的存在推断已定位最终缩放事件。

## 工作流

### 1. 预检与构建

确认目标设备唯一且在线，Cemu 包名为 `info.cemu.cemu`，当前 APK 可调试。若 Cemu 代码有
变化，从 `src/android` 构建并覆盖安装：

```sh
./gradlew assembleRelWithDebInfo
adb -s SERIAL install -r \
  app/build/outputs/apk/relWithDebInfo/app-relWithDebInfo.apk
```

检查 `~/workspace/renderdoc-for-pico` 的 host library、Android server APK 和 C# MCP。缺失或
源码有变化时，读取 [references/android-capture.md](references/android-capture.md) 后重建。

### 2. 抓取有效 gameplay 帧

从 Cemu 仓库根目录执行：

```sh
skills/cemu-renderdoc-analysis/scripts/capture_android_frame.py \
  --serial SERIAL \
  --warmup-args '10 15000 5000 250 60000' \
  --guest-frame-capture \
  --wait-for-gameplay-confirmation \
  --output-dir _out/renderdoc/CASE_NAME
```

暂停后必须查看 `gameplay-before-capture.png`。确认是目标场景再按 Enter；若仍在菜单，终止
本轮并增加按键次数或等待时间，不能把该 RDC 标成性能证据。

成功目录至少包含：

- `gameplay-before-capture.png`；
- 一个稳定写完并拉回本机的 `.rdc`；
- `capture-manifest.json`，包含设备、warmup、设备/本机路径、字节数与 SHA-256。

manifest 还必须满足：

```text
guest_frame_capture=true
renderdoc_guest_capture_state=completed
end_guest_frame=start_guest_frame+1
```

如果 debugbus 返回 `unavailable`，说明当前 APK 没有取得 RenderDoc App API；应检查 layer
注入与 APK 是否包含 Guest-frame capture 实现，不能静默回退到普通 `CaptureFrame`。
`--capture-frame-count 2` 只用于证明 present 解耦或调查 WSI，不是完整 Guest 帧的替代方案。

脚本在成功和失败时都恢复 GPU debug settings/property、停止 RenderDoc server，并把 Cemu
恢复为普通 MainActivity；不清 Cemu 配置。完整抓帧机制见
[references/android-capture.md](references/android-capture.md)。

### 3. 用 MCP 远程回放

读取 [references/mcp-analysis.md](references/mcp-analysis.md)，启动 C# MCP 时同时指定：

- 当前 host `librenderdoc.dylib`；
- 与 native commit 匹配的 ABI manifest；
- Android serial；
- 仍保留在设备上的绝对 RDC 路径。

最小分析顺序：

1. `capture_status`；
2. `capture_open`；
3. `frame_overview` 与 `schema_describe`；
4. `query_execute` 聚合 event kind/flags、资源类型、render-pass draw 分布和 queue-submit
   区间；
5. `texture_list` 读取真实 extent、format、sample count、byte size；
6. 对代表性 draw 串行调用 `pipeline_outputs`，取得 viewport 与 color/depth target；
7. `capture_messages` 检查 validation/debug message；
8. 调查 pass 碎片时，用 native helper 导出 `EventGPUDuration`、`--actions-only`、
   `--pass-state`；遇到 self-dependency 再导出 `--feedback-shaders`，命令见 reference；
9. 用 gameplay 截图或 StatusLayer 做第二路证据；
10. `capture_close`，退出 MCP，停止 Android RenderDoc server并复查 GPU debug 全局项。

优先用 SQL 缩小范围，再调用专用工具。高成本 counter、pixel history、texture export 只有在
基础结构不足以回答问题时才启用。工具返回 `needs_materialization` 时应明确记录能力边界，
不能把空行当成“没有问题”。

### 4. 输出结论

报告至少包含：

- 设备、分支、APK variant、游戏/场景、warmup 参数；
- RDC 本机路径、设备路径、字节数、SHA-256；
- frame event/draw/resource 概览和 debug message 数；
- render pass 总数、低 draw pass 比例、copy 数与 submit 工作分布；
- pass 结束原因、相邻 attachment 是否相同、feedback resource 与 write mask；
- 关键纹理 extent、byte size、用途线索；
- 关键 event 的 viewport、color/depth target 与对应 texture；
- 直接证据、交叉证据、未覆盖边界三栏；
- 清理结果和可复现命令。

不要仅凭纹理尺寸、资源命名或 Cemu 源码推断实际帧路径；任何未由这次 RDC 覆盖的最终
present、异步 queue 或后续 UI 合成必须标为待补抓。
