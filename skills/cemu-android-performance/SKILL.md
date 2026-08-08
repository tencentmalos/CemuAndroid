---
name: cemu-android-performance
description: Use when profiling Cemu CPU, Vulkan GPU, frame pacing, FPS, memory, or draw-call performance on Android or desktop with the foundation Tracy backend, shared debugbus commands, and the in-app performance summary overlay.
---

# Cemu Performance

用于复现 Cemu 在 Android 与桌面端的性能采集。实现以共享 C++ 路径为准：CPU/GPU
埋点、计数器和 debugbus 命令不能只放在 Android JNI 中；Android 仅负责 dumpsys
传输适配，桌面端使用 loopback TCP。

## 固定基线

- Android 性能结论只使用 `relWithDebInfo` variant。它对应 Native
  `RelWithDebInfo`，APK 保持 `android:debuggable=true`，可用 adb、LLDB 和 `run-as`。
- 不用 Debug APK 做帧率或耗时结论，也不要为采集临时切成完全 Release。
- foundation profiler 默认由 Cemu 选择 `tracy` 后端。桌面端可在进程启动前通过
  `CEMU_PROFILER_BACKEND=tracy` 显式指定；不要把运行后的 debugbus 查询误当成后端
  已重新初始化。
- Cemu 当前 Android Tracy endpoint 固定为 `localabstract:cemu-tracy`。不要沿用
  Azahar/Citron 使用的 `localabstract:azahar-tracy`，否则设备上多个模拟器同时运行时
  会连错进程；Tracy listener 首次 bind 失败后不会在同一进程内重试。采集结果仍须
  核对 `captureProgram`/`processId`
  与 Cemu 包名/PID，不能仅凭 socket 连接成功判断目标正确。若设备确认只供 Cemu
  使用，可先停止残留的 endpoint owner，再重启一次 Cemu App；若设备同时供其他工程
  使用则停下来协调，不能终止对方进程。
- Cemu 的 ImGui core 只来自 `spatial::foundation_imgui`。性能概要使用 foundation
  `Statistics` layer；TV/Pad 主层与状态层共享 manager font atlas，但 context 相互
  隔离。不要恢复 `dependencies/imgui`，未来 XR 也复用同一 layer/backend 路径。
- Vulkan GPU context 只在 Tracy 已连接后延迟创建。为了拿到完整 GPU timeline，先
  连接 Tracy，再进入游戏或触发渲染。

## Android 构建与连接

从仓库根目录执行：

```sh
cd src/android
./gradlew assembleRelWithDebInfo
adb install -r app/build/outputs/apk/relWithDebInfo/app-relWithDebInfo.apk
adb shell am force-stop info.cemu.cemu
adb shell monkey -p info.cemu.cemu 1
```

优先使用 Profiler MCP 直接采集 Tracy 协议。MCP 会临时建立并清理 adb forward，调用时
不需要先手工占用本机端口：

```text
capture_profile(
  url="android://localabstract:cemu-tracy",
  protocol="tracy",
  duration_seconds=40,
  keep_session=true
)
```

返回后必须同时检查 `frameCount>0`、`zoneCount>0`、`plotCount>0`，并在连接期间通过
debugbus 确认 `profiler_connected=true`。只读到 Tracy welcome/header、三项均为 0 的
结果是采集器故障样本，不能用来证明“Cemu 没有工作”或“没有性能热点”。这类样本应丢弃，
重启 Cemu App 后先做短连接自检；仍失败时使用匹配协议版本的 Tracy capture 保存 `.tracy`
再离线分析，不得用截图补齐缺失 timeline。

需要 Tracy GUI 时再手工建立转发：

```sh
adb forward tcp:8086 localabstract:cemu-tracy
adb forward --list
```

在主机 Tracy 前端连接 `127.0.0.1:8086`。采集结束后清理本次转发：

```sh
adb forward --remove tcp:8086
```

不要卸载应用或清数据来解决连接问题；这会破坏用户配置。需要更新 APK 时使用覆盖
安装。

