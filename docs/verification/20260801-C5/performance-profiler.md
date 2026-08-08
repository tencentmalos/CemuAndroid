# C5 profiler / performance verification — 2026-08-01

## Scope

This record covers the shared C++ profiler integration, desktop debugbus TCP
transport, Android RelWithDebInfo build, a live Profiler MCP capture over the
Tracy 0.10 protocol, and the foundation ImGui performance status layer. The
debugbus `screenshot` command remains a separate unimplemented C5-T1 item; the
status-layer evidence below was captured with `adb exec-out screencap`.

## Dependency pins

- foundation: `be67a11` (`imgui: release global context on shutdown`), including
  the earlier `bb8d669` profiler connection refresh, pushed to
  `git@github.com:tencentmalos/foundation.git` `main`.
- Boost: `tencentmalos/ext-boost`, branch `cemu-1.90`, pin
  `0ea3da9e9e595a42107af2a144aaf767d6d8744f` (Boost 1.90).

## macOS build

Command:

```sh
cmake --build build_no_vcpkg --parallel 8
```

Result: passed. The final target was
`bin/Cemu_relwithdebinfo` (Mach-O arm64). The remaining linker output only
reported already-known duplicate static archive arguments; there were no link
errors.

## Desktop debugbus runtime smoke test

A temporary portable copy of the newly built binary was launched with an
isolated port so the already-running user instance was not interrupted:

```sh
CEMU_DEBUGBUS_PORT=45991 /tmp/cemu-debugbus-smoke.*/Cemu_relwithdebinfo
```

Observed command response:

```text
Cemu debugbus ready
cemu_debug_status:
native_debugbus=true
emulator_hash=39ec7c55
title_running=false
title_paused=false
profiler_backend=tracy
profiler_connected=false
debugbus_port=45991
.
profiler_backend=tracy
active_profiler_endpoint=tcp:8086
profiler_endpoint=tcp:8428
profilerstudy_endpoint=tcp:8428
profilerstudy_port=8428
framepro_endpoint=tcp:8428
framepro_port=8428
tracy_endpoint=tcp:8086
tracy_port=8086
.
```

The same build then passed a two-client test plus a third connection after the
first client disconnected:

```text
desktop_debugbus_multi_client=passed
smoke_process_exit=0
```

This test exposed and verified fixes for three foundation runtime defects:

1. the async listener no longer requires a JobSystem merely to construct;
2. accept callbacks execute outside the listener mutex and can safely re-arm;
3. debugbus registers its persistent read callback once instead of recursively
   re-registering before the current buffer drains.

After adding the fast-launch commands, a fresh temporary copy of the macOS
binary was also started on `CEMU_DEBUGBUS_PORT=45992`. Its live `help` response
listed `open_last_game`, `warmup_a`, `warmup_status`, and `warmup_cancel`;
`warmup_status` returned the legal idle state and the process exited cleanly.

## Android RelWithDebInfo build

Command:

```sh
cd src/android
./gradlew testDebugUnitTest assembleRelWithDebInfo --no-daemon
```

Result:

```text
> Task :app:testDebugUnitTest UP-TO-DATE
> Task :app:assembleRelWithDebInfo
BUILD SUCCESSFUL in 25s
68 actionable tasks: 8 executed, 60 up-to-date
```

The NDK printed its pre-existing inconsistent-location warning for
`ndk;28.2.13676358`; it did not fail the build. Active native configuration:

```text
CMAKE_BUILD_TYPE:STRING=RelWithDebInfo
```

APK metadata:

```text
package: name='info.cemu.cemu' versionCode='1' versionName='39ec7c55'
application-debuggable
native-code: 'arm64-v8a' 'armeabi-v7a' 'x86' 'x86_64'
```

The arm64 RelWithDebInfo native object contains the expected shared profiler
markers:

