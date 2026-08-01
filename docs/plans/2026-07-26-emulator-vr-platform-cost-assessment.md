# 模拟器 VR 改造跨平台成本评估（Wii U / 3DS / NS）

- 日期：2026-07-26
- 触发问题：BetterVR 那套「PPC 汇编补丁 + HLE 回调」机制的制作流程是什么？能否给特定游戏定制？在 3DS 和 NS 上实现类似机制成本如何？
- 关联文档：
  - `docs/plans/2026-07-26-botw-vr-android-plan.md`（BotW VR on Android 方案）
  - `references/botw` — BotW 反编译项目的固定 reference 子模块，符号来源，详见 §6
  - `docs/bettervr/README.md`（BetterVR 实现、分析快照与验证入口）
  - `~/workspace/emulations/wii/dolphinxr/docs/DolphinXR-OpenXR-Stereo-and-Frame-Pacing.md`（DolphinXR 立体与帧率分析）
- 本文性质：**评估与判断，不是实施计划。** 成本判断是基于代码核查的工程判断，不是实测数据。

---

## 1. 结论摘要

| 维度 | Wii U / Cemu | 3DS / azahar | NS / eden |
|---|---|---|---|
| **原生立体** | 无 | **有（硬件级）** | 无 |
| 补丁基础设施 | **完整**：汇编器 + codecave + HLE 回调 | 仅金手指（内存写值） | IPS32 有，guest→native 回调**无** |
| 着色器覆盖生态 | **成熟**（社区多年在用） | 有相关设施 | 弱（Maxwell/NVN） |
| BotW 符号知识 | 社区 RE，139 地址手工映射（**符号名来自 zeldaret/botw**，见 §6.3） | N/A | 反编译项目原生目标（@ Switch 1.5.0），但**渲染层覆盖率未知**，见 §6.5 |
| 已有 XR 集成 | 无（本项目 C6/C7 在做） | **已有**（`jni/vr/`） | 无 |
| 现成 VR 参考实现 | **BetterVR 完整可用** | 无 | 无 |
| 模拟性能压力 | 高 | **低** | 高 |
| 项目/法律风险 | 低 | 低 | **高**（yuzu/Ryujinx 均已关停） |

**两条推荐，取决于目标：**

- **若目标是「先验证 VR 方向是否成立」** → **3DS 成本最低**。原生立体 + 已有 XR 集成 + 低性能压力，三项叠加。代价是内容不是 BotW。
- **若目标锁死 BotW** → **Wii U / Cemu 仍是最优**。BetterVR 这份参考实现的价值超过 Switch 侧的符号优势——它不只告诉你符号在哪，还告诉你整套设计该怎么搭。

---

## 2. Wii U / Cemu：基线（BetterVR 已完成）

### 2.1 Cemu 提供的基础设施（通用，与游戏无关）

| 能力 | 位置 | 说明 |
|---|---|---|
| PPC 汇编器 | `src/Cemu/PPCAssembler/ppcAssembler.cpp` | 加载 graphic pack 时**现场汇编** `.asm`，作者写汇编源码而非字节补丁 |
| 补丁指令集 | `src/Cafe/GraphicPack/GraphicPack2PatchesParser.cpp` | 仅四个：`moduleMatches`、`codecave`、`entry`、`rpx` |
| codecave | 同上 | 在 RPX 之外分配可执行内存，hook 代码写这里，原地址只放跳转 |
| HLE 回调 | `osLib_registerHLEFunction`（`src/Cafe/OS/common/OSCommon.cpp:105`） | 补丁里 `bl import.coreinit.hook_X` → native C++，拿到完整 `PPCInterpreter_t` |
| 着色器覆盖 | graphic pack `<hash>_vs.txt` / `_ps.txt` | 按着色器 hash 替换反编译后的 Latte 着色器 |
| 调试器 | `src/Cafe/HW/Espresso/Debugger/` | 断点、GDB stub、`DebugSymbolStorage` |
| **`.map` 符号加载** | 上游 `7ff99a5e`（#1916），已随 C1 同步进本仓 | 调试器可加载符号表——见 §5.5 的闭环用法 |

`moduleMatches = 0x6267BFD0` 是 **RPX 校验和**，补丁只对该二进制生效。这是版本锁的来源。

### 2.2 BetterVR 的逆向工作量（从产物反推）

> 下述流程是从仓库产物反推的，BetterVR 没有文档化其工作流程。

**符号来源是混合的。** 139 处 `0xADDR = name:` 映射呈现两种风格：

```
0x030EC860 = ksys__act__ai__InlineParamPack__addBool:    ← BotW 反编译社区的真实符号
0x03821B64 = ksys_phys_ModelBoneAccessor_getBoneName:    ← 同上
0x10263910 = sead__SafeString__vt:                       ← Nintendo sead 引擎符号
0x031FB1B4 = sub_31FB1B4_getTimeForGameUpdateMaybe:      ← IDA 自动命名 + 人工猜测后缀
```

`ksys::` / `sead::` / `agl::` 是 `zeldaret/botw` 与 BotW 逆向社区的通用命名，说明**作者复用了社区符号知识**，只对未覆盖部分自行命名（`Maybe` 后缀诚实地暴露了不确定性）。