## Android debugbus

服务跟随 App 生命周期，而不是游戏生命周期。应用启动后查询：

```sh
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService status
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService profiler_endpoint
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService profiler_backend
```

预期至少包含：

```text
native_debugbus=true
profiler_backend=tracy
tracy_endpoint=localabstract:cemu-tracy
tracy_port=8086
```

空闲态 `title_running=false` 是合法结果。只有设备上已经启动标题时才验证
`pause`/`resume`，不要用推断替代返回值。

## 快速进入游戏与 warmup

`open_last_game` 读取应用记录的上一次游戏路径。第一次使用前需从游戏列表正常启动
一次；标题已经运行时命令会拒绝重复启动。为了在 Tracy 连接后进入游戏并自动通过
启动菜单，可按以下顺序执行：

```sh
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService status
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService open_last_game
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService warmup_a
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService warmup_status
```

无参数默认值专门为低帧率、慢加载设备保守设置：标题就绪后等待
15 秒，发送 6 次 A，每次按住 250ms、按键起点间隔 5 秒，最后再等待 60 秒稳定。
`warmup_a` 的可选参数依次为 A 键次数、标题就绪后的延迟、按键间隔、按下时长和
稳定等待时间，单位均为毫秒；即
`warmup_a [count] [delay_ms] [interval_ms] [press_ms] [settle_ms]`。

有 controller 0 时使用现有 EmulatedController override，同时把 A 镜像进 VPADRead；
没有 controller 0 时临时创建不落盘的 VPAD，并继续通过 VPADRead 镜像保证按键不受
controller profile 加载时序影响。两条路径都不依赖 Android/adb 输入。`controller_ready`
仅用于说明查询时 controller 0 是否存在，`input_mode` 则报告主要注入路径；排查自动化
时还必须查看 `vpad_reads` 与 `vpad_a_reads`，不能只用 `completed` 推断 Guest 已收到 A。

命令是非阻塞的；`warmup_status` 会依次报告 `waiting_for_title`、`delaying`、
`running`、`settling` 和 `completed`。只有全部按键结束、稳定等待完成且标题仍运行时
才会进入 `completed`，需要中止时执行 `warmup_cancel`。但模拟器不知道具体游戏的
菜单语义，**有效性能测试仍必须在 `completed` 后截图确认已进入实际可操作游戏画面，
不能只看 `title_running=true` 或 completed 计数。** 推荐先启动 MCP 采集并确认
`profiler_connected=true`，再依次发出 `open_last_game` 和 `warmup_a`，这样 Vulkan GPU
context 会在首个渲染命令缓冲前建立。默认 warmup 最长约 100 秒，采集窗口必须覆盖
这段时间，或在进入 gameplay 后再开始正式计时。

## 桌面端采集

在独立、无旧 toolchain 污染的构建目录中构建 RelWithDebInfo，然后启动：

```sh
cmake -S . -B build-performance -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-performance
CEMU_PROFILER_BACKEND=tracy ./bin/Cemu_relwithdebinfo
```

Foundation 是 Cemu 的硬依赖，不再提供条件开关；缺少 foundation 子模块时应直接
修复子模块状态，不能用关闭 profiler/ImGui 路径绕过。

Tracy 连接 `127.0.0.1:8086`。共享 debugbus 默认监听 loopback
`127.0.0.1:45987`：

```sh
printf 'status\nprofiler_endpoint\n' | nc 127.0.0.1 45987
```

如端口冲突，在进程启动前设置 `CEMU_DEBUGBUS_PORT`。不要对外网卡开放该调试端口。

## FPS 概要面板

当前 foundation `Statistics` layer 在 Android 和桌面端都始终显示，不依赖旧 overlay
的 `Performance summary` 或位置开关。旧位置为 `Disabled` 时状态层固定回退到左上角；
非 Disabled 位置、缩放和颜色仍沿用现有设置。`Performance summary` 配置项暂时保留，
但在后续新增明确的显隐控制前不参与状态层可见性判断。