```text
localabstract:cemu-tracy
latte.command_buffer.decode
cemu.guest_quanta_per_frame
cemu.guest_jit_entries_per_frame
cemu.guest_interpreter_instructions_per_frame
vulkan.draw
vulkan.present_blit
cemu.fps_milli
cemu.frame_time_us
cemu.cpu_usage_milli_percent
cemu.ram_bytes
```

No active macOS or Android CMake cache contains `dependencies/vcpkg` or
`vcpkg_installed`.

## Live Android Tracy capture

Device `01108YHE01017563` (`AYANEO Pocket DS`) received the RelWithDebInfo APK by
an in-place `adb install -r`; application data was preserved. The APK used for
the live Tracy capture had SHA-256:

```text
22712ec834f0210c9650ee44a7d0953f94ee3e4884ea2aff587b81fc6118985d
```

Profiler MCP was started before launching the title:

```text
url=android://localabstract:cemu-tracy
protocol=tracy
duration_seconds=40
keep_session=true
session_id=s2
```

While MCP was connected, debugbus observed `profiler_connected=true`, then
`open_last_game` launched the recorded BOTW WUA and
`warmup_a 10 5000 1000 60` completed all 10 A-button pulses. The final status
reported `title_running=true`; the process remained PID `19477` and logcat had no
new fatal signal or Vulkan submit crash.

The loaded WUA was visually at the Chinese BOTW main menu and had previously
been verified as base `v0`, update `v208`, and DLC `v80`.

### MCP capture facts

```text
Tracy version:       0.10.0 (protocol 64)
frames:              514
CPU zones:           351394
GPU contexts:        1
GPU zones:           49328
resolved GPU-time:   49327
threads:             6
```

Observed CPU hotspots included `latte.command_buffer.decode`,
`espresso.ppc_quantum`, `vulkan.draw.prepare`, `vulkan.submit`,
`vulkan.present_blit.prepare`, and `vulkan.pipeline_cache.create`.

The structured GPU-zone result resolved both required markers with actual
Tracy GPU timestamps:

| GPU zone | Count | Total GPU time | Maximum |
| --- | ---: | ---: | ---: |
| `vulkan.draw` | 48,811 | 967.836 ms | 2.097 ms |
| `vulkan.present_blit` | 516 | 185.598 ms | 2.097 ms |

All required plots produced live samples: `cemu.fps_milli`,
`cemu.frame_time_us`, `cemu.draws_per_frame`,
`cemu.cpu_usage_milli_percent`, and `cemu.ram_bytes`. During the stable portion
of this launch/menu capture, FPS samples were roughly 19.8–29.8 FPS, frame time
roughly 33.5–50.5 ms, draw calls roughly 98–113 per frame, Cemu CPU roughly
24.9–48.7%, and RAM rose to approximately 1.54 GB. The initial FPS/frame-time
sample reflects title startup and is not used as a steady-state value.

The first attempt exposed that foundation only refreshed its connection flag on
an emulated frame; `bb8d669` makes the app-lifetime status query refresh the
backend directly. A subsequent attempt exposed that Tracy's Vulkan context
constructor submits its command buffer; passing Cemu's already-recording buffer
crashed the Adreno driver. Cemu now allocates a dedicated one-shot initialization
command buffer. The final capture above verifies both fixes together.

The later macOS compile check found that the warmup worker must remain C++17
compatible. After replacing `std::jthread` with `std::thread` plus atomic
cancellation, both platforms were rebuilt. The post-warmup-fix Android APK SHA-256 was
`6f4efaa5f4514e2b8f7745261040f356a648301ca6551bc868071df37a9209c2`;
an in-place install preserved the recorded game path, `open_last_game` launched
it, a short warmup completed 3/3, and a delayed warmup was cancelled with final
state `cancelled`. The app remained alive at PID `21757` with no fatal signal.

## Foundation ImGui status layer

Cemu's `dependencies/imgui` submodule and direct ImGui core source compilation
were removed. `src/imgui` now retains only Cemu's renderer backend glue and links
`spatial::foundation_imgui`. Foundation is now an unconditional dependency; the
obsolete build option and its conditional code paths were removed.
Configuration fails directly when the foundation submodule is missing.