**仓库内没有 `.idb` / `.map` / 符号文件**——逆向数据库是私有的，只有解析完的地址被提交。**接手者拿不到中间产物，只能拿到结论。**

**他们造了一个 PPC 级 profiler 来测绘帧结构。** `patch_debug_PPC_Profiling.asm` 给 30+ 引擎阶段插桩：

```
PPC_Layer3DDrawOpaque   = 29
PPC_Layer3DDrawXlu      = 30
PPC_ActorJob2_1Ragdoll  = 37
PPC_SystemTaskDrawTV    = 21
```

要判断「`procFrame` 里哪些调用是状态更新、哪些是绘制请求」，必须先把整帧调用结构测绘出来。**这个 profiler 是立体渲染补丁的前置工作，不是事后优化工具。**

**运行时观测层。** `src/hooking/entity_debugger.cpp`（674 行）是 ImGui actor 检查器——世界空间 overlay、AABB 可视化、内存值实时监视。配套 `patch_debug_CTRL_Logging.asm`、`patch_debug_RND_Logging.asm`。

**A/B 验证机制。** `patch_STUB_StereoRenderingDebugging.asm` 提供 stub hook，可在不改主逻辑的前提下切换行为对比。`patch_RND_Find3DFrameBuffer.asm` 的魔数清屏色（`0.123456789`）既是运行时通信手段，也是一种**发现技术**：先染色，再找出哪个 buffer 是哪个。

### 2.3 推断出的工作流

```
静态逆向（IDA/Ghidra 分析 RPX，复用社区符号库）
   ↓
写 logging/profiling 补丁插桩 → 在 Cemu 里跑 → 读日志验证假设
   ↓
entity_debugger 运行时观测内存布局与调用时序
   ↓
codecave 写 hook → bl import.coreinit.hook_X → C++ 侧实现
   ↓
stub hook A/B 对比 → 迭代
```

### 2.4 定制能力：机制通用，知识不通用

- **机制**：graphic pack 补丁系统对**任何 Wii U 游戏**可用。社区早就用它做 60fps 补丁、超宽屏、FOV 调整。给别的游戏做 VR，工具链一行不用改。
- **知识**：7692 行补丁编码的全是「BotW 的 sead/agl/gsys 引擎在哪里可以被撬开」。换游戏 = 从零；换 BotW 版本 = 139 个地址全部失效。
- **人的门槛**：作者 Crementif 是知名 Cemu graphic pack 作者，本身深度熟悉 BotW 引擎。这不是「学会工具就能做」的事。

**规模参考**：7692 行 PPC 汇编（31 个文件）+ 67 个 HLE hook + ~7000 行 C++ hook 层 + ~3400 行渲染层。

---

## 3. 3DS / azahar：立体几乎白拿，玩法层要造基础设施

### 3.1 决定性优势：硬件原生立体

azahar 渲染器里：

```cpp
right_eye ? framebuffer.address_right1 : framebuffer.address_left1
```

**3DS 硬件的帧缓冲天然分左右眼，游戏自己就在渲染两只眼睛**（系统级 3D 滑块能力）。
`src/common/settings.h:89` 的 `StereoRenderOption` 已经有 SideBySide / Anaglyph / CardboardVR 等模式。

这意味着 BetterVR 花力气最大的那块——**让游戏跑两遍 + 过滤状态更新 + 替换相机**
（`patch_RND_StereoRendering*.asm` 约 2000 行 + `camera.cpp` 1188 行）——**整块不需要**。

而且**没有 2× draw call 代价**，游戏本来就这么跑。

### 3.2 其他优势

- azahar 已有 OpenXR 集成：`src/android/app/src/main/jni/vr/`（含 `OpenXRManager.cpp`、
  `xr_full_immersive_head_look.cpp`、`vr_session_bridge.cpp`），当前分支
  `feature/malos/full_immersive`。
- foundation 的 XR 模块本来就是从 azahar 沉淀出来的，接口天然贴合。
- 3DS 模拟的性能压力远低于 Wii U/Switch。

### 3.3 两个真实障碍

**障碍一：没有 codecave 补丁基础设施。**
azahar 只有 Gateway 金手指（`src/core/cheats/gateway_cheat.cpp`）——内存写值，
**不是**带符号标签的 ARM11 汇编器 + codecave + HLE 回调注册。
要做 BetterVR 那种深度引擎 hook，得先把这套基础设施造出来。

对照 Cemu 已有的四件套（汇编器 / codecave / 符号标签 / HLE 回调），azahar 一件都没有。

**障碍二：3DS 的立体参数是为掌机屏幕设计的。**
视差预算对应 400×240 屏幕在臂展距离，不是头显。直接把两眼图送进 XR projection layer，
深度感会严重不足。要放大分离度就得干预游戏读取 3D 滑块值的逻辑——**又回到游戏专属逆向**，
而且改大了会破坏 UI 布局与特效缩放（同 BetterVR 里 FOV 必须「融合」而非「替换」的道理）。

### 3.4 成本判断