面板应同时显示：

- FPS 与 frame time；
- Cemu CPU 占用和进程 RAM；
- renderer、当前有效渲染分辨率（例如 `1920x1080`）；
- 每帧 draw calls 与 fast draw calls。

foundation 状态层是 FPS/CPU/RAM/draw-call 标量信息的唯一显示方，不再重复显示旧
overlay 的同名单项；per-core、VRAM 和 debug 行仍由各自开关控制。

验证可见性时不要改用户配置；应特意在旧配置为 `<Position>0</Position>`、
`<Summary>false</Summary>` 的情况下截图，确认实际游戏画面和 foundation 状态层同时
存在。若测试缩放、颜色或非 Disabled 位置而必须改设置，仍须先备份
`/sdcard/Android/data/info.cemu.cemu/files/settings.xml`，验证后原样恢复并用 `cmp`
核对。Vulkan 需要真机肉眼验证；Metal 至少要在启用对应 backend 的 macOS 构建中
通过编译。Cemu OpenGL backend 已移除，不再列入验证矩阵。

## Tracy 检查点

CPU timeline 至少检查：

- `latte.command_buffer.decode`
- `latte.sync.wait_reg_mem`、`latte.sync.mem_semaphore_wait`、
  `latte.sync.wait_for_flip`
- `espresso.recompiler.compile`、`espresso.recompiler.generate_iml`、
  `espresso.recompiler.optimize_iml`、`espresso.recompiler.generate_host_code`
- `vulkan.submit` 及 `vulkan.submit.*` 子阶段
- `vulkan.command_buffer.wait_for_fence`
- `vulkan.draw.prepare`
- `vulkan.pipeline_cache.create`
- `vulkan.shader.compile`、`vulkan.pipeline.compile`

启用了 Guest profiler Mod 时，还要检查：

- `guest_profiler_status` 的 `timeline_tag_begin_count - timeline_tag_end_count`
  等于查询瞬间的 `active_spans`；
- `invalid_section_count`、`unmatched_end_count`、`span_overflow_count` 全为 0；
- Tracy 中出现 `Cemu Guest 0xXXXXXXXX` 虚拟线程；
- `PPCSystemTaskPostCalc`、`PPCActorJob0_1` 等 Guest scope 能通过
  `find_top_slices` 查到，而不只是 `cemu.guest.mod.*` counter。
- `cemu.guest.mod.<section>.gx2_submits/gx2_words` 与
  `cemu.guest.mod.unattributed.*`，确认 command buffer 是由哪个 Guest section 发布；
- `cemu.guest.gx2_draw_done.*`、`cemu.guest.gx2_swap_scan_buffers.*` 和
  `cemu.guest.gpu_fence.*` 的次数与 LR。LR 是 Guest call 的返回地址，解释 callsite 时减
  4 字节；不要把 command words 误写成 Guest→Host memcpy 字节数。

Vulkan GPU timeline 至少检查：

- `vulkan.command_buffer.gpu` 根区间；
- `vulkan.readback_copy`、`vulkan.occlusion_query.resolve` 和
  `vulkan.present_blit` 子区间；
- internal resolution 路径启用时再检查 `surface_scale.native_boundary_copy` 与
  `surface_scale.resample`。

查询 GPU 结论时显式使用 `time_source="gpu-time"`，并核对
`gpuTimeZoneCount`、`cpuSubmitGpuZoneCount` 与 import diagnostics。采集首尾缺少匹配
timestamp 的少量 zone 可以使用 CPU-submit fallback，但不能与真 GPU 时长混算。保留
`depth`/`parentId`：command-buffer 是根，readback/query/present 是其子区间，统计 GPU
covered time 时对根区间取 union，不能把子区间再次相加。CPU 侧的
`vulkan.submit`、query visibility wait 或 readback wait 也不能冒充 GPU 执行时间。