The renderer creates TV/Pad main and `Statistics` foundation layers with isolated
contexts backed by the `LayerManager` shared font atlas. Vulkan uses separate
in-flight buffer rings for main/status draw data. Metal completion and shutdown
use the originating context, and OpenGL renders the same main/status layer pair.
Future XR overlays are required to reuse this foundation lifecycle rather than
add another Cemu ImGui stack.

The latest macOS RelWithDebInfo build explicitly enabled Metal, OpenGL, and
Vulkan and compiled all three backend paths successfully. The final Android
RelWithDebInfo APK had SHA-256:

```text
a053bb06c2c9f80a130b3ce1a4f69e57968ad69311fef516a77a621464e5fec1
```

The final incremental package run was successful in 29s (41 actionable tasks:
8 executed, 33 up-to-date). It was installed with `adb install -r` on device
`01108YHE01017563`; application
data was preserved. The overlay settings file was copied to an explicit backup,
the summary was temporarily enabled, and `open_last_game` plus a 3-pulse warmup
completed with `title_running=true`. The screenshot
`docs/verification/20260801-C5/foundation-imgui-status.png` visibly contains the
Chinese BOTW main menu and foundation status panel. The panel reported internal
version `v208`, 28.1 FPS, 35.57 ms, 32.7% CPU, 1473 MB RAM, Vulkan, and 112 draws
(5 fast) in that frame. Logcat contained no fatal exception, fatal signal, or
abort message.

After capture the app was force-stopped, the original settings were restored and
byte-compared with the backup, and the temporary backup was removed. Final
settings again contained `<Position>0</Position>` and
`<Summary>false</Summary>`; the user's configuration was not left modified.

## Slow-load warmup and always-visible status follow-up

The slow-device warmup was changed to wait for title/controller readiness, delay
35 seconds, issue 6 A-button presses held for 250 ms at 10-second intervals, and
then remain in an observable `settling` state for 60 seconds. It reports
`completed` only after that settling window while the title is still running.
The title/controller readiness timeout is 180 seconds. These defaults are shared
C++ behavior rather than an Android-only path.

The final Android build ran `testDebugUnitTest assembleRelWithDebInfo` successfully
in 24 seconds (68 tasks: 8 executed, 60 up-to-date). The debuggable APK with a
native RelWithDebInfo library had SHA-256:

```text
cd23e6d75bc2395ebff93d4e37e541dcee65150ee8bf9eaf16504b5c0b23f38f
```

It was installed with `adb install -r`; app data and the recorded game path were
preserved. No overlay setting was changed. The existing settings remained
`<Position>0</Position>` and `<Summary>false</Summary>`. With those values, the
foundation status layer was visible on the BOTW save-selection screen, through
the loading transition, and in the actual playable Resurrection Shrine scene.
Position Disabled now means a top-left fallback for this layer; the legacy
Summary flag no longer hides it. Explicit visibility control is intentionally
left for a later change.

The no-argument warmup progressed from `running` at 1/6 presses to `settling` at
6/6, then finished with `warmup_state=completed`, `title_running=true`, and
`controller_ready=true`. The final gameplay/status evidence is
`docs/verification/20260801-C5/profiler-current-state.png` (SHA-256
`668424854a5961f690381a75e7057cb7c0d7343282bf898ad8b1674431865994`).
Logcat contained no fatal exception, fatal signal, abort, or app ANR. The game was
left running for interactive validation.

## Current BOTW gameplay profiler analysis

Profiler MCP captured the playable scene for 20 seconds over Tracy 0.10 using
`android://localabstract:cemu-tracy`. The retained MCP session was `s3`; a
local, untracked analysis copy was saved as `_out/profiles/cemu-botw-current.tracy`
(59,736,635 bytes; SHA-256
`5891b138dc05eeeac9487330e845246730eb7d5fb705f174238a2c343967d366`). This is a
normalized MCP-readable Tracy file, not a byte-exact copy: GPU zones and metadata
are preserved, while hardware samples and original CPU hierarchy are not fully
preserved by the writer.