| 目标 | 相对 BotW VR 的成本 |
|---|---|
| 平面影院模式 | 已基本具备 |
| 立体 VR（几何正确） | **显著更低**——几何立体现成，主要工作在 IPD/FOV 重映射与 UI 处理 |
| 完整 VR 体验（手部 / roomscale / 武器） | **同一量级**，且需先补齐补丁基础设施 |

---

## 4. NS / eden：符号知识最好，立体机制要从零造

评估对象：`~/workspace/emulations/switch/eden-emulator`（yuzu 系 fork，有 Android 支持）。

### 4.1 优势

**补丁基础设施已存在。** `src/core/file_sys/ips_layer.cpp` + `patch_manager.cpp`，
支持 IPS32 格式——即 Switch 改机圈（Atmosphère / pchtxt）的标准，生态成熟。

**符号知识最好（原本判断为最大优势，§6 后需修正）。** `zeldaret/botw` 反编译项目的
目标就是 **Switch 1.5.0**，是真正的反编译源码而非逆向猜测。

**但 §6 的分析削弱了这条作为「平台优势」的分量，有两个原因：**

1. **它是可迁移资产，不是平台绑定资产。** 反编译描述的是同一份源码，可通过
   字符串 / vtable / 调用图锚点映射到 Wii U 侧——BetterVR 事实上已经做过一次
   （§6.3 的 47 个符号）。留在 Wii U 平台同样能吃到这份成果。
2. **渲染层覆盖率未知。** 项目重心在 AI/action 系统（§6.5），而 VR 最需要的是
   `agl::lyr::Layer` / `gsys::SystemTask` 这些渲染与相机路径。这一项未核实前，
   不应把「有完整反编译」当作 NS 的既定优势。

**设计知识可直接迁移。** 是同一个游戏，BetterVR 的全部设计结论（hook 哪里、
过滤什么、UI 怎么处理、舒适度怎么调）原样适用。

**架构亲和**：ARM64 guest 跑在 ARM64 Android host 上，翻译成本理论上更低。

### 4.2 三个硬问题

**问题一：没有 guest→native 回调机制。**
核查后在 eden 里找不到 `osLib_registerHLEFunction` 的对应物。IPS 只能改字节，
不能让补丁后的 guest 代码调进模拟器 native 层。这套要自己造
（加自定义 SVC 或指令陷阱 + 回调注册表）。不算大工程，但是新工作，
而且它是 BetterVR 整套架构的地基。

**问题二：没有原生立体，也没有着色器覆盖生态。**
Cemu 的 graphic pack 着色器替换是社区用了多年的成熟机制（BetterVR 自带几十个
`*_vs.txt` 即是证明）；Switch 侧 Maxwell/NVN 的着色器注入难度高得多。
**所以立体既不能白拿，也没有现成注入点**——两条路（guest 双渲染 / 模拟器侧注入）
都要从零设计。

**问题三：项目与法律风险。** yuzu 和 Ryujinx 均已被关停，在 fork 上建产品不稳。

### 4.3 成本判断

符号知识省下的量确实很大，但**立体机制要从零设计，且缺少 Cemu 那样的着色器覆盖机制**。
**总体不比 Wii U 便宜，风险更高。**

---

## 5. 逆向工具链：IDA 能否用于主机游戏可执行程序

### 5.1 结论

**可以，IDA 是这类工作的标准工具。** 但每个平台都有两道门槛：**加载器**和**反编译器授权**。
IDA 本体只负责「认识这个 CPU」，「认识这个文件格式」通常要靠加载器插件。

### 5.2 各平台格式与支持情况（概览）

| 平台 | CPU | 可执行格式 | IDA 处理器支持 | 加载器 | 反编译器 |
|---|---|---|---|---|---|
| **Wii U** | PowerPC "Espresso"（PPC750 系，**大端**） | RPX / RPL（改造版 ELF32，分段 zlib 压缩 + RPL 专有节） | 内置 PPC | 自带 ELF 加载器**处理不了压缩段**，需预处理或插件 | PowerPC 反编译器是 **Hex-Rays 独立付费模块** |
| **3DS** | ARM11（ARMv6K），小端 | CXI 容器 / ExeFS `.code`（需解密+解压）/ CRO、CRS 动态模块 | 内置 ARM | 需社区加载器；解密解压后的 `.code` 可当 raw binary 加载 | ARM 反编译器，**普及度最高** |
| **NS** | ARM64（Cortex-A57） | NSO0（LZ4 压缩）/ NRO0 / KIP1 | 内置 ARM64 | 社区标准 `nxo64.py`（reswitched 系） | ARM64 反编译器，成熟 |
| GameCube/Wii（对照） | PowerPC Gekko/Broadway | DOL / REL | 内置 PPC | 需社区加载器 | 同 Wii U |

**对本项目最关键的一条**：Wii U 是 **PowerPC + 大端**，而 **Hex-Rays 的 PowerPC 反编译器是单独售卖的模块**。
没有它就只有反汇编，逆向效率差一个量级。3DS/Switch 用的 ARM/ARM64 反编译器普及度高得多。

这解释了 BetterVR 符号表里为什么会出现 `sub_31FB1B4_getTimeForGameUpdateMaybe` 这种命名——
在大端 PPC 上做纯反汇编逆向，函数语义往往只能猜。