同时检查每个 GPU context 的 `flags`、`gpuCpuAlignment`、`calibrationSource`，以及
diagnostics 中的 `gpuCalibrationEventCount`：

- `flags & 1 != 0` 且长采集收到 calibration event，才表示 Vulkan host/device clock
  可以放在同一条绝对 timeline 上；
- `flags == 0` / `initial-query-pair-estimate` 时，GPU duration 仍是硬件 timestamp，
  但逐帧相对位置必须使用 `cpu-submit-time`，不能用原始 GPU absolute clock 推断 CPU/GPU
  critical path；
- Cemu `log.txt` 中的 `Vulkan profiler timestamp calibration` 必须与 context 状态一致。
  当前 Adreno 740 驱动会报告 `api=none hardware=unavailable`，这是合法能力边界，不得伪造
  calibrated flag。

统计根区间时同时报告 `inclusiveZoneMs`、`coveredMs` 和 root `selfMs`。嵌套 zone 的
inclusive sum 会重复计算父子区间；GPU critical path 使用 root union/`coveredMs`，而
“仍未标记的 command-buffer 时间”使用 root self，不能继续把整段 root 都称为监控盲区。
若 `avgGpuZonesPerFrame > 256`，把 attribution 标为高扰动风险，并用 root-only 或抽样明细
重新采集做 A/B。

默认禁止恢复每 draw 一个 GPU zone。BotW 这类高 draw-count 标题会在 10 秒内生成数十万
GPU zone，采样开销足以改变目标行为。需要继续拆分根区间时，优先添加低频、语义明确的
render-phase/batch scope，并先给出每帧 zone 预算。GPU timestamp 应能呈现连续的微秒级
差值；若所有时长异常集中在 `8.388608 ms` 的整数倍，先检查 Tracy decoder 的
`timestampPeriod` 精度，不能把量化结果当成 GPU 硬件行为。

Plots 至少检查：

- `cemu.fps_milli`、`cemu.frame_time_us`
- `cemu.draws_per_frame`
- `cemu.cpu_usage_milli_percent`
- `cemu.ram_bytes`
- `cemu.guest_quanta_per_frame`
- `cemu.guest_jit_entries_per_frame`
- `cemu.guest_interpreter_instructions_per_frame`
- `cemu.gx2_command_pool_wait_count`
- `cemu.gx2_command_pool_last_wait_us`
- `cemu.tcl_ring_wait_count`、`cemu.tcl_ring_last_wait_us`
- `cemu.latte_command_ring_wait_count`、`cemu.latte_command_ring_last_wait_us`
- `cemu.command.guest.submissions_per_frame`、`cemu.command.host.submissions_per_frame`
- `cemu.command.guest.words_per_frame`、`cemu.command.host.words_per_frame`
- `cemu.command.queue.pending_submissions`、`cemu.command.queue.pending_words`
- `cemu.command.host.consume_us_per_frame`、
  `cemu.command.host.draw_translate_us_per_frame`
- `cemu.command.host.changed_set_register_packets_per_frame`、
  `cemu.command.host.redundant_set_register_packets_per_frame`
- `cemu.command.host.vulkan_submits_per_frame`、
  `cemu.command.host.vulkan_threshold_submits_per_frame`、
  `cemu.command.host.vulkan_readback_submits_per_frame`、
  `cemu.command.host.vulkan_other_submits_per_frame`
- `cemu.query.{policy,cpu,gpu,completed,zero_results,nonzero_results,bypassed}`；移动 GPU
  query 实验还要同时检查
  `cemu.guest.query_get.{calls,cpu_calls,gpu_calls,unknown_calls,not_ready,ready_zero,ready_nonzero}`、
  `cemu.guest.query_predication.{begin_calls,end_calls,cpu_calls,gpu_calls,unknown_calls,dont_wait_calls,pixels_must_pass_calls}`、
  `cemu.sync.query.wait_us`、
  `cemu.command.host.vulkan_render_pass_end.query_per_frame` 和 GPU zone
  `vulkan.occlusion_query.resolve`。不能只看 query resolve 的 GPU 时间推断 query 很便宜，
  pass fragmentation、completion wait 与 Guest DrawDone 必须一起比较。