```text
frames:                    198
CPU zones:                 875,932
GPU zones:                 777,827
resolved GPU-time zones:   776,215
CPU-submit fallbacks:        1,612
decoded events:          8,060,435
```

The steady counters reported 9.790–11.927 FPS (mean 10.194), frame time
83.840–102.142 ms (mean 98.266 ms), Cemu CPU 33.522–41.399% (mean 35.496%),
3,860–3,878 draws per frame (mean 3,870.4), and process RAM rising from 2,448 MB
to 2,491 MB. Once Tracy disconnected, the same playable scene's status layer
showed 12.2 FPS / 81.88 ms, 33.5% CPU, 2,493 MB RAM, and 3,866 draws (2,850 fast).
That post-capture comparison is scene-local rather than a calibrated overhead
measurement, but it shows that this very high event volume materially affects
absolute capture FPS.

MCP's frame/GPU correlation classified all 198 frames as CPU-dominant:

| Metric | Mean | p50 | p90 | p95 | Maximum |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU frame | 99.477 ms | 99.736 ms | 101.269 ms | 101.652 ms | 117.223 ms |
| covered Vulkan GPU time | 46.773 ms | 46.137 ms | 50.332 ms | 52.429 ms | 54.526 ms |
| CPU/GPU ratio | 2.137x | 2.151x | 2.351x | 2.382x | 2.761x |

The measured CPU critical path is `LatteThread`, especially command-buffer
decode and per-draw preparation. Over the 198-frame range:

| CPU scope | Total | Approx. per frame | Interpretation |
| --- | ---: | ---: | --- |
| `latte.command_buffer.decode` | 15,943.579 ms | 80.523 ms | dominant inclusive Latte work |
| decode self time | 10,609.099 ms | 53.581 ms | decode/state/dispatch outside named children |
| `vulkan.draw.prepare` | 3,275.281 ms | 16.542 ms | about 3.87k draw preparations per frame |
| `vulkan.submit` | 2,046.175 ms | 10.334 ms | submission/collection path |

Actual GPU work is still a secondary limit: `vulkan.draw` totals 9,196.012 ms
across the range (about 46.45 ms/frame), while `vulkan.present_blit` averages
0.323 ms. Even after removing the current CPU bottleneck, the measured GPU work
would cap this scene near 20–21 FPS without further GPU/render-work reduction.
The recommended optimization order is therefore:

1. reduce `LatteThread` decode/self cost and redundant per-draw state work;
2. reduce the roughly 3.87k draw preparations/submissions per frame or make the
   fast-draw path materially cheaper;
3. then address Vulkan draw workload, because its 46.8 ms average remains above
   a 30 FPS budget.

Do not use the huge aggregate values reported for nested
`espresso.ppc_quantum` scopes: this live import contains malformed/wrapped
nesting timestamps for those scopes. MCP diagnostics also state that callstack
payload/sample export and IP symbolization/CPU-zone attribution are incomplete.
PPC execution may still be material, but this session cannot rank it reliably;
the next CPU pass should first fix/avoid that decoding boundary or use a
symbolized sampling capture.

Frame `#71`（117.223 ms）的 Guest / Host / Vulkan GPU 边界、帧内裁剪、相邻帧
对照、Mermaid 时序图和后续埋点方案已单独整理到
[`cemu-frame-performance.md`](../../architecture/cemu-frame-performance.md)。后续引用
“本次最长帧”时以该文档为准，不要直接把跨帧 scope 的原始完整时长当作帧内占用。

## 最新构建的 warmup 与埋点验证

本轮验证对象是最新本地 RelWithDebInfo 构建及新增的 Guest / Host 分界埋点。CPU 侧
结论以本节为准；但本次 Vulkan timestamp 存在单独的数据有效性问题，因此不能用它
替换上文 `s3` 的历史 GPU 基线，也不能把旧基线当成本次构建的测量结果。