### 5.2.1 Wii U：RPX / RPL 详解

**格式实质**（以下结构常量均从本仓 Cemu 源码核实，可作权威参考）：

RPX 是**改造过的 ELF32-PPC-BE**。`RPX` 是可执行体，`RPL` 是共享库（相当于 DLL）。
相对标准 ELF 的差异集中在两处：

*一、分节 zlib 压缩*（`src/Cafe/OS/RPL/rpl_structs.h:28`）：

```c
#define SHF_RPL_COMPRESSED   0x08000000
#define SHF_TLS              0x04000000
```

带该 flag 的节，节内容是 zlib deflate 流，前 4 字节为解压后大小。
Cemu 的解压路径在 `rpl.cpp:370-402` 与 `rpl.cpp:1256-1278`（`inflateInit` / `inflate`）。
**这是 IDA 自带 ELF 加载器失效的直接原因**——它会把压缩数据当作节内容直接映射。

*二、RPL 专有节类型*（`rpl_structs.h:9-12`）：

```c
#define SHT_RPL_EXPORTS    0x80000001   // 导出表
#define SHT_RPL_IMPORTS    0x80000002   // 导入表（BetterVR 的 import.coreinit.hook_X 就走这里）
#define SHT_RPL_CRCS       0x80000003   // 各节 CRC
#define SHT_RPL_FILEINFO   0x80000004   // 模块元信息
```

`SHT_RPL_IMPORTS` 尤其值得注意：**BetterVR 的 `bl import.coreinit.hook_X` 之所以能工作，
就是因为它往导入表里加了条目**，Cemu 的 RPL 加载器解析导入时把它解析到
`osLib_registerHLEFunction` 注册的 native 函数。理解这张表是理解整套 hook 机制的前提。

*三、PPC 重定位类型*（`rpl_structs.h:32-37`）：

```c
RPL_RELOC_ADDR32 = 1,  LO16 = 4,  HI16 = 5,  HA16 = 6,  REL24 = 10,  REL14 = 11
```

标准 PPC ELF 重定位子集。节头结构里带 `virtualAddress` 字段
（`rpl_structs.h`，`rplSectionHeader` 的 +0x0C），**即 RPX 自己声明各节的加载地址**。

**三条可选路径：**

| 路径 | 做法 | 评价 |
|---|---|---|
| **A（推荐）** | 用 **`wiiurpxtool`**（0CBH0）把 RPX 解压成标准 ELF：`wiiurpxtool -d in.rpx out.elf`，再用 IDA 自带 ELF 加载器打开（处理器选 PPC 大端） | 最稳，不依赖插件生死；解压后是合法 ELF，节地址、符号、重定位都被标准加载器正确处理 |
| B | 社区 IDA / Ghidra RPX 加载器插件 | 省一步预处理，但**插件的存在与维护状态请自行确认**——本文不点名，避免引用已失效的项目 |
| C | 自己写加载器 | **本仓的 `src/Cafe/OS/RPL/rpl.cpp` + `rpl_structs.h` 是一份完整、可运行、经过验证的格式参考**，照它写一个 IDA loader 脚本工作量不大 |

**地址正确性（本项目最要紧的一点）。** Cemu 的 Wii U 内存布局（已核实）：

```
src/Cafe/HW/MMU/MMU.cpp:114   TEXT_AREA  0x02000000 ~ 0x0E000000   // 模块 text 节
src/Cafe/OS/RPL/rpl.cpp:56    数据分配器起始 0x10000000
src/Cafe/HW/MMU/MMU.h:151     MEMORY_CODEAREA_ADDR = 0x02000000
```

**这与 BetterVR 补丁里的地址完全吻合：**

```
0x024A6F20, 0x031FB1B4, 0x0344B864 ...   → TEXT_AREA（0x02xxxxxx / 0x03xxxxxx）
0x1011A7A0, 0x10463708, 0x10549DD0 ...   → 数据区（0x10xxxxxx）
```

**这不是巧合，而是一个可用的自检手段**：如果你的 IDA 数据库里 BotW 的代码不落在
`0x02000000`–`0x0E000000`、数据不落在 `0x10000000` 起，那基址就是错的，
所有地址都需要 rebase，写出来的补丁会全部打错位置。

**验证方法**：在 Cemu 里加载游戏 → 调试器读一个已知函数的实际地址 → 与 IDA 中的地址比对。
配合 §5.5 的 `.map` 导入，这个校验可以做成常规步骤。

**大端注意事项**：IDA 不会自动帮你换字节序。每个 guest 结构体都要按大端解读，
定义镜像类型时要显式处理——BetterVR 的 `BESeadPerspectiveProjection`、`BEVec3`
的 `BE` 前缀就是这套约定。浮点常量同样是大端存储。

### 5.2.2 3DS：CXI / ExeFS / CRO 详解

**容器层层嵌套**，IDA 之前要剥好几层：

```
.3ds / .cci（卡带镜像）或 .cia（可安装包）
   └─ NCCH 分区（需解密）
        └─ CXI（可执行内容）
             ├─ ExeFS
             │    ├─ .code   ← 真正的 ARM11 可执行体（通常还被压缩）
             │    ├─ .rodata
             │    └─ .data
             └─ RomFS（资产，逆向代码时不需要）
```