Cemu 默认把 occlusion query 转成 conservative-visible bypass：不创建 Host query，不切
render pass，Guest query end 时写回 1；现代 Host GPU 继续使用自身 depth/Hi-Z/HSR。这个
默认与平台/vendor 无关，`accurate` 只用于兼容性定位和正式 A/B。切换使用：

```sh
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService occlusion_query_policy always_visible
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService occlusion_query_policy accurate
```

命令返回后必须等 `requested=active`、`pending=false`，并在 StatusLayer 核对 `Occlusion` 行。
正常默认应显示 `bypassed / visible`；`accurate (debug)` 不得作为日常性能基线。
对照必须保持同一 gameplay 场景，同时记录 query 类型、draw 数、断开 Tracy 的 FPS，以及
Tracy 内 query pass/resolve、第一次 DrawDone 和 GPU root。BotW v208 的首轮证据与回退边界见
`docs/architecture/mobile-occlusion-query-hsr.md`。

Android gameplay 正确性快验统一按每个目标场景至少 90 秒执行。开始计时前必须完成
`warmup_a 10 15000 5000 250 60000`，并截图确认已经进入可操作 gameplay；结束时核对 PID
未变化、`completed_queries == cpu_queries + gpu_queries`、Guest getter 计数、内存变化，以及
logcat 中没有 native fatal、device lost、KGSL/SMMU fault 或 GPU page fault。10 秒或 40 秒
Tracy 窗口只用于统计稳定帧，不能替代 90 秒正确性验证。只有复现低频问题或专项稳定性任务
明确要求时才延长，不再默认等待 3 或 10 分钟。

线程至少应能辨认 `Cemu Main`、`Latte GPU Thread`、`PPC Core N`、
`PPCRecompiler`、`vkShaderComp` 和 `compilePl`。GPU zone
不存在时依次核对：Tracy 是否在进入渲染前连接、当前 renderer 是否为 Vulkan、
debugbus 是否报告 `profiler_connected=true`。Metal 当前只有共享 CPU/计数器采集，
没有 Vulkan GPU timeline。

不要在 Guest fiber 内用普通 RAII CPU zone 包住 quantum、JIT 或可调度等待：fiber 会
迁移 Host 线程，live trace 会产生跨线程嵌套和异常巨大时长。Guest Mod 的显式 begin/end
应使用 Foundation `ProfilerTagBegin/End`，按 Guest `OSThread` 写入外部虚拟 lane；普通
JIT/interpreter 指标仍优先使用每帧语义 counters。长期 command-ring wait 也只在等待完成
后提交 counter，避免采集结束时留下开放 zone。设计与真机证据见
`docs/architecture/guest-profiler-tags.md` 和
`docs/verification/guest-profiler-tags-android.md`。

启用 Guest GPU command-stream tag 后，还要查询 `guest_profiler_status` 中每个 section 的
`gpu_tag_emitted`、`gpu_tag_consumed`、Host duration、draw/fast-draw/fragment 及全局异常计数。
Tracy 中 Host 消费 lane 使用 `GpuCommand/<section>`，不要与 Guest CPU lane 的同名 section
聚合；二者和 Vulkan GPU zone 可以跨线程、跨帧重叠，不能将 inclusive 时间直接求和。
这组 tag 也是 shadow FrameGraph 的业务归属输入，但不能替代资源读写依赖。设计与解释边界见
`docs/architecture/guest-semantic-framegraph.md`。

需要验证 FrameGraph/RenderPass 重编译模型时，先在进入标题前开启只观测模式：

```sh
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService framegraph_shadow_status on
```

