# Cemu 帧性能架构：Guest / Host / GPU 与长帧案例

> 本文分析的是 **2026-08-01、AYANEO Pocket DS、BOTW v208 / DLC v80、
> Vulkan、Android RelWithDebInfo** 这一次 20 秒 Tracy 采集中的最慢帧，即 frame
> `#71`。它不是“Cemu 在所有设备和游戏中的全局最慢帧”。后续比较必须复用相同场景、
> 构建类型和采集方法。

## 1. 结论

Frame `#71` 用时 **117.223 ms**，是本次 198 帧中的最大值。它相对采集平均帧
99.477 ms 慢 17.746 ms（+17.84%），但同帧 Vulkan GPU 覆盖为 46.137 ms，反而比
全段 GPU 平均值低 1.36%。因此这次长帧不是 GPU 时间突然增长造成的。

当前最可信的归因是：

1. **Host CPU 是这一帧的主导侧。** 198/198 帧均由 Profiler MCP 判定为 CPU 时间
   长于 GPU 时间，frame `#71` 的 CPU/GPU 比达到 2.541x。
2. **Host `LatteThread` 是已观测到的关键路径。** `latte.command_buffer.decode` 在
   帧窗口内精确裁剪后的并集覆盖为 89.481 ms，占整帧 76.33%。
3. **逐 draw 数量上升有贡献，但不能独立解释长帧。** 本帧有 4,129 次 draw，比
   采集均值高 6.65%；CPU 帧时间却高 17.84%，而 `vulkan.draw.prepare` 与
   `vulkan.submit` 没有同比例增长。
4. **长尾主要落在 decode scope 的未细分 self 区域。** 最后一段 decode 从相对
   82.166 ms 持续到帧末，裁剪后 35.057 ms；Tracy 报告该 scope 的原始 self time
   为 31.691 ms。现有埋点无法区分这里是在执行命令、等待 flip/guest、等待 OS 调度，
   还是做未命名的资源和状态处理。
5. **Guest 工作存在，但本次不能可靠量化。** 三个 `PPC Core N` 上都采到了
   `espresso.ppc_quantum`，但 Tracy live import 的嵌套时间戳发生回绕/层级损坏。
   Guest/Host 语义边界可信，Guest wall time 数值不可信。
6. **GPU 仍是后续的第二道上限。** 本段 GPU 平均覆盖 46.773 ms，即使完全消除
   CPU 瓶颈，也只能对应约 21 FPS；但它不是 frame `#71` 成为局部最长帧的原因。

## 2. 证据基线

| 项目 | 值 |
| --- | --- |
| 设备 | AYANEO Pocket DS，8 核 ARM64，15,257 MB RAM |
| 应用 / PID | `info.cemu.cemu` / `18747` |
| APK | debuggable，Native `RelWithDebInfo` |
| APK SHA-256 | `cd23e6d75bc2395ebff93d4e37e541dcee65150ee8bf9eaf16504b5c0b23f38f` |
| 游戏 | Breath of the Wild，内部版本 v208，DLC v80 |
| Renderer | Vulkan |
| Tracy | 0.10.0，protocol 64 |
| MCP artifact | `20260801-090701-291-tracy-tracy-live-normalized-only-a75500dd` |
| MCP session | 原始分析 `s3`；本文从 artifact 重载为 `s4` |
| 采集长度 | 20 秒，198 个可分析帧 |
| CPU / GPU zones | 875,932 / 777,827 |
| 真实 GPU-time zones | 776,215；另有 1,612 个 CPU-submit fallback，但 frame #71 为 0 |
| 本地 trace | `_out/profiles/cemu-botw-current.tracy`，59,736,635 bytes |
| Trace SHA-256 | `5891b138dc05eeeac9487330e845246730eb7d5fb705f174238a2c343967d366` |
| 游戏态截图 | [`profiler-current-state.png`](../verification/20260801-C5/profiler-current-state.png) |

状态面板在断开 Tracy 后的同一可操作场景显示 12.2 FPS / 81.88 ms。Tracy 在本次
20 秒内解码了 8,060,435 个事件，绝对 FPS 会受到采集开销影响；本文优先用相对变化
和 CPU/GPU 归属判断瓶颈，不把连接 Tracy 时的 FPS 当作最终性能基线。

## 3. Guest / Host 的定义和关联边界

Cemu 中所有代码最终都运行在 host 设备上。本文所说的 Guest / Host 指“模拟语义”，
不是不同物理进程：

| 域 | Tracy 线程 / scope | 语义 | 本次耗时可信度 |
| --- | --- | --- | --- |
| Guest CPU | Host 线程 `PPC Core 0/1/2` 上的 `espresso.ppc_quantum` | 执行 Wii U PPC guest 指令；先尝试 JIT recompiler，剩余周期走 interpreter | 语义高；数值低 |
| Guest → Host 边界 | GX2/PM4 风格 command buffer、寄存器和同步包 | Guest 生成命令，Host Latte 消费 | 映射高；缺少队列等待计时 |
| Host CPU / GPU emulation | `LatteThread` / `latte.command_buffer.decode` | 解析 guest GPU 命令、更新模拟状态、处理 draw/wait/flip/sync | 高，但 self 尚未细分 |
| Host Vulkan CPU | `vulkan.draw.prepare`、`vulkan.submit` | 准备资源/状态、记录 Vulkan 命令、提交和回收 command buffer | 高 |
| Host physical GPU | GPU context 0 / `vulkan.draw`、`vulkan.present_blit` | Adreno 实际执行 Vulkan 命令 | 高；单 draw 受时间戳粒度限制 |
| Frame boundary | `latte.frame.end` / Tracy frame mark | 将连续 timeline 切成 Cemu 帧 | 高；不是整帧工作 scope |

```mermaid
flowchart TB
    subgraph Guest[Guest 语义：Wii U 软件]
        direction TB
        Game[BOTW guest code]
        GX2[GX2 调用与命令生成]
        PPC[Guest workload on Host PPC Core 0/1/2<br/>espresso.ppc_quantum]
        Game --> PPC --> GX2
    end

    subgraph HostCPU[Host CPU：Cemu 模拟与 Vulkan 前端]
        direction TB
        Queue[Guest command buffer]
        Decode[LatteThread<br/>latte.command_buffer.decode]
        Dispatch{命令分类}
        Translate[寄存器 / resource / draw 翻译]
        Wait[同步分支<br/>wait / semaphore / flip]
        Prepare[vulkan.draw.prepare]
        Submit[vulkan.submit]
        FrameEnd[latte.frame.end]
        Queue --> Decode --> Dispatch
        Dispatch --> Translate --> Prepare --> Submit
        Dispatch --> Wait
        Decode -.帧边界.-> FrameEnd
    end

    subgraph HostGPU[Host GPU：Adreno Vulkan]
        direction TB
        Draw[vulkan.draw]
        Present[vulkan.present_blit]
        Draw --> Present
    end

    GX2 --> Queue
    Prepare -->|记录 vkCmdDraw*| Draw
    Submit -->|vkQueueSubmit| Draw

    classDef guest fill:#4b3f72,color:#fff,stroke:#8f7fd1
    classDef host fill:#164e63,color:#fff,stroke:#38bdf8
    classDef gpu fill:#713f12,color:#fff,stroke:#fbbf24
    class Game,GX2,PPC guest
    class Queue,Decode,Dispatch,Translate,Wait,Prepare,Submit,FrameEnd host
    class Draw,Present gpu
```