**提取工具链**：

- **`ctrtool`**（Project CTR / 3dstools 系）——解密 NCCH、导出 ExeFS：
  `ctrtool --exefs=exefs.bin --exefsdir=exefs/ input.cxi`
- **`3dstool`** —— 另一套等价工具，处理 CCI/CIA/ExeFS 拆解
- **`.code` 解压**：ExeFS 的 `.code` 常用反向 LZ77（俗称 BLZ）压缩，需专门解压
  （`blz`/`lzss` 类工具，或 `3dstool` 的对应选项）

解密需要合法获取的游戏副本与对应密钥，见 §5.3。

**加载进 IDA 的两条路径**：

| 路径 | 做法 |
|---|---|
| **A（务实）** | 解密解压后的 `.code` 按 **raw binary** 加载，处理器 ARM 小端，**加载基址 `0x00100000`**（3DS 应用代码的标准加载地址） |
| B | 社区 3DS 加载器插件（能直接吃 CXI/ExeFS 并自动分段），存在与维护状态请自行确认 |

**CRO/CRS 是额外的复杂度**：部分游戏把代码拆成动态模块（相当于 DLL），
运行时加载并重定位。这类模块要单独分析，且**运行时基址不固定**——
如果目标函数在 CRO 里，地址会随加载顺序变化，硬编码地址不可靠。
分析前先确认目标代码在 `.code` 还是 CRO 里。

**与 azahar 的关系**：azahar 只有 Gateway 金手指（§3.3），没有符号桥。
若要做 3DS 侧深度逆向，「IDA ↔ 模拟器调试器」的闭环需要自己建，
Cemu 侧现成的那套（§5.5）在这里没有对应物。

### 5.2.3 NS：NSO0 / NRO0 详解

**格式实质**：

- **NSO0** —— 正式游戏可执行体格式。头部含魔数 `NSO0`、各段（text/rodata/data）的
  文件偏移、内存偏移、大小，以及**每段独立的 LZ4 压缩**标志与压缩后大小；
  还带 BuildID（模块唯一标识，做版本比对很有用）与段哈希。
- **NRO0** —— homebrew 格式，结构更简单，通常不压缩。
- **KIP1** —— 内核初始进程，逆向系统模块时才会遇到。

段内还有 **MOD0** 头，指向 `.dynamic`、`.bss`、eh_frame 等，
以及需要处理的 `R_AARCH64_RELATIVE` 等重定位。

**提取工具链**：

- **`hactool`** / **`nstool`** / `hactoolnet`（LibHac 系）—— 解密 XCI/NSP、导出 ExeFS
- BotW 的主体可执行文件是 ExeFS 里的 **`main`**
- **注意版本合并**：1.5.0 是 base + update 的合并结果，**必须把 update 的 ExeFS
  叠加到 base 上**才能得到与 `zeldaret/botw` 目标一致的二进制。
  只分析 base（1.0.0）会与反编译项目的地址完全对不上——**这是 §6.6 版本对齐问题的实际操作面**。

**加载进 IDA**：

- **`nxo64.py`**（hthh / reswitched 系）是事实标准。它负责：解 LZ4、按段建立内存布局、
  处理 MOD0 与重定位、恢复动态符号。有多个维护分支，选一个活跃的。
- Ghidra 侧对应 **`switch-loader`**（Adubbz）。

**优势**：Switch 二进制通常保留较多动态符号信息，加上 `zeldaret/botw` 的
`uking_functions.csv`，起点比 Wii U 好得多——但**渲染层覆盖率仍需先核实**（§6.5）。

### 5.2.4 三平台共同的两件事

**一、IDA 之前的提取与解密是独立工序**，见 §5.3。不要把它算进「IDA 会不会用」的评估里。

**二、基址对齐是所有平台的共同陷阱。** 三个平台都存在「IDA 里的地址 ≠ 模拟器运行时地址」
的可能：

| 平台 | 风险来源 | 校验方式 |
|---|---|---|
| Wii U | 压缩节处理不当、未读 `virtualAddress` | 代码应落在 `0x02000000`–`0x0E000000`，数据在 `0x10000000` 起 |
| 3DS | raw 加载时基址填错；CRO 模块基址运行时才定 | 主 `.code` 基址应为 `0x00100000`；CRO 需运行时确认 |
| NS | 段内存偏移与文件偏移混淆；未叠加 update | 用 BuildID 确认二进制身份；与 `uking_functions.csv` 抽样比对 |

**做法建议**：定位到第一个函数后，立刻用模拟器调试器验证该地址真的是那个函数
（下断点、看是否命中），**再继续往下做**。BetterVR 的 139 条映射如果基址错了，
错的是全部 139 条。

### 5.3 IDA 之前的前置步骤（与 IDA 无关但必须做）

拿到可分析的二进制本身是独立工序：

- **Wii U**：从 WUA / WUP / 光盘镜像提取并解密 → 取出 RPX/RPL
- **3DS**：CIA/3DS 镜像解密 → 提取 ExeFS 的 `.code`
- **NS**：XCI/NSP 解密 → 提取 NSO