macOS `CemuCafe` 与 Android `assembleRelWithDebInfo` 均构建成功。APK 保持
`android:debuggable=true`，通过 `adb install -r` 覆盖安装，未清除应用配置：

```text
APK:     app-relWithDebInfo.apk
bytes:   113036782
SHA-256: b5d718677ffa2db3b3bb5ed7001a7146889d795b61b987e8ae473459de1b0c5b
```

应用先启动到空闲态，再建立 Tracy 连接；debugbus 返回
`profiler_connected=true` 后才执行 `open_last_game`。随后使用无参数 warmup，完整
走完 35 秒启动等待、以 10 秒间隔发送 6 次 A，以及最后 60 秒稳定等待：

```text
warmup_state=completed
title_running=true
controller_ready=true
completed=6
count=6
```

物理屏幕截图
[`profiler-tag-validation-gameplay.png`](profiler-tag-validation-gameplay.png)
(1920x1080 PNG，SHA-256
`3fe4652fcd2cb9f25996e784b0c0f58f7cd1cce73b0e56317ba70749bb303c2f`)
确认 BOTW v208 已进入初始神庙的实际可操作场景。以下正式结果不包含启动和菜单样本。
本轮 trace 采集时，StatusLayer 中实验性的分辨率字段显示了辅助 render target 尺寸，
因此当时从性能结论中排除。该 UI 问题随后已单独修正：字段改读主窗口/GamePad 窗口
物理尺寸，使用新 APK 完整 warmup 后显示 `1920x1080`，证据见
[`status-resolution-fixed.png`](status-resolution-fixed.png)。这项修正不改变上面的正式
trace、APK 哈希或性能数值。

### 采集标识与数据有效性边界

正式窗口在 warmup 完成且截图确认 gameplay 后采集：

```text
协议:          Tracy 0.10.0 / protocol 64
时长:          20 秒
session:       s6
artifact:      20260801-104819-345-tracy-tracy-live-normalized-only-7057d988
帧:            198 个 frame-set 帧（Guest 逐帧 counter 为 200 个样本）
CPU zones:     1,035,216
GPU zones:     776,674
```

session 已另存为未跟踪的
`_out/profiles/cemu-botw-warmed-tag-validation.tracy`（63,857,743 bytes，SHA-256
`b50170881dda83a2f4e4fb407dc38d685e33bad7b5ed553b78f40491603cdd80`）。该文件由 MCP
Tracy writer 归一化生成，可由 MCP 重新加载，但不是源流的 byte-exact 副本；CPU
hierarchy 与 hardware sample 未完整保留，GPU zone 和 metadata 保留。

仍有 4 个跨越 live capture 边界的 scope 产生了不可能的全段时长：
`latte.command_buffer.decode`、`latte.draw_pass.decode`、`vulkan.submit` 和
`vulkan.submit.end_command_buffer`。本文拒绝使用它们的整段 aggregate；只使用经过
frame 裁剪的结果，以及内部一致的 frame 10–190 有界区间。已移除的
`espresso.ppc_quantum*`、`latte.command_ring.wait_for_guest` 和
`tcl.ring.wait_for_space` zone 没有再次出现，对应的有限语义 counter 已正常出现。

此外，解码结果中出现了一个名称损坏、值恒为 0 的 plot。本文直接忽略，不用推断替代
验证结果。

### 稳态 gameplay 概要

| Counter | 最小值 | 平均值 | 最大值 |
| --- | ---: | ---: | ---: |
| FPS | 9.775 | 10.135 | 11.655 |
| 帧时间 | 85.800 ms | 98.801 ms | 102.300 ms |
| Cemu CPU | 32.613% | 34.852% | 39.930% |
| 每帧 draw 数 | 3,856 | 3,866.8 | 3,876 |
| 进程内存 | 2.498 GB | — | 2.499 GB |