关键解释：`LatteThread` 的输入来自 Guest，但其解码、状态缓存和 Vulkan API 开销属于
Host。Guest PPC 三个线程、LatteThread 与 GPU 可以并行，因此这些线程/设备上的 scope
耗时**不能相加**得到 117.223 ms。

## 4. 关键线程、执行实体与职责

### 4.1 先区分三种“线程”

Cemu 的线程模型不能只按 Tracy 左侧显示的线程名理解：

| 执行实体 | 是什么 | 谁调度 | BOTW 中的作用 |
| --- | --- | --- | --- |
| Guest `OSThread_t` | Wii U 进程看到的逻辑 OS 线程；在 Cemu 中保存 guest 寄存器、栈、优先级、亲和性和状态 | Cemu 的 coreinit guest scheduler | 执行 BOTW 游戏逻辑、任务、GX2 提交、回调及 guest 系统服务；本次未导出逐个 Guest 线程的名字和 ID |
| Host `PPC Core 0/1/2` | Android/Linux 上真实的三个 Native OS 线程 | Android/Linux scheduler | 每个线程承载一个 scheduler fiber，并在多个 Guest `OSThread_t` fiber 之间切换；执行出来的语义仍属于 Guest CPU |
| Host Native 辅助线程 | `LatteThread`、`PPCRecompiler`、Vulkan compiler、IOSU、输入等真实线程 | Android/Linux scheduler | 实现 GPU 模拟、JIT 编译、文件系统、输入和后台任务 |
| Host physical GPU queue | Vulkan graphics queue，不是 CPU 线程 | Vulkan driver / Adreno firmware | 执行由 `LatteThread` 翻译和提交的 Vulkan command buffer |