这一步需要合法获取的游戏副本与对应密钥。**评估时不要把它算进「IDA 会不会用」，
它是另一件事。**

### 5.4 真正的难点不是反汇编

主机游戏二进制的共同特征：

- **完全 strip**，无符号（Switch 部分构建保留少量信息）
- **静态链接**，引擎与游戏代码混在一起
- **重度 C++**：虚表、模板膨胀、大量小函数内联
- 大端平台（Wii U）还要处理**字节序敏感的结构体定义**——
  BetterVR 里 `BESeadPerspectiveProjection`、`BEVec3` 的 `BE` 前缀正是这个原因，
  每个 guest 结构体都要写成大端感知的镜像类型

**所以工作量的大头是恢复类型与结构，不是看懂指令。** 这也是社区符号库
（`zeldaret/botw` 等）价值极高的原因——它省掉的正是最贵的那部分。

### 5.5 静态 + 动态结合：本项目已经具备的闭环

纯静态逆向效率有限。Cemu 侧的设施可以形成闭环：

1. IDA 里做静态分析，命名函数与结构
2. **导出 `.map` 符号表**
3. **加载进 Cemu 调试器**——上游 `7ff99a5e`（#1916）刚加的能力，已随 C1 同步进本仓
4. Cemu 调试器下断点 / GDB stub 附加，验证静态推断
5. 写 logging 补丁批量插桩（BetterVR 的做法），跑一遍读日志
6. 结论回写 IDA

**第 3 步是新能力，本仓刚拿到。** 在此之前 IDA 与 Cemu 调试器之间没有符号桥，
BetterVR 的 139 个地址是手工维护的。

### 5.6 用签名替代硬编码地址（对版本锁的直接解法）

BetterVR 最大的脆弱点是 **139 个硬编码地址锁死 BotW v208**。
IDA 的签名能力可以直接缓解这一点：

- 为关键函数生成**字节签名 / 交叉引用签名**，而不是记录绝对地址
- 换游戏版本时用签名重新定位，而不是全部重做
- 把「签名 → 地址」的解析放进补丁生成步骤，`.asm` 里引用符号名

本会话配置的 IDA Pro MCP 恰好带了这套工具：`make_signature`、
`make_signature_for_function`、`find_xref_signatures`、`infer_types`、
`type_apply_batch`、`trace_data_flow`、`survey_binary`、`analyze_component` 等。
**当前无打开的 IDB**（`idb_list` 返回空），要用需先 `idb_open`。

**这是一条本文认为值得单独立项的改进**：若要走路线 A/C（移植 BetterVR 补丁），
先建立签名化的地址解析，能把「换版本 = 全部重做」降级为「换版本 = 重跑签名匹配」。

> **重要限定**：本节讲的签名是**同架构跨版本**（Wii U v208 → 其他 Wii U 版本）。
> **跨架构**（Switch → Wii U）字节签名完全无用，需要的是语义级锚点，见 §6.4。
> 这两件事不能混用。

---

## 6. 符号来源：zeldaret/botw 反编译项目

### 6.1 项目是什么（2026-07-26 实际查询确认）

| 项 | 值 |
|---|---|
| 仓库 | `references/botw`（`tencentmalos/botw` fork 自 `zeldaret/botw`） |
| 目标 | **The Legend of Zelda: Breath of the Wild v1.5.0（Nintendo Switch）** |
| 性质 | 实验性、进行中的**匹配式反编译**（matching decompilation） |
| 明确声明 | 不含游戏资产或 RomFS，**无法用于运行游戏** |
| 目的 | 理解游戏内部机制、辅助 glitch 研究、沉淀既有逆向知识 |
| 规模 | 2.1k star、123 fork，有活跃 Discord |
| 构建 | CMake + Clang（`.clang-format`、`.clang-tidy`） |
| 进度 | README 用 badge 展示（总体进度 + 函数级进度），**具体百分比需实时查 `botw.link/progress`**——本次未取到数字，不要引用任何记忆中的百分比 |

所属组织 `zeldaret` 同时维护 oot、mm、tp 等塞尔达系列反编译项目。BotW 是 C++ 项目
（不像 oot/mm 是 C），因此反编译产物本身就是可读的 C++ 类与方法。

### 6.2 关键产物：符号与结构数据

`data/` 目录（已确认存在）：

| 文件 | 内容 | 对 VR 工作的价值 |
|---|---|---|
| **`uking_functions.csv`** | 函数清单：地址 + 名称（+ 状态） | **最核心**——Switch 1.5.0 的函数符号表 |
| `data_symbols.csv` | 数据符号映射 | 全局变量、单例、常量表定位 |
| `aidef_action_vtables.yml` | action 系统虚表定义 | **跨架构映射的锚点**，见 §6.4 |
| `aidef_ai_vtables.yml`、`aidef_vtables.yml`、`as_element_vtables.yml` | 各类虚表定义 | 同上 |
| `aidef_action_params.yml`、`aidef_ai_params.yml` | AI/action 参数定义 | 玩法层 hook 用 |
| `status_action.yml`、`status_ai.yml`、`status_query.yml` | 状态追踪数据 | 进度与覆盖面参考 |

