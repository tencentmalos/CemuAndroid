# RenderDoc MCP 远程分析参考

## 为什么使用 remote replay

Android RDC 依赖 Adreno Vulkan 能力。macOS 本地 `ICaptureFile.OpenCapture` 返回
`APIHardwareUnsupported` 是合法能力边界，不能据此判断抓帧损坏。当前 RenderDoc fork
提供 `RENDERDOC_OpenAndroidCapture`，由 Mac MCP 连接设备上的 replay server，真实状态仍
由 Android Vulkan driver 回放。

## 启动参数

先记录 native commit：

```sh
git -C ~/workspace/renderdoc-for-pico rev-parse HEAD
```

启动 MCP 时设置以下环境和参数；`DEVICE_RDC` 必须是仍存在于设备的绝对路径：

```sh
RENDERDOC_REPLAY_MARKER_PATH="$HOME/workspace/renderdoc-for-pico/renderdoc-mcp-csharp/src/RenderDoc.ReplayMarker/bin/Release/net8.0/osx-arm64/native/RenderDoc.ReplayMarker.dylib" \
RENDERDOC_ARTIFACT_ROOT="$PWD/_out/renderdoc/mcp-artifacts" \
dotnet "$HOME/workspace/renderdoc-for-pico/renderdoc-mcp-csharp/src/RenderDoc.Mcp.Server/bin/Release/net8.0/RenderDoc.Mcp.Server.dll" \
  --renderdoc-library="$HOME/workspace/renderdoc-for-pico/build-mcp-native/lib/librenderdoc.dylib" \
  --abi-manifest="$HOME/workspace/renderdoc-for-pico/renderdoc-mcp-csharp/abi/osx-arm64-pico.json" \
  --native-commit=NATIVE_COMMIT \
  --android-device=SERIAL \
  --android-capture-path=DEVICE_RDC
```

`capture_open.path` 传本机 RDC 路径，作为 session 身份与证据路径；实际 replay 使用上面的
设备路径。ABI manifest 的 commit 与 host library 不一致时必须停止，不能关闭校验。

## 分析顺序

必须串行：

1. `capture_status`，预期 `isOpen=false`；
2. `capture_open`；
3. `frame_overview`、`schema_describe`；
4. SQL 聚合；
5. `texture_list`；
6. 对 SQL 选出的 event 逐个调用 `pipeline_outputs`；
7. `capture_messages`；
8. `capture_close`。

不要并发调用会选择 event 的工具。即使 native replay dispatcher 串行，MCP 工具的
“select 后再读取”也可能在多个请求之间交错，使结果绑定到错误 event。

## 常用 SQL

动作类型和 flags：

```sql
SELECT kind, flags, depth, COUNT(*) AS count,
       SUM(num_indices) AS indices, SUM(num_instances) AS instances
FROM event
GROUP BY kind, flags, depth
ORDER BY count DESC;
```

完整关键动作：

```sql
SELECT event_id, parent_event_id, depth, kind, flags,
       num_indices, num_instances, name
FROM event
ORDER BY event_id;
```

资源类型：

```sql
SELECT type, COUNT(*) AS count, SUM(byte_size) AS total_bytes
FROM resource
GROUP BY type
ORDER BY count DESC;
```

完整 Guest 帧还必须按 queue submit 划分工作量，并统计 render pass 粒度。不同 RenderDoc
版本对 begin/end action 的 `kind`、`flags` 和 `name` 文本可能不同，先用上面的完整动作查询
确认本次 capture 的实际编码，再按相邻 begin/end 维护 pass/submit 区间，输出：

- 每个 render pass 的 draw 数及 `1`、`2`、`3`、`4`、`>4` 分桶；
- 每个 queue submit 的 event 范围、draw、copy 和 render pass 数；
- load/store、attachment identity、readback/copy 或 query 是否解释低 draw pass 的结束；
- 前半段 draw-heavy submit 与帧尾 copy/pass-heavy submit 的占比。

不能只报告整帧 draw 总数。若大量 pass 只有 1～4 个 draw，应把它记录为后续 hazard/
attachment 审计集合，不能在没有 load/store 和资源依赖证据时直接判定它们可合并。

## 专用工具

- `texture_list`：读取 native `TextureDescription`，包括 extent、mips、array、samples、
  byte size、creation flags 和 format fields。
- `pipeline_outputs(eventId)`：串行选择 event 后读取 viewport、color targets 和 depth
  target。Vulkan 负 viewport height 是正常的 Y 翻转表达。
- `capture_messages`：确认 validation/debug message；空列表只表示这一路没有消息。

`pass_list` 依赖 action hierarchy 推导。某些 Android capture 的 render-pass begin/end 与
draw 全部是 root action，因而可能返回空 pass 表；此时用 event flags + pipeline outputs
重建 pass 边界，并记录这是 MCP 派生能力限制。

`mobile_diagnose`、`resource_usage`、GPU counter 等若返回 `needs_materialization`，必须在
报告中保留该状态，不能把空数据解读为无热点或无依赖。

## Android remote EventGPUDuration