完成 warmup 并截图确认 gameplay 后再查询 `framegraph_shadow_status`。至少核对
`render_nodes` 与同帧 draw 数量级一致，`actual_vulkan_render_passes` 非零，所有 overflow 和
`unclosed_node_fallbacks` 为零，并同时记录 `render_pass_candidates`、各类 barrier 与
`build_us`。Tracy 中对应 counter 为 `cemu.framegraph.shadow.*`。这是 shadow 编译成本与模型
覆盖率测试，不得把开启后的 FPS 当作原执行路径基线，也不得把逻辑 RenderPass 候选直接当成
已经合并的 Vulkan pass。测试结束后执行 `framegraph_shadow_status off`。

验证 P2 Host 状态 mirror store 去重时，完成 warmup 并截图确认 gameplay 后重置固定窗口：

```sh
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService command_translation_status reset
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService command_translation_status
```

至少核对 `register_payload_words`、`register_applied_store_words`、
`register_elided_store_words`、`register_store_elision_milli_ratio` 以及 Context、Resource、
Constant、Sampler、Config 五个 `register_domain.*` 明细。Guest context shadow 写入必须保持，
该指标只表示值未变化的 Host `LatteGPUState` mirror store 被跳过；不得据此推断整个 SET packet、
dirty callback 或状态翻译都已消失。Tracy 的对应逐帧 counter 使用
`cemu.command.host.{register_payload,applied_register_store,elided_register_store}_words_per_frame`
及 `register_store_elision_milli_ratio_per_frame`。BotW v208 证据见
`docs/verification/botw-framegraph-state-dedup-p2.md`。

## PM4/Vulkan 翻译增量化验证

验证 `feature/malos/pm4_optimize` 的 dirty routing、dynamic state 去重、pipeline transition cache
或 descriptor cache 时，必须先按上文完成 warmup 并截图确认 gameplay，再重置固定窗口：

```sh
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService command_translation_status reset
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService command_translation_status
```

至少核对：

- 所有 `dirty_domain.*` 的 classified/mark/consume，尤其
  `dirty_domain.unclassified.classified_words=0`；
- `pipeline_hash_calls` 与 transition/global/miss 三类 lookup，miss 必须与创建或启动阶段对应；
- descriptor hash/hit/miss，不能只看高命中率就推断 hash 工作已经消失；
- blend constants、depth bias 的 requested/emitted/elided，command buffer reset 后首个状态必须重新
  emitted；
- 原有 register store elision、draw/full/fast 数量和画面没有回退。

Tracy 使用 `cemu.command.host.pipeline.*`、`descriptor.*`、`dynamic_state.*`、`dirty_*` 逐帧
counter，并同时比较 `full_draw.pipeline_us_per_frame`、`full_draw.descriptors_us_per_frame` 与
`draw_translate_us_per_frame`。结构计数下降不等于 FPS 收益；同场景至少三个窗口一致后才可宣称
CPU 收益。完整设计和首轮 BotW 证据分别见
`docs/plans/pm4-vulkan-translation-optimization-spec.md` 与
`docs/verification/botw-pm4-vulkan-translation-p1-p3.md`。

## Internal Resolution P0 验证

进入 BotW v208 gameplay 并完成上述 warmup 后，从仓库根目录执行：

```sh
skills/cemu-android-performance/scripts/verify_surface_scale_p0.sh [device-serial]
```

脚本验证 title-scoped 诊断已初始化、active/configured factor 仍为 1、TV guest/host
extent 都为 `1280x720x1`、存在 `SampledToRenderTarget`、readback 和 alias 证据，并要求
1x 下没有 copy scale conflict、resized readback 或 readback failure。surface、family、
transition 与 Graphic Pack conflict 的字段由 foundation reflection payload 统一生成；
脚本只依赖稳定的 `key=value` 边界，不依赖字段输出顺序。

验证 Graphic Pack 固定尺寸时，使用仓库内同尺寸 fixture，不修改 `settings.xml`：