命名空间体系：`ksys::`（KingSystem，游戏自己的框架）、`sead::`（Nintendo 标准库）、
`agl::`（Nintendo 图形库）、`gsys::`。相关生态还有 `open-ead/sead`（sead 库的重实现）
与社区的 BotW 逆向笔记。

### 6.3 BetterVR 确实在用它（有证据）

核查 BetterVR 仓库，`ksys` / `sead` / `agl` / `gsys` 命名空间的符号有 **47 个不同名字**：

```
agl__lyr__Layer__getRenderCamera          ← 立体渲染的核心 hook 点
agl__lyr__Layer__getRenderProjection      ← 同上
agl__lyr__LayerJob__invoke
gsys__SystemTask__drawTV_
gsys__SystemTask__postCalc_
gsys__ModelJobQueue__clear                ← preventModelQueueClear 相关
ksys__act__ai__InlineParamPack__addBool
ksys__phys__ModelBoneAccessor__getBoneName  ← 手部骨骼绑定
sead__SafeString__vt
```

这些名字不可能是独立逆向出来的——**它们就是反编译项目的命名**。
BetterVR 把这些符号名与自己在 Wii U RPX 里定位到的地址配成 139 条
`0xADDR = name:` 映射。

**也就是说：BetterVR 事实上已经完成了一次「Switch 符号 → Wii U 地址」的跨平台映射**，
只不过映射结果（139 条）是手工产物，中间过程和完整数据库没有公开。

### 6.4 跨架构映射的技术现实（最关键的一节）

BotW 的 Wii U 版与 Switch 版是**同一份源码的两个编译产物**：
Wii U 是 PowerPC 大端（Cafe SDK 工具链），Switch 是 ARM64 小端（clang）。

**完全不能映射的：**

- **绝对地址** —— 不同 CPU、ABI、编译器、优化策略，地址空间毫无关系
- **字节签名 / 指令模式** —— 跨架构指令编码完全不同，`make_signature` 那类
  字节签名在这里**一点用都没有**（这是 §5.6 的适用边界）
- 寄存器分配、栈帧布局、内联决策

**可以映射的（按可靠性排序）：**

| 锚点 | 为什么可靠 | 工具 |
|---|---|---|
| **字符串字面量** | 同一份源码里的字符串完全相同，且在两个二进制里都以明文存在 | 直接搜字符串 → 找 xref → 定位函数 |
| **虚表结构与方法顺序** | 同一源码的虚函数声明顺序相同 → vtable 布局同构 | `aidef_*_vtables.yml` 直接给出顺序；IDA 里找结构相同的 vtable |
| **浮点常量 / 魔数** | 物理参数、阈值在源码里是字面量，两边都保留 | BotW 物理常量很多，是好锚点 |
| **调用图形状** | 同一源码的调用关系拓扑相同 | 从已确认锚点向外扩散；IDA 的 `callgraph` / `xref_query` |
| **类字段顺序** | 源码声明顺序决定 | 但 **padding/对齐因 ABI 而异**，且 PPC 是大端 → 每个结构体要写字节序感知的镜像类型 |

**BetterVR 已经在用字符串锚点，补丁里有直接证据：**

```
0x1011A7A0 = str_IsShootByPlayer:
0x1011A7F4 = str_TargetPos:
```

这两个是 BotW AI 参数名字符串。把它们标出来，就能通过 xref 找到读取这些参数的函数。

**结论**：跨架构映射是可行的，但它是**半自动的语义级工作**，不是「跑个签名匹配就完事」。
本会话的 IDA MCP 里真正对得上的工具是 `find_xref_signatures`、`xref_query`、
`callgraph`、`search_text`、`search_structs`、`type_apply_batch`，
**不是** `make_signature`（那是同架构用的）。

### 6.5 一个必须先确认的覆盖面问题

`data/` 目录的构成暴露了一个风险：**9 个 YAML 里有 5 个是 `aidef_*`（AI/action 定义），
外加 3 个 `status_*` 也都是 action/ai/query 状态。**

这说明**反编译项目的重心在 AI / action 系统**——这符合社区动机（glitch 研究、
行为分析）。而 **VR 最需要的恰好是渲染与相机路径**：`agl::lyr::Layer`、
`gsys::SystemTask`、`gsys::ModelJobQueue`。

**这两个方向可能不重合。** BetterVR 用到的 47 个符号里，`agl__lyr__` 和 `gsys__`
占了相当比例，但这不能证明反编译项目对渲染层的覆盖率高——BetterVR 可能是自己
逆向出这些渲染函数、只借用了命名约定。

**动工前必须实测的一件事**：拉下 `uking_functions.csv`，统计 `agl::`、`gsys::` 前缀
函数的**数量与已匹配状态**。如果渲染层覆盖率很低，那「Switch 有完整反编译源码」
这个优势**对 VR 工作基本不成立**，§1 结论表里 NS 的那项优势要打折。

### 6.6 版本对齐（待核实）

- 反编译项目目标：**Switch 1.5.0**（已确认）
- BetterVR 目标：**Wii U update v208**（补丁全部标 `_V208`，`moduleMatches = 0x6267BFD0`）