断开 Tracy 后，同一静态场景显示 12.1 FPS / 82.54 ms、33.3% CPU、约 2.500 GB
内存和 3,870 draws（其中 2,851 fast）。这不是严格控制变量的 profiler overhead
实验，但足以给高事件量带来的扰动划定范围：连接 Tracy 时 FPS 约低 16%，帧时间约高
20%。因此绝对 FPS 对比应使用断开 profiler 的运行；trace 适合做同一次采集内部的耗时
归因。

### Guest 工作量与 Host Latte 工作量

新增 Guest counter 的结果有限且数量级合理：

| Guest counter | 200 个样本合计 | 每帧平均 | 含义 |
| --- | ---: | ---: | --- |
| `cemu.guest_quanta_per_frame` | 74,686 | 373.4 | scheduler 执行 quantum 数 |
| `cemu.guest_jit_entries_per_frame` | 74,577 | 372.9 | 进入已有 native block 的次数 |
| `cemu.guest_interpreter_instructions_per_frame` | 1,559,111 | 7,795.6 | interpreter fallback 指令量 |

JIT entry 与 quantum 的比例约为 99.85%，说明稳态场景不是 interpreter fallback
主导。20 秒正式窗口内只有 7 次 recompiler job，合计 40.040 ms；181 帧有界区间内为
6 次、合计 34.274 ms，其中 `optimize_iml` 占 28.639 ms。因此，完整 AOT 或稳态编译
开销都不是当前约 10 FPS 的优先解释。

frame 10–190 有界区间共 181 帧，Host 侧具名耗时如下：

| Host scope | 合计 | 约每帧 | 边界说明 |
| --- | ---: | ---: | --- |
| `latte.command_buffer.decode` | 14,934.303 ms | 82.51 ms | LatteThread inclusive 工作 |
| `latte.sync.async_readback` | 4,488.494 ms | 24.80 ms | GPU readback / fence inclusive 路径 |
| `latte.sync.wait_reg_mem` | 3,737.578 ms | 20.65 ms | register-memory 同步等待 |
| `vulkan.draw.prepare` | 3,107.411 ms | 17.17 ms | 约 3.87k draws 的 CPU 准备工作 |
| `latte.draw_pass.decode` | 2,794.313 ms | 15.44 ms | inclusive；与 submit 子项重叠 |
| `vulkan.submit` | 2,097.478 ms | 11.59 ms | submit 生命周期 inclusive 耗时 |
| `vulkan.submit.recycle_command_buffer` | 1,256.057 ms | 6.94 ms | 最大的 submit 子阶段 |
| `latte.scanbuffer.swap` | 726.748 ms | 4.02 ms | 呈现 / swap 路径 |

Latte command ring counter 记录到 579 次已完成的空闲等待，总计 3,480,999 微秒；按
逐帧 counter 样本折算约 17.4 ms/帧，单次最长 20.66 ms。约 82.5 ms Latte 工作加上
约 17.4 ms 等待 Guest 继续生产命令，与观测到的近 100 ms 帧节奏基本闭合。但该
counter **不能证明 Guest PPC 已经 CPU 饱和**：producer 可能正在执行 Guest 代码、等待
其他依赖，也可能受到有意的 pacing。要区分这些情况，仍需调度 trace 或 fiber-aware
Guest wall-time 测量。

### 最长帧

正式窗口中最长的 4 帧为 118.824 ms（`#22`）、117.531 ms（`#147`）、
115.788 ms（`#28`）和 115.324 ms（`#21`）。`#22` 的帧内拆分如下：

| Frame #22 scope | 帧内耗时 |
| --- | ---: |
| `latte.sync.wait_reg_mem` | 26.426 ms |
| `latte.sync.async_readback` | 25.513 ms |
| `vulkan.command_buffer.wait_for_fence` | 25.505 ms inclusive，7 次等待 |
| `vulkan.draw.prepare` | 18.864 ms，3,814 次调用 |
| `latte.draw_pass.decode` | 16.593 ms，1,071 次调用 |
| `vulkan.submit` | 14.100 ms，12 次 submit |
| `latte.scanbuffer.swap` | 7.549 ms |