```sh
fixture_dir=/sdcard/Android/data/info.cemu.cemu/files/graphicPacks/InternalResolutionP0NativeFixture
adb shell mkdir -p "$fixture_dir"
adb push \
  skills/cemu-android-performance/fixtures/botw-p0-native-texture-redefine/rules.txt \
  "$fixture_dir/rules.txt"
```

重启 App、重新进入 BotW 并完成 warmup 后执行：

```sh
REQUIRE_GRAPHIC_PACK=1 \
  skills/cemu-android-performance/scripts/verify_surface_scale_p0.sh [device-serial]
```

fixture 只把 `1280x720` 和 `854x480` 重定义为原尺寸，用于证明 Graphic Pack 优先级、
family 标记和冲突诊断，不能据此推断真正的放大渲染已经实现。测试完成后删除设备副本
并重启 App；仓库副本保留用于复现：

```sh
adb shell rm "$fixture_dir/rules.txt"
adb shell rmdir "$fixture_dir"
adb shell am force-stop info.cemu.cemu
adb shell monkey -p info.cemu.cemu 1
```

完整 P0 Android 证据见 `docs/verification/internal-resolution-p0-android.md`。

## 报告要求

报告必须区分“命令执行成功”“Tracy 已连接”“确实采到 CPU zone”“确实采到 GPU
zone”和“FPS 面板肉眼可见”五种结果。记录 APK variant、Native build type、设备、
renderer、标题、采集时长，以及上面每个检查点的实测结果；未验证项明确写未验证。
Profiler MCP 返回 session id 后，至少再查询 `find_scope_hotspots`、`list_gpu_zones` 和
`list_counters`；GPU 分析还要调用 `get_gpu_timeline`，并设置
`include_hierarchy=true`、`time_source="gpu-time"`。工具的 Markdown 摘要可能省略
zone 明细，判断 GPU zone 名称、
时间源和父子关系时以结构化结果为准。Tracy 连接本身可能降低目标 FPS；用采集数据判断
瓶颈归属，但绝对 FPS 还要与断开 profiler 后同场景的状态层读数对照。若 MCP 报告异常
巨大的嵌套 zone 时长、source-location 名称漂移或固定步长量化，先查
`get_import_diagnostics`，不得把时间戳精度、动态 source-location 或层级解码异常当作
真实热点。若 diagnostics 同时显示大量 `hardwareSampleCount`，且 summary 的 `lastTime` 或
CPU zone 达到远超采集时长的 `10^15～10^17 ns`，检查 MCP 是否包含 ProfilerStudy
`ea75c2a`：`QueueHwSample.time` 是绝对 TSC，不能推进 thread delta 时钟。修复前的 artifact
不能用于 CPU hotspot、同帧关联或 critical-path 结论，必须用修复后的 server 重新采集。

ProfilerStudy MCP 源码位于 `~/workspace/spatial_mcp_publish/profiler_study`。当前已安装的 MCP
进程不会热重载源码；需要验证 decoder 修复时，先按该仓说明构建，再直接启动最新
`ProfilerStudy.McpServer/bin/Release/net8.0/ProfilerStudy.McpServer` 做 stdio MCP 采集。修改
该仓前必须遵守其 AGENTS/设计文档与 focused commit 要求，不得把 Cemu 改动混入 MCP 仓库。

分析 `cemu.sync.*` 这类因果 counter 时，以 PM4 携带的 `guest_frame_id` 和
`draw_done_sequence` 为关联键；不能把不同线程上时间最近的 counter 自动视为同一事件。
live artifact 的 `normalized/plots.ndjson` 保留每个样本的 `timeNs` 和值，可按 sequence 顺序
把 DrawDone phase、query mode、wait、retirement gap 与 display ordinal 对齐。报告至少给出：

- phase 1/2 各自的 DrawDone wait 与 retirement gap；
- query mode 0/2 各自的 wait、in-flight、Guest query 和 event gap；
- display 的 `target-initial`、`target-final` 与 wakeup；
- 同一 frame 内 GPU command-buffer root 与上述 CPU wait 的 containment，而不是把平均耗时相加。