[`OSSchedulerBegin`](../../src/Cafe/OS/libs/coreinit/coreinit_Thread.cpp#L1482) 在 BOTW 的
multicore recompiler 配置下创建三个 `PPC Core N` Host 线程；每个线程通过
[`__OSFiberThreadEntry`](../../src/Cafe/OS/libs/coreinit/coreinit_Thread.cpp#L1382) 运行当前
Guest `OSThread_t` 的 JIT/interpreter quantum，再回到 Guest scheduler 选择下一条可运行
线程。Guest 线程按优先级、状态和 core affinity 被分派，不与某一个 `PPC Core N`
永久一一绑定。

```mermaid
flowchart TB
    AndroidMain[Android UI / lifecycle thread]
    CemuMain[Cemu Main profiler label<br/>Native 初始化 / diagnostics]

    subgraph Critical[主执行链]
        direction TB
        Fibers[Guest OSThread fiber pool<br/>BOTW application / worker<br/>GX2 callback / coreinit services]
        PPC[PPC Core 0/1/2<br/>三个 Host OS 线程]
        Commands[GX2 command buffer / TCL ring]
        Latte[LatteThread<br/>Guest GPU command consumer]
        Driver[Vulkan renderer / driver]
        Fibers -->|按 priority / affinity 动态映射| PPC
        PPC -->|生成命令| Commands --> Latte --> Driver
    end

    subgraph Auxiliary[并行辅助线程]
        direction TB
        Rec[PPCRecompiler<br/>异步 JIT 编译]
        Compile[vkShaderComp / compilePl<br/>shader 与 pipeline 编译]
        Services[IOSU-FSA / IOS-Timer / Input_update<br/>文件、服务与输入]
    end

    GPU[Adreno Vulkan graphics queue]

    AndroidMain --> CemuMain
    CemuMain --> Fibers
    Rec -.提供已编译 PPC block.-> PPC
    Compile -.提供 shader / pipeline.-> Latte
    Services -.唤醒或供给 Guest.-> Fibers
    Driver --> GPU

    classDef guest fill:#4b3f72,color:#fff,stroke:#8f7fd1
    classDef host fill:#164e63,color:#fff,stroke:#38bdf8
    classDef gpu fill:#713f12,color:#fff,stroke:#fbbf24
    class Fibers guest
    class AndroidMain,CemuMain,PPC,Commands,Latte,Driver,Rec,Compile,Services host
    class GPU gpu
```

图中的 `BOTW application/worker OSThread` 是职责类别，不是本次 trace 已解析出的实际
线程名。当前证据不足以断言 BOTW 创建了多少条同类线程、各自名字是什么，或者某一条
Guest 线程在 frame `#71` 占了多少时间。

### 4.2 本次 Tracy 实际可见的线程

Profiler MCP 从当前 artifact 解码出以下六条有 Native zone 或 profiler 事件的线程。
这是一张**抓包可见列表**，不是进程的完整线程枚举：

| TID | Trace 名称 | 层级 | 作用 | 本次可得结论 |
| ---: | --- | --- | --- | --- |
| 18903 | `LatteThread` | Host CPU | 持续消费 TCL ring / indirect command buffer，模拟 Latte command processor，并调用 Vulkan renderer | `latte.command_buffer.decode` 是 frame `#71` 已观测到的 Host 关键路径 |
| 18951 | `PPC Core 0` | Host 线程 / Guest 语义 | 承载 Guest `OSThread` fiber，执行 PPC JIT/interpreter quantum | 有 Guest 活动；scope 时间戳损坏，不能量化 |
| 18952 | `PPC Core 1` | Host 线程 / Guest 语义 | 同上，代表第二个模拟 Espresso core | 同上 |
| 18953 | `PPC Core 2` | Host 线程 / Guest 语义 | 同上，代表第三个模拟 Espresso core | 同上 |
| 18778 | `Tracy Sampling` | Profiler | Tracy 自身采样/采集工作 | 只用于理解 profiler 扰动，不属于 Cemu 帧关键路径 |
| 18779 | `Tracy Profiler` | Profiler | Tracy 自身传输和处理工作 | 同上 |

`Cemu Main`、Android UI 线程、JIT/compiler、IOSU、输入、音频和 Vulkan driver worker 没有
出现在这张六线程列表中，只表示当前 20 秒窗口没有导出可被 MCP 识别的 zone/thread
事件，**不表示这些线程不存在、未运行或没有影响帧时**。如要判断 Host 抢占和调度，
下一次需同时采 Perfetto `sched_switch` 或给关键 worker 加低频顶层 zone。

### 4.3 代码中确认的关键 Host 线程

| 线程 / 线程组 | 稳态职责 | 对帧时的可能影响 | 本次是否可量化 |
| --- | --- | --- | --- |
| `Cemu Main` | diagnostics 初始化时赋给当前 Native 调用线程的 profiler label；该入口启动 profiler/DebugBus，Android 生命周期和 surface 通过 JNI 进入 Native | 该 label 不足以证明存在一条独立、常驻的 Cemu 主循环；相关工作仍可能与 PPC/Latte 争用 CPU | 否；本次未出现在 MCP 线程列表；命名点见 [`CemuDiagnostics.cpp`](../../src/Cafe/Diagnostics/CemuDiagnostics.cpp#L144) |
| `PPC Core 0/1/2` | 执行 Guest `OSThread` 的 PPC JIT/interpreter；Guest scheduler 在 fiber 间切换 | BOTW game logic 变重、Guest 同步或 Host 抢占都会延后 GX2 producer | 仅确认活动，不能可靠计时 |
| `LatteThread` / profiler 名 `Latte GPU Thread` | [`LatteCP_ProcessRingbuffer`](../../src/Cafe/HW/Latte/Core/LatteThread.cpp#L218) 循环读取 Guest GPU 命令，处理 draw/sync/flip，并在 Renderer 上记录/提交命令 | command decode、surface/resource 同步、driver 调用、等待和 command starvation 都可进入关键路径 | 是；当前最重要 |
| `PPCRecompiler` | 异步把 Guest PPC 函数编译为 Host ARM64 block | 新代码路径/JIT miss 可带来编译 CPU 竞争；未编译完时 Guest 可能回退 interpreter | 否；代码入口见 [`PPCRecompiler_thread`](../../src/Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp#L453) |
| `vkShaderComp` | 异步编译翻译后的 Vulkan shader | 新 shader 或 cache miss 可造成 CPU 峰值，必要时仍可能在消费点等待 | 否；入口见 [`CompilerThreadFunc`](../../src/Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.cpp#L157) |
| `compilePl` | 构建 Vulkan graphics pipeline | 新材质/状态组合会增加编译工作和 CPU 竞争；warmup 的目标之一是减少该变量 | 否；入口见 [`compilePipeline_thread`](../../src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.cpp#L1080) |
| `plCacheCompiler` / `plCacheWriter` / `vkDriverPlCache` | 编译、写入 Cemu stable pipeline cache 及保存 driver pipeline cache | 通常是 warmup/后台工作；写盘或编译仍可能形成干扰 | 否 |
| `IOSU-FSA` | 执行模拟 IOSU 文件系统请求，连接 Guest FS API 与 Host/WUA 存储 | BOTW 场景切换、贴图/资源流式读取或 shader cache I/O 可能出现等待 | 否；入口见 [`iosu_fsa.cpp`](../../src/Cafe/IOSU/fsa/iosu_fsa.cpp#L835) |
| `IOS-Timer` 及 IOSU service threads | 模拟 IOS 定时器和系统服务 | 可唤醒 Guest callback；通常不是 draw 主路径，但会与模拟线程并行 | 否 |
| `Input_update` | 轮询 Host 控制器并更新 Cemu input state | Guest 的 VPAD/KPAD 读取消费其状态；通常开销小 | 否；入口见 [`InputManager.cpp`](../../src/input/InputManager.cpp#L949) |
| Audio backend workers | Host audio callback/输出；Guest AX 工作仍由 PPC fiber 执行 | 音频 callback 过载会引起音频问题并争用 CPU，但本次没有归因数据 | 否 |
| Vulkan driver workers | Qualcomm driver 内部编译、提交、内存管理；数量和命名由 driver 决定 | `vkQueueSubmit`、pipeline 和内存操作可能把工作转移到这些线程 | 否；App zone 不能覆盖 driver 内部工作 |

另外，`GX2 event callback`、coreinit alarm/callback/IPC 等名字属于 Guest `OSThread_t`，
不是额外的 Host OS 线程；例如 [`GX2 event callback`](../../src/Cafe/OS/libs/gx2/GX2_Event.cpp#L247)
最终仍由 `PPC Core N` 中某条 Host 线程以 fiber 形式执行。

### 4.4 从“谁阻塞谁”看关键路径

```mermaid
flowchart TB
    GuestRun[Guest OSThread 在 PPC Core N 运行]
    GuestWait[Guest scheduler / fence / timestamp wait]
    Pool[每 core GX2 command buffer pool]
    TCL[TCL ring]
    Latte[LatteThread decode]
    VkRecord[Vulkan record / submit]
    GPUQ[Adreno GPU queue]
    Retire[retire marker / fence / flip]

    GuestRun -->|写 GX2 packets| Pool
    Pool -->|Flush: indirect buffer descriptor| TCL
    TCL --> Latte --> VkRecord --> GPUQ --> Retire
    Retire --> Continue[释放 command pool<br/>完成同步并进入后续 Guest quantum]
    GuestRun -->|pool 满、显式 wait| GuestWait
    GuestWait -->|retired timestamp / event| Continue
    TCL -.空时 Latte 等 producer.-> Latte
    GPUQ -.command-buffer ring 满时 Host 等 fence.-> VkRecord

    classDef guest fill:#4b3f72,color:#fff,stroke:#8f7fd1
    classDef host fill:#164e63,color:#fff,stroke:#38bdf8
    classDef gpu fill:#713f12,color:#fff,stroke:#fbbf24
    class GuestRun,GuestWait,Pool,Continue guest
    class TCL,Latte,VkRecord,Retire host
    class GPUQ gpu
```

同样表现为 `LatteThread` wall time 变长，根因可能是 Host decode busy、Guest producer
没有及时供给、显式 Guest/Latte 同步、Host 被系统抢占，或 Vulkan command-buffer ring
等 GPU fence。当前 `latte.command_buffer.decode` self scope 没把这些状态分开，所以只能
把它定位为“已观测关键路径”，还不能把全部 89.481 ms 都归为纯 decode 算力开销。

## 5. 以 BOTW 为例的 Guest / Host 交互基础逻辑

### 5.1 启动阶段：从 Android 到 Guest 三核

本次对象是 BOTW v208 / DLC v80 的 WUA。Android UI 选择游戏后，JNI 入口先解析 title，
[`PrepareForegroundTitle`](../../src/android/app/src/main/cpp/NativeEmulation.cpp#L351) 建立
base/update/DLC 对应的虚拟 title 文件系统、内存空间、PPC recompiler 和 RPX；随后
[`LaunchForegroundTitle`](../../src/android/app/src/main/cpp/NativeEmulation.cpp#L390) 启动
IOSU title modules、Cemu 游戏态和 Guest scheduler。当前 BOTW 配置使用 multicore
recompiler，因此最终进入三个 `PPC Core N` Host 线程，而不是为每条 BOTW Guest 线程
创建一条 Android thread。

```mermaid
flowchart TB
    UI[Android UI 选择 BOTW WUA]
    JNI[NativeEmulation.prepareTitle]
    Prepare[CafeSystem.PrepareForegroundTitle]
    Mount[挂载虚拟 title FS<br/>base + update overlay + DLC AOC]
    Runtime[初始化 Guest memory / PPC recompiler<br/>加载 RPX]
    Launch[NativeEmulation.launchTitle]
    Start[启动游戏态]
    IOSU[启动 IOSU title modules]
    PPC[OSSchedulerBegin 3<br/>PPC Core 0/1/2]
    Latte[LatteThread 初始化 renderer/cache<br/>等待 GX2Init]
    Guest[BOTW Guest OSThread fibers 开始运行]
    Ready[Guest GX2Init 完成<br/>开始生产 GPU commands]

    UI --> JNI --> Prepare --> Mount --> Runtime --> Launch --> Start
    Start --> IOSU --> PPC --> Guest --> Ready
    Start --> Latte --> Ready
```

这里不是把三个目录简单拷贝到一起：base 挂载到 `/vol/content`，update 以更高优先级
覆盖同一路径，DLC 则挂载到 `/vol/aoc<TitleId>`；对应逻辑见
[`LoadAndMountForegroundTitle`](../../src/Cafe/CafeSystem.cpp#L733)。BOTW 的 Guest 代码仍按
Wii U 的 FS/GX2/coreinit 接口运行，不知道这些 Host 文件实际来自一个 WUA，也不知道
Host 图形后端是 Vulkan。

### 5.2 BOTW 稳态一帧的主链路

BOTW 一帧不是“Guest 完成后 Host 再完成”的串行函数调用。它是 producer/consumer
流水：Guest PPC 生成 GX2 命令，`LatteThread` 异步消费并翻译，物理 GPU 再异步执行；
三者通常处理不同进度的工作。

```mermaid
sequenceDiagram
    participant Guest as BOTW Guest<br/>OSThread on PPC Core N
    participant Boundary as GX2 command pool<br/>and TCL ring
    participant Host as LatteThread<br/>and Vulkan renderer
    participant Device as Adreno GPU<br/>and Android surface

    loop Guest scheduler quantum
        Guest->>Guest: JIT executes simulation / animation / visibility
        Guest->>Boundary: GX2 state, resource, draw, sync and flip
        Boundary->>Boundary: encode PM4-style packets in Guest memory
        opt display list
            Boundary->>Boundary: record list and emit indirect-buffer packet
        end
        alt buffer needs flush
            Boundary->>Host: submit descriptor + retire marker
        else pool has no safe space
            Boundary-->>Guest: wait for retired timestamp
        end
        Guest->>Guest: end quantum; scheduler switches fiber
    end

    par Host command consumption
        Host->>Host: decode state / draw / sync / flip
        Host->>Host: prepare resources, pipeline and vkCmdDraw*
        Host->>Device: batch vkQueueSubmit
    and Physical GPU execution
        Device->>Device: execute earlier submitted Vulkan work
        Device-->>Host: signal semaphore / fence / query
    end

    Host->>Device: present/blit TV or DRC scan buffer
    Host-->>Boundary: retire marker / flip / event
    Boundary-->>Guest: unblock a later Guest wait
```

图中的 `simulation / animation / visibility / render setup` 是典型的游戏职责描述，不是
本次 trace 已有的 BOTW 函数级 symbol 结论。当前抓包只能确认 Guest quantum 存在，不能
把这些职责分别计时。

### 5.3 GX2 命令如何跨过 Guest / Host 边界

以 BOTW 发出一批 draw 为例，边界依次是：

1. BOTW Guest 代码在某个 `OSThread_t` 上运行；承载它的 `PPC Core N` 先进入已经生成的
   ARM64 JIT block，未覆盖的剩余 cycle 才走 interpreter。
2. Guest 调用 GX2 API 时，Cemu HLE 将状态/draw/sync 编成 PM4 风格 dword。每个模拟
   core 有自己的 [`s_perCoreCBState`](../../src/Cafe/OS/libs/gx2/GX2_Command.cpp#L17)，
   packet 被写进 Guest 可见的 command pool 或 display list。
3. [`GX2Command_Flush`](../../src/Cafe/OS/libs/gx2/GX2_Command.cpp#L268) 不把整个 buffer
   复制进 ring，而是通过 [`GX2Command_SubmitCommandBuffer`](../../src/Cafe/OS/libs/gx2/GX2_Command.cpp#L222)
   提交 `IT_INDIRECT_BUFFER_PRIV` 描述符和 retire marker；
   [`TCLSubmitToRing`](../../src/Cafe/OS/libs/TCL/TCL.cpp#L132) 负责等待 ring 空间并写入。
4. `LatteThread` 的 [`LatteCP_ProcessRingbuffer`](../../src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp#L1458)
   读取 top-level ring；遇到 indirect buffer 后进入 `LatteCP_processCommandBuffer`，这正是
   当前 `latte.command_buffer.decode` scope 所在的 Host 区域。
5. draw packet 最终进入 `VulkanRenderer::draw_execute`，Host 进行 shader/pipeline、
   descriptor/resource/state 准备并记录 `vkCmdDraw*`。它对应 CPU zone
   `vulkan.draw.prepare` 和 GPU zone `vulkan.draw`，两者不是同一段执行时间。
6. Vulkan renderer 的新 command buffer 默认将 submit threshold 设为 300 recorded draws，
   也会因 readback、显式“尽快提交”等条件提前 submit。frame `#71` 观测到 4,129 draws
   和 12 次 `vulkan.submit`；由于 CPU frame marker、录制和 GPU 执行跨帧流水，不能用
   `4,129 / 300` 推导该帧应有的 submit 次数，也不能假定每批严格 300 draw。

```mermaid
flowchart TB
    subgraph GuestStage[Guest 生产阶段]
        direction TB
        A[BOTW PPC instructions]
        B[GX2 HLE call]
        C[编码 PM4-style dwords]
        D[Guest memory<br/>per-core CB / display list]
        A --> B --> C --> D
    end

    subgraph HostStage[Host 翻译与提交阶段]
        direction TB
        E[IT_INDIRECT_BUFFER_PRIV]
        F[TCL ring]
        G[Latte command decode]
        H[Vulkan draw.prepare]
        I[Vulkan command buffer]
        J[vkQueueSubmit]
        E --> F --> G --> H --> I --> J
    end

    K[Adreno GPU execution]

    D --> E
    J --> K

    classDef guest fill:#4b3f72,color:#fff,stroke:#8f7fd1
    classDef host fill:#164e63,color:#fff,stroke:#38bdf8
    classDef gpu fill:#713f12,color:#fff,stroke:#fbbf24
    class A,B,C,D guest
    class E,F,G,H,I,J host
    class K gpu
```

`IT_INDIRECT_BUFFER_PRIV` 的 dword 位于 Guest memory，但读取、递归展开和处理发生在 Host
`LatteThread`；所以“命令来源属于 Guest”和“decode 耗时属于 Host”必须同时成立。

### 5.4 反馈、等待与背压

Guest/Host/GPU 不是无限前冲，主要反馈环包括：

| 等待点 | 等待者 | 等待对象 | 帧分析意义 |
| --- | --- | --- | --- |
| GX2 command pool 无安全空间 | Guest `OSThread`，实际阻塞在承载它的 `PPC Core N` 调用路径 | 已提交 buffer 的 retired timestamp 更新 read pointer | Guest producer 被 GPU/Latte 消费速度反压；代码见 [`GX2Command_StartNewCommandBuffer`](../../src/Cafe/OS/libs/gx2/GX2_Command.cpp#L145) |
| TCL ring 空间不足 | 发起 `TCLSubmitToRing` 的 Guest 调用路径 | `LatteThread` 消费 ring | 可能让 Guest graphics submission 停顿；当前未独立计时 |
| TCL ring 没有命令 | `LatteThread` | Guest 继续生产/flush | 表现为 Host consumer starvation；当前没有 queue-empty zone/counter |
| GX2 wait / semaphore / flip | Guest 或 Latte command decode | Guest memory 条件、retire marker、显示时序或异步操作 | 可能落在 decode self 或 Guest scheduler wait；当前未拆分 |
| Vulkan command-buffer ring 用尽 | `LatteThread` / Vulkan renderer | GPU fence / finished command buffer | 包含于 `vulkan.submit` 父 scope，尚未单列 |
| Present / surface 生命周期 | `LatteThread` / renderer | swapchain image、Android surface 和显示时序 | 本次 present CPU scope 很小，但不能外推所有场景 |

因此“Guest 慢”既可能表示 BOTW PPC 工作本身多，也可能表示 Guest 在等 Host/GPU；
“LatteThread scope 长”既可能表示 Host busy，也可能包含等待。只有新增 queue depth、wait
reason、retire latency 和 OS scheduling 数据后，才能闭环判断方向。

### 5.5 BOTW 的非图形侧交互

图形是 frame `#71` 已观测的关键链路，但 BOTW 还有几条并行侧链：

| 侧链 | Guest 侧 | Host 侧 | 当前 trace 覆盖 |
| --- | --- | --- | --- |
| 资源/文件 | BOTW FS/IOS 请求、Guest 线程等待结果 | IOSU-FSA、WUA/Host filesystem、解压和 page cache | 无可归因 zone；场景切换时应单独观察 I/O |
| Shader/pipeline | Guest GX2 shader/state 首次出现 | shader 翻译、`vkShaderComp`、`compilePl`、stable/driver cache | worker 未出现在本次 MCP 列表；warmup 后仍需记录 cache hit/miss |
| 输入 | Guest VPAD/KPAD 读取 | Android/controller provider 与 `Input_update` | 未计时；一般不是本帧主导项 |
| 音频 | Guest AX 逻辑由 PPC fiber 执行 | Host mixer/audio backend callback | 未计时；可造成 CPU 竞争，但没有本次证据 |
| Guest OS 服务 | coreinit callback/alarm/IPC `OSThread` | PPC fiber scheduler + IOSU services | 只看到聚合 quantum，没有逐 Guest 线程归属 |

### 5.6 当前 BOTW 抓包能回答和不能回答的问题

| 问题 | 当前答案 | 证据状态 |
| --- | --- | --- |
| frame `#71` 是不是 GPU 执行时间突增？ | 不是；GPU 46.137 ms，低于全段均值 | 可验证 |
| 已观测 CPU 关键路径在哪？ | Host `LatteThread`，帧内 decode union 89.481 ms | 可验证 |
| BOTW Guest 是否同时在运行？ | 是；三个 PPC Core 有 365 个匹配 quantum | 可验证，但数值损坏 |
| 哪条 BOTW Guest `OSThread` 最慢？ | 不知道；trace 没有 Guest thread id/name 维度 | 未采集，不能推断 |
| Guest 是计算慢还是等 command pool/fence？ | 不知道 | 缺 Guest wait reason 与 queue/retire counter |
| Latte decode self 是 busy 还是 off-CPU/wait？ | 不知道 | 缺 opcode 子 scope 与 `sched_switch` |
| shader/pipeline 编译是否参与 frame `#71`？ | 不知道；相关 worker 未被当前 artifact 解码 | 不可用“未出现”证明没有参与 |
| 4,129 个 draw 是否来自某个具体 BOTW pass？ | 不知道 | 缺 render-pass/Guest marker 关联 |

这也是下一轮埋点的边界：先补可验证关联，再讨论 BOTW 某个系统、pass 或 Guest 线程的
优化，不能用一般游戏架构常识替代当前版本/当前场景的采集结果。

## 6. Frame #71 与全段、相邻帧的比较

| 指标 | Frame #71 | 198 帧平均 | 相对平均 |
| --- | ---: | ---: | ---: |
| CPU frame | 117.223 ms | 99.477 ms | **+17.84%** |
| Vulkan GPU covered | 46.137 ms | 46.773 ms | **-1.36%** |
| CPU / GPU | 2.541x | 2.137x | CPU 偏离更明显 |
| draw count | 4,129 | 3,871.7 | **+6.65%** |
| GPU zones | 4,130 | 约 3,872.7 | +6.64% |
| 最大单个 GPU zone | 6.291 ms | 非同帧均值 | 不是全段最大 10.486 ms |

相邻帧进一步说明 GPU 没有在 frame `#71` 同步恶化：

| Frame | CPU | GPU | Draws | CPU/GPU |
| ---: | ---: | ---: | ---: | ---: |
| 69 | 101.286 ms | 50.332 ms | 3,682 | 2.012x |
| 70 | 99.587 ms | 50.332 ms | 3,681 | 1.979x |
| **71** | **117.223 ms** | **46.137 ms** | **4,129** | **2.541x** |
| 72 | 99.680 ms | 48.234 ms | 4,042 | 2.067x |
| 73 | 98.295 ms | 46.137 ms | 3,863 | 2.131x |

```mermaid
flowchart TD
    F[Frame #71 = 117.223 ms]
    CPU[CPU 比均值慢 17.84%]
    GPU[GPU 比均值快 1.36%]
    Draws[Draw 数只增加 6.65%]
    Host[Host LatteThread<br/>帧内 decode 覆盖 89.481 ms]
    Guest[Guest PPC<br/>已观测但时间戳不可量化]
    Verdict[判定：CPU/同步侧长尾<br/>不是 GPU 执行尖峰]

    F --> CPU --> Host --> Verdict
    CPU --> Guest --> Verdict
    F --> GPU --> Verdict
    F --> Draws --> Host

    classDef evidence fill:#0f172a,color:#fff,stroke:#64748b
    classDef verdict fill:#7c2d12,color:#fff,stroke:#fb923c
    class F,CPU,GPU,Draws,Host,Guest evidence
    class Verdict verdict
```

## 7. Frame #71 的端到端时序

帧的 trace-relative 边界为：

```text
start = 184,442,335,868 ns
end   = 184,559,558,576 ns
span  =     117,222,708 ns = 117.223 ms
```

以下时间均相对该帧起点：

```mermaid
sequenceDiagram
    participant Guest as Guest workload<br/>on Host PPC Core 0/1/2
    participant Latte as Host LatteThread
    participant VkCPU as Host Vulkan CPU
    participant GPU as Adreno Vulkan GPU

    Note over Guest,GPU: t=0.000 ms：Frame #71 开始
    par Guest 执行与产生命令
        Guest-->>Latte: espresso.ppc_quantum / guest command buffers<br/>持续时间当前不可可靠量化
    and Host 消费上一批和当前命令
        Note over Latte: t=0.000–14.075：仅有 0.016 ms decode 尾部；其余未归属
        Latte->>VkCPU: t=14.075–42.226：多段 command decode / draw prepare / submit
        VkCPU-->>GPU: 首批有分辨率的 GPU 工作在 t=21.449 开始
        Note over Latte,VkCPU: t=47.300–72.244：24.944 ms decode self 段
        Note over GPU: frame-assigned GPU 活动到 t=71.781 结束<br/>覆盖 46.137 ms
        Note over Latte: t=72.244–80.633：8.389 ms 未归属空洞
        Latte->>VkCPU: t=80.633–117.223：连续 decode 到帧末
    end
    Note over Guest,GPU: t=117.223 ms：latte.frame.end / Frame #71 结束
```

这张图表达的是同一 frame window 内的并行关系，不证明 Guest 命令在同一帧内生产并
消费，也不证明 GPU zone 与某个具体 Guest draw 一一因果对应。

## 8. Host CPU：LatteThread 细分

### 8.1 先修正 `analyze_frame` 的边界统计

Profiler MCP 的 `analyze_frame` 报告：

```text
latte.command_buffer.decode total = 115.478 ms
count = 61
```

这个 115.478 ms 是“与帧相交的 scope 的原始完整时长之和”，不是严格裁剪后的帧内
占用。其中一个 25.975 ms scope 基本位于前一帧，只在 frame `#71` 开头相交约
0.016 ms。按每个 slice 的 start/end 裁剪到 frame `#71` 后：

| 指标 | 精确帧内值 |
| --- | ---: |
| decode slice 数 | 61 |
| 裁剪后总和 / 并集 | **89.481 ms** |
| 相互重叠 | 0 ms |
| frame 覆盖率 | **76.33%** |
| 没有 decode scope 的帧内时间 | **27.742 ms** |

后续自动分析应优先使用裁剪并集，不要把 115.478 / 117.223 直接解释为 98.5% 利用率。

### 8.2 主要 decode 区间

| 相对区间 | 裁剪时长 | Tracy 原始 self | 说明 |
| --- | ---: | ---: | --- |
| 82.166–117.223 ms | **35.057 ms** | 31.691 ms | 最长段；一直延伸到 frame end |
| 47.300–72.244 ms | **24.944 ms** | 24.944 ms | 完全是未细分 self，没有命名 child |
| 18.160–38.910 ms | **20.750 ms** | 4.478 ms | 大部分可由 draw prepare / submit child 解释 |
| 14.410–18.157 ms | 3.747 ms | 1.103 ms | 短 decode burst |
| 38.917–40.489 ms | 1.572 ms | 0.193 ms | 短 decode burst |
| 80.633–82.160 ms | 1.527 ms | 0.269 ms | 紧邻最终长段 |

三个主要无 decode 区间是：

| 相对区间 | 时长 | 当前状态 |
| --- | ---: | --- |
| 0.016–14.075 ms | 14.059 ms | 未归属，可能是 Guest/Host 同步、线程调度或未埋点 Latte 工作 |
| 42.226–47.300 ms | 5.074 ms | 未归属 |
| 72.244–80.633 ms | 8.389 ms | 未归属；不能仅凭数值等于 GPU 时间戳量化步长断言原因 |

其余微小间隔合计约 0.220 ms。

### 8.3 已命名的 Host 子 scope

| Scope | Count | Total | 单次统计 | 归属 |
| --- | ---: | ---: | --- | --- |
| `vulkan.draw.prepare` | 4,129 | 16.848 ms | p50 0.002、p90 0.008、p99 0.030、max 0.244 ms | Host CPU，嵌套于 decode |
| `vulkan.submit` | 12 | 9.454 ms | p50 0.647、p90 1.322、max 2.079 ms | Host CPU / driver，嵌套于 decode |
| `Collect` | 12 | 1.096 ms | max 0.430 ms | profiler GPU query 收集，嵌套于 submit；不要重复相加 |
| `vulkan.present_blit.prepare` | 1 | 0.042 ms | 单次 | Host CPU |
| `latte.frame.end` | 1 | 0.002 ms | 单次 | frame marker，不代表整帧工作 |

全段平均每帧的 `vulkan.draw.prepare` 约 16.54 ms、`vulkan.submit` 约 10.33 ms；
frame `#71` 分别为 16.848 ms 和 9.454 ms。它们没有出现足以解释额外 17.746 ms
帧时的尖峰。因此当前优先目标不是 `vkQueueSubmit` 本身，而是 decode self 与未归属
空洞。

### 8.4 Host scope 对应源码

| Scope / 边界 | 代码入口 | 当前包含的工作 |
| --- | --- | --- |
| `latte.command_buffer.decode` | [`LatteCP_processCommandBuffer`](../../src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp#L1177) | 命令读取/dispatch、寄存器与 resource 更新、surface sync、indirect buffer、draw、wait、semaphore、flip、async sync |
| draw packets | [`IT_DRAW_INDEX_*`](../../src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp#L1263) | 进入 draw pass、normal/fast draw 路径，最终调用 renderer |
| wait / sync | [`IT_WAIT_REG_MEM`](../../src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp#L1297)、[`IT_MEM_SEMAPHORE`](../../src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp#L1314)、[`IT_HLE_WAIT_FOR_FLIP`](../../src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp#L1365) | 可能在 decode self 内等待；当前没有独立 zone |
| `vulkan.draw.prepare` | [`VulkanRenderer::draw_execute`](../../src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRendererCore.cpp#L1747) | pipeline/resource/state 准备，记录 `vkCmdDraw*`；同时建立 GPU `vulkan.draw` zone |
| `vulkan.submit` | [`VulkanRenderer::SubmitCommandBuffer`](../../src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.cpp#L2251) | end render pass、query collect、`vkEndCommandBuffer`、`vkQueueSubmit`、finished buffer 处理和下一个 command buffer 获取 |
| command-buffer fence wait | [`WaitForNextFinishedCommandBuffer`](../../src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.cpp#L2234) | command buffer ring 用尽时等待 fence；当前只包含在 submit 父 scope 中 |
| present | [`DrawBackbufferQuad`](../../src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.cpp#L3390) | swapchain acquire、barrier、backbuffer blit |
| frame mark | [`LattePerformanceMonitor_frameEnd`](../../src/Cafe/HW/Latte/Core/LattePerformanceMonitor.cpp#L11) | 更新统计并发出 Tracy frame end |

`latte.command_buffer.decode` 的 self time 是 wall-clock scope time，不等价于 on-CPU
执行时间。Tracy session 没有可用的 `sched_switch` 分析，所以当前无法区分 busy、sleep、
yield、preempted 或等待 Guest/GPU。

## 9. Guest CPU：可以关联什么，暂时不能断言什么

Guest scope 位于 [`__OSFiberThreadEntry`](../../src/Cafe/OS/libs/coreinit/coreinit_Thread.cpp#L1382)。
`espresso.ppc_quantum` 包围：

1. `PPCRecompiler_attemptEnterWithoutRecompile`：优先进入已生成的 JIT block；
2. 未消耗完的 guest cycle：用 `PPCInterpreterSlim_executeInstruction` 继续解释执行；
3. scope 结束后才进入 reservation reset 和 OS scheduler reschedule。

```mermaid
flowchart TD
    Q[Guest timeslice / remainingCycles > 0]
    Z[espresso.ppc_quantum]
    JIT[PPC recompiler<br/>attemptEnterWithoutRecompile]
    Left{仍有 remainingCycles?}
    INT[PPCInterpreterSlim<br/>executeInstruction]
    SCHED[Guest OS scheduler / fiber switch<br/>不在 quantum scope 内]
    CMD[Guest GX2 代码写入 command buffer]

    Q --> Z --> JIT --> Left
    Left -->|是| INT --> Left
    Left -->|否| SCHED
    JIT -. guest graphics calls .-> CMD
    INT -. guest graphics calls .-> CMD
    CMD -->|生产| HOST[Host Latte command queue]

    classDef guest fill:#4b3f72,color:#fff,stroke:#8f7fd1
    class Q,Z,JIT,Left,INT,SCHED,CMD guest
```

本帧可确认：

- `PPC Core 0/1/2` 都有 `espresso.ppc_quantum`；
- MCP 在 frame query 中匹配到 365 个 quantum scope；
- Guest workload 与 Host Latte/GPU 并行存在。

本帧**不能**使用以下数值做结论：

- `espresso.ppc_quantum totalMs=664690414276.934`；
- `maxMs=94955771214.205`；
- 三核 `selfMs=545.355` 的直接求和。

原因是 live Tracy import 出现异常巨大、递归嵌套的 quantum scope，时间戳明显超过
本次进程/采集寿命；而三个 PPC 线程本来也并行，求和不会得到 frame wall time。
MCP diagnostics 同时说明 callstack payload/sample 尚未导出，硬件采样 IP 也未完成
symbolization 和 CPU-zone attribution。

因此当前 Guest 结论应写为：**Guest 可能参与了 Host command starvation、同步等待或
本身的 CPU critical path，但 frame #71 的现有数据不能对 Guest 与 Host 的 wall time
占比排序。**

## 10. Host GPU：执行量和与 Host CPU 的重叠

Frame `#71` 的 GPU 结果为 high confidence，4,130 个 GPU zones 全部使用真实
`gpu-time`，没有 CPU-submit fallback：

| 指标 | 值 |
| --- | ---: |
| GPU covered time | **46.137 ms** |
| 占 frame window | 39.36% |
| `vulkan.draw` | 4,129 zones，46.134 ms |
| `vulkan.present_blit` | 1 zone，解析为 0 ms |
| 帧内有非零时长的 zone | 19 |
| 解析为 0 的 zone | 4,111 |
| 有效 GPU 活动跨度 | t=21.449–71.781 ms |
| 最大 draw zone | 6.291 ms，t=42.421–48.712 ms |
| 次大 draw zone | 4.194 ms，t=31.935–36.130 ms |
| 其他非零 draw zone | 主要为 2.097 ms |

GPU timestamp 呈明显的约 2.097 ms 量化步长。4,111 个单 draw zone 为 0 不表示这些
draw 免费，只表示当前 GPU 查询/时间戳分辨率不足以逐 draw 分辨。适合本次结论的是
整帧 covered time、活动跨度和大 zone，不是单 draw 平均。

从 CPU 上记录 GPU marker 到 GPU 实际开始的差值，在 19 个非零 zone 上平均为
17.599 ms、p50 为 20.586 ms、最大 28.895 ms。这个差值包含 Vulkan command buffer
继续录制、等待批量 submit 和 GPU queue 排队，不能直接解释成单独的
`vkQueueSubmit` 调用耗时。

Frame `#71` 的 frame-assigned GPU 活动约在 t=71.781 ms 结束，而 Host 最后一段 decode
从 t=82.166 ms 持续到 t=117.223 ms。即使考虑跨帧流水和关联误差，GPU 也没有呈现
“一直忙到 frame end”的形态，进一步支持 CPU/同步尾部而非 GPU 尖峰的判断。

## 11. 新增埋点与下一轮验证顺序

```mermaid
flowchart TD
    Start[复现同一 BOTW 场景]
    Frame[找到 >110 ms frame]
    Split{CPU frame 是否仍明显大于 GPU?}
    Host[细分 Host decode self]
    Guest[修复 Guest quantum 生命周期<br/>或改用可符号化采样]
    GPU[改为 pass / submit 粒度 GPU zones]
    Wait{长尾落在哪里?}
    Busy[opcode/state/resource CPU busy]
    Sync[flip / semaphore / queue / fence wait]
    Starve[Guest producer / Host consumer starvation]
    Optimize[按证据实施优化]
    Compare[断开 Tracy 后做同场景 FPS 对照]

    Start --> Frame --> Split
    Split -->|是| Host
    Split -->|Guest 不可量化| Guest
    Split -->|GPU 接近或超过 CPU| GPU
    Host --> Wait
    Guest --> Wait
    Wait --> Busy --> Optimize
    Wait --> Sync --> Optimize
    Wait --> Starve --> Optimize
    GPU --> Optimize --> Compare
```

### 11.1 本轮已补的共享 C++ 标签

这些标签同时用于 Android 与桌面端，Foundation profiler 是硬依赖，不再存在条件
关闭分支。标签优先覆盖 frame `#71` 中仍未归类的长段，且
没有继续增加逐 draw 事件：

| 域 | 新增 scope / counter | 要回答的问题 |
| --- | --- | --- |
| Guest 执行 | `cemu.guest_quanta_per_frame`、`cemu.guest_jit_entries_per_frame`、`cemu.guest_interpreter_instructions_per_frame` | 每帧 JIT 命中与 interpreter fallback 的语义工作量 |
| Guest 编译 | `espresso.recompiler.compile` 及 discover/IML/optimize/codegen/activate 子阶段 | 长帧是否与首次编译或某个编译阶段重叠 |
| Guest 失效 | `espresso.recompiler.invalidate` | RPL/codegen/调试失效是否造成重复编译 |
| Guest 背压 | `cemu.gx2_command_pool_wait_count`、`cemu.gx2_command_pool_last_wait_us` | Guest 是否在等 command buffer 退休 |
| Guest → Host ring | `cemu.tcl_ring_*`、`cemu.latte_command_ring_*` wait counters | producer 被塞满，还是 consumer 因无命令而饥饿 |
| Latte decode | `latte.draw_pass.decode`、surface/copy/clear、scanbuffer scopes | decode self time 是否落在 draw pass、资源或 scanbuffer 操作 |
| Latte 同步 | `latte.sync.wait_reg_mem`、`.mem_semaphore_wait`、`.wait_for_flip`、`.async_*` | 尾部是 CPU busy 还是 Guest/GPU 同步等待 |
| Vulkan 生命周期 | `vulkan.command_buffer.process_finished`、`.wait_for_fence` | command buffer 回收是否阻塞 Host |
| Vulkan submit | `vulkan.submit.*` 子阶段 | collect/end/queue-submit/recycle 哪一段变长 |
| 后台编译 | `vulkan.shader.compile`、`vulkan.pipeline.compile`、`vulkan.pipeline_cache.compile/write` | shader/pipeline worker 是否与长帧竞争 CPU |
| IOSU | `iosu.fsa.request` | 流式读取是否形成并发 I/O 尖峰 |

Guest fiber 可以迁移到另一条 Host PPC core 线程，长期等待也可能跨越 Tracy 采集
边界。因此 Guest quantum、GX2 pool wait、TCL ring wait 和 Latte command-ring idle
都使用有限语义计数或在等待完成后提交 counter，不使用可能在另一 Host 线程结束、或
在采集结束时仍未闭合的普通 RAII zone。下一轮应先验证这些名称确实出现在
RelWithDebInfo trace 中，再依据结果决定第二轮埋点。

### 11.2 Host 后续补点

不要继续给每个 draw 增加更多 Tracy zones；本次已有约 3,870–4,129 draw/帧，逐 draw
事件本身会放大采集开销。优先使用少量 scope + per-frame aggregate counter：

1. 将 `latte.command_buffer.decode` self 拆为：command fetch、opcode dispatch、
   state/resource update、normal draw、fast draw、sync/wait、flip/vsync、async command。
2. 为 `IT_WAIT_REG_MEM`、`IT_MEM_SEMAPHORE`、`IT_HLE_WAIT_FOR_FLIP` 和
   `IT_HLE_SYNC_ASYNC_OPERATIONS` 分别累计次数和总 wall time。
3. 每帧记录 command words、opcode 分类数、indirect-buffer 数/最大深度、normal/fast
   draw 数，判断 4,129 draws 是工作量上涨还是同样工作变贵。
4. 将 `vulkan.submit` 拆为 query collect、`vkEndCommandBuffer`、`vkQueueSubmit`、
   `ProcessFinishedCommandBuffers`、`WaitForNextFinishedCommandBuffer`。
5. 增加 Latte command queue depth / empty-wait 时间，直接区分 Host busy 和 Guest
   producer starvation。

### 11.3 Guest 后续补点

1. 用每帧 `guest_quanta`、`guest_jit_entries` 和 `guest_interpreter_instructions` 判断 JIT
   命中及 fallback；这些是语义工作量，不把它们误写为 Guest wall time。
2. 后续再补 JIT miss/leave、成功编译 block 和 guest scheduler 计数；若要恢复 Guest
   wall-time zone，必须先由 Foundation 提供并验证 Tracy fiber-aware 生命周期。
3. 将已有 hardware sample IP 用 APK 内未剥离符号完成 symbolization，并按 PPC core /
   LatteThread 归属。
4. 如需判断阻塞/抢占，增加 Perfetto `sched_switch` 辅助采集；Tracy zone wall time
   单独无法区分 on-CPU 和 off-CPU。

### 11.4 GPU 后续补点

1. 保留 `vulkan.draw`，但正式性能基线建议降采样或增加 render-pass / command-buffer
   粒度 zone，避免每帧四千多个 query。
2. 记录每个 submit 的 draw 数、GPU covered time、CPU marker→GPU start latency。
3. 继续以整帧 GPU covered time 判断 GPU 上限；不要按 0 ms 的单 draw zone 排热点。

### 11.5 最新埋点验证结果

最新 Android RelWithDebInfo 构建在 AYANEO Pocket DS 上完成完整默认 warmup，并通过
截图确认 BOTW v208 已进入初始神庙的可操作 gameplay。正式 Tracy 窗口为 20 秒、
198 个 frame-set 帧，session 为 `s6`。启动和菜单阶段不参与以下结论。

```mermaid
flowchart TD
    Guest[Guest 每帧约 373 quanta]
    Jit[JIT entry 每帧约 373<br/>interpreter 约 7.8k 指令]
    Ring[Latte command ring 空闲<br/>约 17.4 ms/帧]
    Decode[Latte inclusive 工作<br/>约 82.5 ms/帧]
    Sync[同步与回读<br/>约 45.5 ms/帧]
    Draw[draw prepare<br/>约 17.2 ms/帧]
    Frame[约 98.8 ms/帧<br/>10.1 FPS]

    Guest --> Jit --> Ring
    Ring --> Decode
    Decode --> Sync
    Decode --> Draw
    Sync --> Frame
    Draw --> Frame
```

Guest counter 均为有限值：每帧平均 373.4 quanta、372.9 次 JIT entry 和 7,795.6
条 interpreter 指令。JIT entry / quantum 约为 99.85%；20 秒内只有 7 次 recompiler
job、合计 40.040 ms。因此当前稳态低帧率不是 interpreter fallback 或持续编译主导，
AOT 也不是解决这组数据的第一优先级。

Host 的 181 帧有界区间中，`latte.command_buffer.decode` inclusive 平均约
82.51 ms/帧，主要具名子项为：

| Host 子项 | 约每帧 | 说明 |
| --- | ---: | --- |
| `latte.sync.async_readback` | 24.80 ms | GPU readback / fence 路径 |
| `latte.sync.wait_reg_mem` | 20.65 ms | Guest/Host 寄存器内存同步 |
| `vulkan.draw.prepare` | 17.17 ms | 约 3.87k draws 的 CPU 准备 |
| `vulkan.submit` | 11.59 ms | inclusive；最大子项为 recycle |
| `latte.scanbuffer.swap` | 4.02 ms | 呈现路径 |

Latte command ring 另记录到 579 次已完成空闲等待，合计 3,480,999 微秒，折算约
17.4 ms/帧。它与 82.5 ms Latte 工作基本闭合约 100 ms 帧时间，但只能说明 Host
consumer 等待 Guest producer，不能直接证明 Guest PPC CPU 饱和。

最长帧 `#22` 为 118.824 ms，其中 `wait_reg_mem` 26.426 ms、async readback
25.513 ms、fence wait 25.505 ms、draw prepare 18.864 ms；次长帧 `#147` 具有相同
形态。这说明最长帧来自重复出现的同步等待和高 per-draw Host 成本，而不是偶发编译
尖峰。

本次 GPU timestamp 虽匹配到 773,894 个 zone，但抽查 zone 的起止 timestamp 完全
相同，GPU 耗时均为 0 ms；因此本构建只能确认 Host 在 fence/readback 上等待，不能
据此判断 GPU 是否实际饱和。后续必须先修复 timestamp/query 生命周期，并将 GPU zone
从逐 draw 降为 pass/submit 粒度，再给出新的 GPU 排名。

完整采集标识、APK hash、截图、counter 和最长帧表见
[`performance-profiler.md`](../verification/20260801-C5/performance-profiler.md)。

## 12. 后续退出标准

下一次声称“定位/修复最长帧”前，至少满足：

- 使用同一设备、BOTW 场景和 Android RelWithDebInfo；
- warmup 完成且截图确认处于可操作 gameplay；
- 至少 20 秒采集，保留 frame count、trace hash 和 artifact id；
- 分别报告 Guest、Host CPU、Host GPU，不能把并行线程耗时相加；
- frame 范围使用裁剪后的 scope union，不能用跨帧 scope 的原始全长；
- 明确区分 CPU busy、OS off-CPU、Guest/Host queue wait 和 GPU fence wait；
- 同时提供 Tracy 连接态和断开态 FPS，量化 profiler perturbation；
- 对比 p50/p90/p95/p99、最慢帧和相邻帧，不能只看单一截图；
- 若 Guest 时间戳仍异常，结论必须保持“Guest 未量化”，不得用推断补数。

## 13. Profiler MCP 查询记录

本文基于以下查询类型：

```text
load_trace_artifact(artifact_id=..., keep_session=true)
analyze_frame(session_id=s4, frame_index=71)
analyze_frame_detail(session_id=s4, frame_index=71)
find_top_slices(session_id=s4, frame #71 time range, name_filter=...)
analyze_gpu_frames(session_id=s4, time_source=gpu-time)
get_gpu_timeline(session_id=s4, frame 71, time_source=gpu-time)
query_counter(session_id=s4, counter_name=...)
get_import_diagnostics(session_id=s3/s4)
```

概要报告与全段统计见
[`performance-profiler.md`](../verification/20260801-C5/performance-profiler.md)。
AOT 的成本、适用边界与推荐的 eager JIT 路径见
[`cemu-aot-assessment.md`](cemu-aot-assessment.md)。
