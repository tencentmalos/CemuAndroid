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
- foundation 当前 Android Tracy endpoint 名为
  `localabstract:azahar-tracy`。这是 foundation 的兼容接口名，不代表依赖 Azahar
  应用进程。
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
  url="android://localabstract:azahar-tracy",
  protocol="tracy",
  duration_seconds=40,
  keep_session=true
)
```

需要 Tracy GUI 时再手工建立转发：

```sh
adb forward tcp:8086 localabstract:azahar-tracy
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
tracy_endpoint=localabstract:azahar-tracy
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

无参数默认值专门为低帧率、慢加载设备保守设置：标题与 controller 0 就绪后等待
35 秒，发送 6 次 A，每次按住 250ms、按键起点间隔 10 秒，最后再等待 60 秒稳定。
`warmup_a` 的可选参数依次为 A 键次数、标题就绪后的延迟、按键间隔、按下时长和
稳定等待时间，单位均为毫秒；即
`warmup_a [count] [delay_ms] [interval_ms] [press_ms] [settle_ms]`。

命令是非阻塞的；`warmup_status` 会依次报告 `waiting_for_title`、`delaying`、
`running`、`settling` 和 `completed`。只有全部按键结束、稳定等待完成且标题仍运行时
才会进入 `completed`，需要中止时执行 `warmup_cancel`。但模拟器不知道具体游戏的
菜单语义，**有效性能测试仍必须在 `completed` 后截图确认已进入实际可操作游戏画面，
不能只看 `title_running=true` 或 completed 计数。** 推荐先启动 MCP 采集并确认
`profiler_connected=true`，再依次发出 `open_last_game` 和 `warmup_a`，这样 Vulkan GPU
context 会在首个渲染命令缓冲前建立。默认 warmup 最长约 145 秒，采集窗口必须覆盖
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
核对。Vulkan 需要真机肉眼验证；Metal/OpenGL 至少要在启用对应 backend 的 macOS
构建中通过编译。

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

Vulkan GPU timeline 至少检查：

- `vulkan.draw`
- `vulkan.present_blit`

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

线程至少应能辨认 `Cemu Main`、`Latte GPU Thread`、`PPC Core N`、
`PPCRecompiler`、`vkShaderComp` 和 `compilePl`。GPU zone
不存在时依次核对：Tracy 是否在进入渲染前连接、当前 renderer 是否为 Vulkan、
debugbus 是否报告 `profiler_connected=true`。OpenGL/Metal 当前只有共享 CPU/计数器
采集，没有 Vulkan GPU timeline。

不要在 Guest fiber 内用普通 RAII CPU zone 包住 quantum、JIT 或可调度等待：fiber 会
迁移 Host 线程，live trace 会产生跨线程嵌套和异常巨大时长。Guest Mod 的显式 begin/end
应使用 Foundation `ProfilerTagBegin/End`，按 Guest `OSThread` 写入外部虚拟 lane；普通
JIT/interpreter 指标仍优先使用每帧语义 counters。长期 command-ring wait 也只在等待完成
后提交 counter，避免采集结束时留下开放 zone。设计与真机证据见
`docs/architecture/guest-profiler-tags.md` 和
`docs/verification/guest-profiler-tags-android.md`。

## 报告要求

报告必须区分“命令执行成功”“Tracy 已连接”“确实采到 CPU zone”“确实采到 GPU
zone”和“FPS 面板肉眼可见”五种结果。记录 APK variant、Native build type、设备、
renderer、标题、采集时长，以及上面每个检查点的实测结果；未验证项明确写未验证。
Profiler MCP 返回 session id 后，至少再查询 `find_scope_hotspots`、`list_gpu_zones` 和
`list_counters`；工具的 Markdown 摘要可能省略 zone 明细，判断 GPU zone 名称时以
`list_gpu_zones` 的结构化结果为准。高 draw-count 场景会产生大量逐 draw CPU/GPU
zones，Tracy 连接本身可能降低目标 FPS；用采集数据判断瓶颈归属，但绝对 FPS 还要与
断开 profiler 后同场景的状态层读数对照。若 MCP 报告异常巨大的嵌套 zone 时长，先查
`get_import_diagnostics`，不得把时间戳回绕或层级解码异常当作真实热点。