**Wii U 的 v208 是否就等于 1.5.0，本次未能核实**（尝试查询版本历史时被目标站点拒绝）。
若两者确为同一游戏版本，则符号语义完全对齐，映射可靠性最高；
若不是，跨版本差异要叠加在跨架构差异之上。

**这是一条 load-bearing 的假设，建议优先核实**——它决定了能不能把反编译成果
直接当作 Wii U 侧的语义参照。

### 6.7 局限与风险

1. **进度不完整。** 是 WIP 项目，且未取到具体百分比。不要假设「符号都有」。
2. **渲染层覆盖率未知。** 见 §6.5，这是对本项目最要紧的一条。
3. **它是 Switch 版。** Wii U 是先发平台，`agl`/`gsys` 的版本可能有实际差异
   （不只是编译差异）。
4. **许可与合规。** 项目本身不含游戏资产，但把其符号数据引入商业/内部产品前，
   需确认许可条款与公司合规要求。**这一项不要由工程侧自行判断。**
5. **不能用于运行游戏。** 项目明确声明，不要误解为「有源码就能直接编译一个 BotW」。

### 6.8 建议的使用方式

如果决定利用它：

1. **先做覆盖面调查**（§6.5）——统计 `agl::` / `gsys::` 函数的匹配状态，
   这一步很便宜，但直接决定后面值不值得做。
2. **核实版本对齐**（§6.6）。
3. **确认许可与合规**（§6.7-4）。
4. 建立**语义锚点表**而不是地址表：以「符号名 + 字符串锚点 + vtable 位置 +
   调用图特征」描述每个目标函数，地址是解析结果而非源数据。
   这样换版本或换平台时可以重跑解析。
5. 把解析结果导出为 `.map`，喂给 Cemu 调试器（§5.5 的闭环），
   用动态断点验证每一条映射。**不要在未验证的映射上直接写 hook。**

---

## 7. 决策建议

| 若目标是… | 建议平台 | 理由 |
|---|---|---|
| 快速验证「模拟器 VR」这条产品方向 | **3DS / azahar** | 原生立体 + 已有 XR 集成 + 低性能压力；能在最短时间内拿到可评估的真实体验 |
| 交付 BotW VR | **Wii U / Cemu** | BetterVR 是唯一现成的完整参考实现，设计知识的价值高于 Switch 的符号优势 |
| 长期多游戏 VR 平台 | **Wii U / Cemu** | 补丁基础设施最完整（汇编器 + codecave + HLE 回调 + 着色器覆盖），可复用到其他游戏 |
| —— 不建议起步的 | NS / eden | 立体机制要从零造 + 无 guest→native 回调 + 法律风险 |

**§6 对「交付 BotW VR」那一行的补强**：Switch 的符号优势**不是平台优势，而是可迁移资产**。
反编译成果描述的是「同一份源码」，通过字符串 / vtable / 调用图锚点可以映射到 Wii U 侧
（BetterVR 事实上已经做过一次，见 §6.3）。所以正确的用法是
**留在 Wii U / Cemu 平台，把 Switch 反编译成果当作语义参照引入**，
而不是为了符号去迁移平台。这条让 Wii U 的推荐更稳，而非削弱。

---

## 8. 未验证项

本文的判断基于代码核查与公开资料查询，以下几项仍需实测才能坐实：

| # | 未验证项 | 影响 | 成本 |
|---|---|---|---|
| 1 | **Cemu Android 的 graphic pack `.asm` 补丁与 codecave 是否正常工作**（parser 在跨平台核心代码里，但 Android 上的加载路径、`moduleMatches` 匹配、codecave 分配未实测） | 决定路线 A/C 是否可行 | 低，同 V0-2 |
| 2 | **BotW 在目标设备平面模式的稳态帧率** | **整条路线的门槛** | 中，同 V0-1 |
| 3 | **`uking_functions.csv` 里 `agl::` / `gsys::` 函数的数量与匹配状态**（§6.5） | 决定「Switch 有完整反编译」这个优势对 VR 是否成立 | **很低，建议最先做** |
| 4 | **Wii U v208 是否等于 1.5.0**（§6.6），本次查询被目标站点拒绝 | 决定反编译成果能否直接当 Wii U 语义参照 | 很低 |
| 5 | **zeldaret/botw 的许可与合规适用性**（§6.7-4） | 决定能否使用 | 低，但**不由工程侧判断** |
| 6 | **Hex-Rays PowerPC 反编译器的授权情况** | 直接决定 Wii U 侧逆向效率 | 很低 |
| 7 | **3DS 立体分离度能否安全放大到 VR 可用范围**，以及放大后 UI/特效的破坏程度 | 决定 3DS 路径的实际质量上限 | 中 |
| 8 | **eden 是否真的没有 guest→native 回调机制**（基于 grep 结论，未通读架构） | 影响 NS 成本估算 | 中 |

**建议顺序：先做 3、4、5、6 四项**——它们成本都很低（几乎只是查询与统计），
但其中 3 和 6 会显著改变成本估算，5 是准入前提。第 2 项是真正的门槛，
但需要设备与游戏，安排上可以并行。