当前 C# MCP 的 counter 查询只读取已经 materialize 的数据，不能把空 counter 表解释成
GPU action 时间为零。需要逐 action 的 `EventGPUDuration` 时，使用 repo-local helper 直接
调用 Android remote replay 的 `FetchCounters`：

```sh
RENDERDOC_ROOT="$HOME/workspace/renderdoc-for-pico"
clang++ -std=c++17 -DRENDERDOC_PLATFORM_APPLE \
  -I "$RENDERDOC_ROOT/renderdoc" \
  -I "$RENDERDOC_ROOT/renderdoc/3rdparty" \
  skills/cemu-renderdoc-analysis/scripts/fetch_gpu_durations.cpp \
  -L "$RENDERDOC_ROOT/build-mcp-native/lib" \
  -Wl,-rpath,"$RENDERDOC_ROOT/build-mcp-native/lib" \
  -lrenderdoc -o _out/renderdoc/fetch_gpu_durations

RENDERDOC_REPLAY_MARKER_PATH="$RENDERDOC_ROOT/artifacts/osx-arm64/native/osx-arm64/RenderDoc.ReplayMarker.dylib" \
  _out/renderdoc/fetch_gpu_durations SERIAL DEVICE_RDC gpu-durations.tsv

RENDERDOC_REPLAY_MARKER_PATH="$RENDERDOC_ROOT/artifacts/osx-arm64/native/osx-arm64/RenderDoc.ReplayMarker.dylib" \
  _out/renderdoc/fetch_gpu_durations SERIAL DEVICE_RDC --actions-only actions.tsv

RENDERDOC_REPLAY_MARKER_PATH="$RENDERDOC_ROOT/artifacts/osx-arm64/native/osx-arm64/RenderDoc.ReplayMarker.dylib" \
  _out/renderdoc/fetch_gpu_durations SERIAL DEVICE_RDC --pass-state pass-state.tsv

RENDERDOC_REPLAY_MARKER_PATH="$RENDERDOC_ROOT/artifacts/osx-arm64/native/osx-arm64/RenderDoc.ReplayMarker.dylib" \
  _out/renderdoc/fetch_gpu_durations SERIAL DEVICE_RDC \
  --feedback-shaders feedback-shaders.txt

skills/cemu-renderdoc-analysis/scripts/summarize_render_passes.py \
  --actions actions.tsv --durations gpu-durations.tsv \
  --pass-state pass-state.tsv
```

`DEVICE_RDC` 是设备上的绝对路径，不是本机 pull 回来的文件。输出会给出 draw/copy action
时间、pass 结束原因、one-draw 与不超过四 draw 的 pass 数。`--pass-state` 只选择每个
legacy render pass 的第一个 draw，额外导出 attachment slot、color write mask、viewport、
pixel read-only resource 和实际 feedback 交集；它适合分析 pass 连续性，不能代表 pass 内所有
draw 的 pipeline 都完全相同。`--feedback-shaders` 只反汇编 self-dependency 后首次出现的
不同 pixel shader，用于区分 filtered sampling、texel fetch 与 input-attachment 候选。

RenderDoc 的 action counter
不覆盖 render-pass load/store、barrier、queue idle 等 action 间隔，因此 action duration
总和通常显著小于 Tracy 的 `vulkan.render_pass.guest` wall time。两者是互补证据，不能用
前者否定后者。

若 actions 只有数百个且截图仍在存档确认或菜单页，该 RDC 是合法但无效的性能样本；必须
重新执行 10 次 A、60 秒 settle 并查看截图，不能用 pass 数较少误判优化成功。

## 证据分级

| 级别 | 可写结论 |
| --- | --- |
| RDC + native tool | 某 event 的 viewport/target、某 texture 的实际 extent/format/size |
| RDC + gameplay 截图/StatusLayer | 内部 source 和 output 数值互相吻合 |
| 只有 texture inventory | 资源存在；不能证明本帧写入、读取或 present |
| 源码推导 | 设计解释或下一步假设；不能替代真机事件证据 |

分析结束必须关闭 capture、退出 MCP、停止 Android server，并检查 GPU debug globals 已恢复。

## 双屏设备 remote server 排障

AYN Thor/AYANEO 一类双屏设备在熄屏、keyguard 或焦点落在副屏时，RenderDoc Loader Activity
可能已经启动，但 abstract socket 没有建立。此时 attach 常返回 code 6，不能据此判断 RDC
损坏。先检查：

```sh
adb -s SERIAL shell 'grep renderdoc_ /proc/net/unix || true'
```

如果没有 `@renderdoc_49920`，唤醒设备并重启 server：

```sh
adb -s SERIAL shell input keyevent KEYCODE_WAKEUP
adb -s SERIAL shell wm dismiss-keyguard
adb -s SERIAL shell am force-stop com.picoxr.renderdoccmd.arm64
adb -s SERIAL shell am start -n \
  com.picoxr.renderdoccmd.arm64/.Loader -e renderdoccmd remoteserver
adb -s SERIAL shell 'grep renderdoc_ /proc/net/unix'
```

分析完成后必须 `force-stop` RenderDoc server，并重新把 Cemu 拉到前台；否则 server Activity
会占据双屏焦点，使后续 warmup 看似失效。