`#147` 的结构相同，并不是一次偶发编译尖峰：`wait_reg_mem` 30.689 ms、async
readback 21.878 ms、fence wait 21.867 ms、draw prepare 21.888 ms、submit
12.128 ms。因此最长帧属于可重复出现的“同步等待 + 高 per-draw Host 工作量”。

### 当前 GPU 时间数据的限制

trace 包含 1 个 Vulkan context、773,894 个匹配 GPU timestamp event 的 zone，以及
2,780 个 CPU-submit fallback。但抽查到的 GPU-time zone 起止时间完全相同，耗时均为
0 ms；从标题启动前就连续连接的 trace，其 warmup 后尾段也有同样现象，因此不能只归因
于重连。`s6` 另有 7,342 个未匹配到 zone 的 GPU timestamp event。

因此，本次精确构建能够确认 Host 阻塞在 `wait_for_fence` / async readback，但无法区分
“GPU 实际饱和”和“query/timestamp 埋点失效”。不能把上次 `s3` 的 46.8 ms GPU 估计
冒充为本构建的结果。下一次给出 GPU 结论前，必须验证 Vulkan timestamp period 与 query
生命周期，并优先使用 pass / submit 粒度的 GPU zone，避免每帧近 4,000 个逐 draw
query。

### 本次采集支持的优化顺序

1. 移除或延后同步 async-readback fence wait；它在 LatteThread 上约占
   24.8 ms/帧。
2. 找到 `WAIT_REG_MEM` 的准确 producer / 条件，避免串行轮询或多余 barrier；它约占
   20.7 ms/帧。
3. 在约 3.87k draws/帧的前提下，减少重复 draw prepare、状态更新和 command buffer
   recycle 成本。
4. 围绕约 17.4 ms/帧的 Latte command-ring 空闲补充 queue depth / scheduler 证据，
   在此之前不要直接归因于 Guest CPU。
5. 修复并降低 Vulkan GPU timestamp 埋点量，再用新的完整 warmup 采集重新排序真实 GPU
   pass。

## 退出状态矩阵

| 项目 | 状态 | 证据边界 |
| --- | --- | --- |
| macOS RelWithDebInfo 链接 | 通过 | 实际构建 |
| 桌面 debugbus 监听与命令 | 通过 | 实际 loopback 连接 |
| 桌面多客户端与重连 | 通过 | 实际三客户端 smoke test |
| 桌面快速启动命令 | 通过 | 新构建二进制的 help/status smoke test |
| Android RelWithDebInfo APK | 通过 | 实际 Gradle/NDK 构建 |
| Android Debug 单元测试 | 通过 | `testDebugUnitTest` |
| Android debuggable variant | 通过 | APK metadata |
| Android Tracy endpoint 与 marker | 通过 | Native 二进制字符串 |
| Profiler MCP Tracy 连接 | 通过 | protocol 64：`s2`（启动/菜单）、`s3` 和 `s6`（gameplay） |
| `open_last_game` 与 warmup | 通过 | 保守默认值完成 6/6，并在 settling 后到达实际 gameplay |
| CPU timeline | 通过 | 最新 gameplay `s6`：有界 frame-local scope 与 Guest counter |
| 历史 Vulkan GPU timeline | 通过 | `s3`：776,215 个有效 GPU-time zone，仅作历史基线 |
| 最新构建的 Vulkan 耗时 | 待继续 | timestamp 匹配但 zone 耗时为 0，不能给出 GPU-time 结论 |
| 性能 counter | 通过 | 所需 plot 均返回实时样本 |
| Foundation ImGui 为唯一 core | 通过 | 移除 Cemu 子模块/core source，完成链接与反向配置检查 |
| Metal/OpenGL/Vulkan status backend | 通过 | macOS RelWithDebInfo 显式启用三后端构建 |
| 游戏内 FPS 概要 | 通过 | BOTW v208 gameplay 截图；状态层跨菜单/加载/gameplay 持续显示 |
| 视觉验证后的配置保持 | 通过 | 未改配置；Position 保持 0，Summary 保持 false |
