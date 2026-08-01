# BotW VR on Android（Cemu）功能规划

- 日期：2026-07-26
- 参考实现一：`~/workspace/emulations/wii/dolphinxr` — 分析见其
  `docs/DolphinXR-OpenXR-Stereo-and-Frame-Pacing.md`
- 参考实现二：`references/BotW-BetterVR` — 分析与来源索引见
  `docs/bettervr/README.md`
- 关联计划：`docs/plans/2026-07-26-foundation-integration-plan.md`（C6/C7 为本文的前置）
- 本文性质：**方案规划，不是实施 spec。** 多个关键前提尚未验证，落到任务级之前需要先过 V0 门槛。

---

## 0. 结论先行

三条判断，后面所有规划都建立在它们之上：

**一、Wii U 架构决定了不能照搬 DolphinXR。**
DolphinXR 能在模拟器侧做通用立体，是因为 GameCube/Wii 的 GX 是固定功能 transform，
投影矩阵在 XF 寄存器里，模拟器天然拿得到。Wii U 的 Latte 用可编程着色器，投影矩阵藏在
游戏自定义的 uniform buffer 里，**没有通用拦截点**。任何 Wii U 立体方案都必然带有
游戏专属成分。

**二、BetterVR 已经证明「Cemu 上的高质量 BotW VR」可行，但它的代价在移动端可能不成立。**
它让游戏整帧渲染两遍，作者自己在补丁注释里写明「Cemu 要翻译两遍 draw call，而这通常
正是模拟瓶颈」。PC 上用高单线程性能硬扛，Android arm64 上这个前提不存在。

**三、因此第一步不是写代码，是测性能。**
必须先测出 BotW 在目标头显上**平面模式**的基准帧率。如果平面模式已经跑不到 30fps，
立体模式没有讨论价值——这个 gate 应该在投入任何 VR 工程之前完成。

---

## 1. 两个参考实现的适用边界

| 维度 | DolphinXR | BotW-BetterVR |
|---|---|---|
| 立体来源 | 模拟器替换 per-eye projection | **游戏自己渲染两遍** |
| 游戏跑几遍 | 一遍 | **两遍** |
| 主机架构前提 | GX 固定功能 transform | 无（绕过架构问题） |
| 游戏耦合 | 基本通用 + per-game 分类 DB | **单游戏单版本**（BotW v208，`moduleMatches = 0x6267BFD0`） |
| guest 代码补丁 | 无 | **7692 行 PPC 汇编，31 个文件** |
| 剔除正确性 | 需模拟器侧处理 | **游戏引擎天然正确** |
| HUD/skybox | 模拟器侧 per-draw 分类 | 游戏侧改 blend + alpha 遮罩 |
| 手部/武器实体 | 无 | **有**（真实骨骼绑定） |
| multiview | **可用** | 不可用（两遍发生在 guest 层） |
| 移动端友好度 | 较好 | **差** |
| 装载方式 | 编译进模拟器 | 外部 Vulkan layer + DLL 注入 |

**关键认识：两者不互斥。** DolphinXR 贡献的是「怎么用低成本拿到立体几何 + 移动端该怎么
组织渲染」；BetterVR 贡献的是「BotW 引擎在哪里可以被撬开，撬开之后要做什么」。

**另一个关键认识：BetterVR 的装载机制在 Android 上会变简单，不是变难。**
它在 Windows 上要做 Vulkan implicit layer + `GetProcAddress` + launcher，是因为它是外挂
mod；我们拥有 Cemu 源码，hook 模块可以直接编译进 `CemuAndroid`，进程内调用
`osLib_registerHLEFunction`（`src/Cafe/OS/common/OSCommon.cpp:105`），
`PPCInterpreter_t` 直接用 Cemu 自己的定义。连带可以删掉 magic-clear-value 那条
眼别识别通道（有源码后直接读 `currentEyeSide`）。

---

## 2. 三条候选路线

### 路线 A：移植 BetterVR（guest 双渲染）

把 7692 行 PPC 补丁 + 67 个 hook 的语义搬到 Android 的 Cemu 里，hook 实现重写为进程内。

- **优点**：路径已被证明可行；引擎级正确性（剔除、HUD、动画）白拿；手部/武器/舒适度
  这些真正决定 VR 体验的东西直接继承。
- **缺点**：2× draw call 翻译，移动端头号风险；死锁在 BotW v208 单版本；补丁是逆向产物，
  维护成本高。
- **前置未验证项**：Cemu Android 的 graphic pack 系统能否正常应用 `.asm` 补丁与
  codecave（parser 在跨平台核心代码 `GraphicPack2PatchesParser.cpp`，但 Android 上的
  加载路径、`moduleMatches` 匹配、codecave 分配都未实测）。

### 路线 B：DolphinXR 式模拟器侧注入（单渲染 + multiview）

在 Cemu 的 Latte 着色器层注入 per-eye 投影，用 Vulkan multiview 一次 draw 出两眼。

- **优点**：游戏只跑一遍，理论上接近平面模式的 CPU 成本；multiview 契合移动端 tile GPU；
  对游戏版本的依赖比路线 A 小得多。
- **缺点**：没有引擎配合——视锥剔除按原单眼视锥做，视野边缘会有物体被错误剔除；
  HUD/skybox/后处理需要在模拟器侧自己分类；**没有手部、武器、roomscale**，
  只是「立体的第三人称 BotW」。
- **可行性依据**：BetterVR 自带的 `resources/BreathOfTheWild_Graphics/` 里有几十个
  `*_vs.txt` 顶点着色器覆盖文件，**证明 Cemu 的着色器替换机制在 BotW 上是通的**，
  这正是注入 per-eye 投影的落点。
- **前置未验证项**：BotW 的投影矩阵在哪个 uniform buffer 的哪个偏移；Cemu 的 graphic
  pack 着色器能否读到宿主注入的自定义 uniform（per-eye 矩阵要传进去）；Cemu 的 Latte
  渲染器改成 2-layer array render target 的工作量。

### 路线 C：混合（推荐）

**用路线 B 拿几何立体，用路线 A 的一个最小子集补引擎级问题。**

- 几何立体走模拟器侧注入 + multiview（成本可控）
- 只移植 BetterVR hook 中**必要的少数**：可见性检查修正（`patch_FixVisibilityChecks.asm`、
  `hook_CheckIfCameraCanSeePos`）、UI blending 修正（`patch_ImproveGUI.asm`）
- 手部/武器/roomscale 作为**独立的后续增量**，不进入第一个可玩版本

理由：路线 A 的 67 个 hook 里，真正为「立体渲染本身」服务的只是一小部分，大头是玩法
增强。把玩法增强从立体渲染的关键路径上摘出来，能让第一个可验版本早得多，也让性能
风险可分段评估。

**但这个推荐是有条件的**——它成立的前提是路线 B 的两个未验证项（投影矩阵定位、
自定义 uniform 注入）能通过。如果通不过，就只能回退路线 A 并接受 2× 成本，
届时 V0 的性能余量就变成硬约束。

---

## 3. V0：可行性门槛（先做这个，不要跳过）

**这是整个规划的 gate。V0 不过，后面全部不启动。**

### V0-1 平面模式性能基准

在目标头显（Pico 4 / Quest 级）上，用当前 Cemu Android **平面模式**跑 BotW，
测量：

- 稳态帧率（野外、神庙、村镇三种典型场景）
- CPU / GPU 时间分布，确认瓶颈在 draw call 翻译还是别处
- 着色器编译卡顿的严重程度（BotW 首次运行的 shader cache 问题在 PC 上就很明显）
- 热节流后的持续帧率（移动端跑 10 分钟后的数字才是真实数字）

**判定：**

| 平面模式稳态帧率 | 结论 |
|---|---|
| < 20 fps | **停止。** 立体不可能，先解决基础模拟性能 |
| 20–30 fps | 只有路线 B/C 有讨论价值，且需要 heartbeat/reprojection 兜底 |
| > 30 fps | 路线 A 也可评估 |

### V0-2 graphic pack 机制在 Android 上的可用性

- `.asm` 补丁能否加载、`moduleMatches` 能否匹配、codecave 能否分配
- 着色器覆盖（`*_vs.txt`）能否生效
- 用 BetterVR 自带的 `BreathOfTheWild_Graphics` 包做验证，它是现成的测试样本

### V0-3 路线 B 的两个未知项

- BotW 投影矩阵的 uniform buffer 位置（可借助 RenderDoc / Cemu 的着色器 dump）
- Cemu graphic pack 着色器读取宿主注入 uniform 的机制是否存在、够不够用

**V0 退出标准：** 三项都有实测结论，路线选择（A/B/C）有依据地定下来，写成决策记录。

---

## 4. 分阶段规划（V0 通过后）

阶段编号用 `V0…V6`，与 foundation 接入计划的 `C1…C7` 区分。

| 阶段 | 内容 | 依赖 | 风险 |
|---|---|---|---|
| **V0** | 可行性门槛（见上） | 无 | — |
| **V1** | Flat baseline：XR session + 平面 quad | C6/C7 | 中 |
| **V2** | 最小立体：per-eye 几何，只处理普通 perspective world | V0 定路线 | **高** |
| **V3** | pose 正确性：帧边界、render pose stamping | V2 | 高 |
| **V4** | 内容分类：skybox、HUD、后处理、剔除修正 | V3 | 高 |
| **V5** | 移动端性能：multiview、foveation、pacing | V4 | 高 |
| **V6** | VR 玩法：手部、武器、roomscale、舒适度 | V5 | 高 |

这个顺序直接沿用 DolphinXR 文档 §10 的 Phase 0–5，它是从一个已完成的模拟器 XR 改造里
总结出来的，没必要另发明一套。

### V1 Flat baseline

**就是现有计划的 C6 + C7**，不重复展开。产出：OpenXR session/reference space 正确、
mono framebuffer → quad layer、wait/begin/end frame 正确、退出与切游戏不崩。

**为什么必须先做**：它是 XR 生命周期、swapchain、输入的低风险验证基线，也是所有不兼容
情况的兜底显示模式（菜单、FMV、加载）。DolphinXR 保留了 Flat 模式并与 Stereo 共存，
这个设计要照抄。

**照抄的具体设计（DolphinXR 文档 §3.1）**：不要用一个 flag 混表示「需要 XR session」和
「需要双眼 3D」。三个状态相互关联但不等价：

- XR session 生命周期
- 立体渲染策略（Off / Stereo）
- 呈现 layer 类型（Quad / Projection）

### V2 最小立体

按 V0 定下的路线实施。无论哪条路线，退出标准一致：

- 静止时左右眼有合理 IPD 视差
- **交换左右眼会立即产生明显错误**（这是最好的 sanity check，DolphinXR 文档 §11 提出）
- 头部旋转不改变世界尺度
- 只要求普通 perspective world draw 正确，HUD/skybox 允许错

### V3 pose 正确性

**这一步比它看起来重要。** DolphinXR 文档 §3.7 专门论述：模拟器不是普通游戏引擎，
GPU FIFO、framebuffer copy、present 和 OpenXR frame loop 分属不同线程。若渲染用 pose A
但 `xrEndFrame` 声明 pose B，compositor 会以错误基准做 reprojection，产生抖动和左右眼
不一致。

要求：

- 定义可靠的 game frame boundary
- 同一 game frame 的所有 draw 使用同一份缓存 pose
- image 与 render pose **原子式交接**
- 提交时用 stamped pose，不用 live pose

### V4 内容分类

Wii U 侧没有 Dolphin 的 BP/XF 语义可用，分类信息要另找来源（着色器 hash、render target
状态、graphic pack 层面的标注）。至少要区分：

- perspective world
- skybox（**只应用头部旋转，不应用 IPD 与头部平移**，否则天空盒会像近处物体一样产生视差）
- ortho HUD / 菜单
- render-to-texture / offscreen
- 后处理

**剔除修正**在这一步：路线 B/C 下视锥剔除按原单眼视锥做，视野边缘会丢物体。
BetterVR 的 `hook_CheckIfCameraCanSeePos` 和 `patch_FixVisibilityChecks.asm` 是现成解法。

### V5 移动端性能

- **Vulkan multiview 优先**（`arraySize=2` layered swapchain，`gl_ViewIndex` 选矩阵），
  per-eye 双 swapchain 作为 fallback——DolphinXR 文档 §3.5/§3.6 明确这是移动端最有价值的部分
- foveation
- 避免多余 framebuffer blit
- pacing：**先用 runtime reprojection，不要一上来就自己做 heartbeat**。
  DolphinXR 早期用 Opcode Replay 补帧，后来删掉改成 pacing thread + heartbeat +
  compositor ATW，其文档 §5–§7 详细论证了为什么。**不要复活 command replay。**

关于 pacing 有一条必须写清楚的认知（DolphinXR 文档 §7.3）：重提交上一帧 + ATW
只能改善头动响应和刷新连续性，**不能创造新的游戏状态**。物体动画、physics 仍停在旧帧。
它不是插帧，不能把 30fps 的游戏变成真正的 90fps。

### V6 VR 玩法

到这一步才谈手部、武器、roomscale——即 BetterVR 真正的价值所在。
`weapon.cpp`、`skeleton.cpp`、`bow.cpp`、`entity_controller.cpp` 加起来 3400 行，
配套 PPC 补丁另算。这是**独立的大工程**，不应与立体渲染混在一个阶段里评估。

舒适度调整（`camera.cpp` 里的蹲伏/游泳/骑乘偏移等）虽然代码量小，但是实际体验的来源，
无法从任何框架白拿，必须逐项调。

---

## 5. 与 foundation 接入计划的衔接

| 本文阶段 | 对应现有计划 | 说明 |
|---|---|---|
| V1 | **C6 + C7** | Flat baseline 就是现有计划的 XR 前置与影院模式 |
| V2 起 | 现有计划 §4 提到的「可能需要的 C8」 | 本文即是对那个 C8 的展开 |

现有计划 C6 的帧路径选型决策（D7）**必须结合本文重新评估**：

- 如果最终要做立体（V2+），**Surface path 出局**——立体需要 per-eye 渲染目标和
  multiview layered swapchain，Surface 中转路径给不了。
- 也就是说：**只要 BotW VR 是目标，C6 的 D7 就应该直接选 Vulkan path**，
  不必再纠结。这是本文对现有计划最直接的输入。

foundation 侧：`spatial::foundation_xr` 提供 session/layer/输入框架，
但立体投影注入、per-draw 分类、guest hook 全部是 **cemu 侧宿主逻辑**，
不能进 foundation（违反其「不含游戏语义」原则）。BotW 专属逻辑的落点需要单独定：
建议放 cemu 侧独立模块，与 graphic pack 系统对接，不污染 Cemu 核心。

---

## 6. 关键未知与决策点

| 编号 | 问题 | 何时必须回答 |
|---|---|---|
| **V-D1** | BotW 在目标设备平面模式的稳态帧率 | V0-1，**门槛** |
| **V-D2** | Cemu Android 的 graphic pack `.asm` 补丁 + codecave 是否可用 | V0-2 |
| **V-D3** | BotW 投影矩阵的 uniform 位置 + Cemu 自定义 uniform 注入机制 | V0-3，决定路线 B 是否成立 |
| **V-D4** | 路线 A / B / C 选择 | V0 结束 |
| **V-D5** | 目标是「立体第三人称」还是「完整 VR 体验（手部/roomscale）」 | V0 结束前，决定是否规划 V6 |
| **V-D6** | BotW 版本策略：锁 v208 复用 BetterVR 补丁，还是自己重做逆向 | 若走路线 A/C |
| **V-D7** | BotW 专属逻辑的代码落点（独立模块 / graphic pack 扩展 / 其他） | V2 之前 |

**V-D5 需要产品侧回答，不能由工程侧默认。** 「立体的 BotW」和「BetterVR 那样的 BotW VR」
是两个产品，工作量差一个数量级。

---

## 7. 验证矩阵

沿用 DolphinXR 文档 §11，按本项目裁剪：

**立体几何正确性**

- 静止时左右眼有合理 IPD 视差
- 交换左右眼立即产生明显错误
- 头部旋转不改变世界尺度
- 头部平移能探头，但不把 skybox 拉到近处
- render pose 与 submitted pose 一致

**内容分类**

- 3D world / skybox / 2D 菜单 / HUD / FMV / 全屏后处理 / render-to-texture
- BotW 特有：神庙内部、blood moon 特效、Sheikah Slate 界面、过场 letterbox

**pacing**

- 30fps 游戏在 72/90 Hz 头显上的表现
- 着色器编译与加载期间的 keep-alive
- session stop/restart、切游戏

**性能**

- per-eye vs multiview GPU time
- 立体模式相对平面模式的开销倍数（路线 A 预期接近 2×，路线 B/C 应显著低于）
- **热节流后的持续帧率**（移动端必测，PC 参考实现完全没有这个维度）

**BotW 专项回归**（来源：BetterVR README 的 Known Issues，是现成用例）

- 退出菜单后黑屏
- 爬梯子
- 游泳 / 骑马 / 蹲伏时的相机高度

---

## 8. 不做什么

- **不复活 Opcode Replay。** DolphinXR 已经删掉它并给出了完整论证
  （其文档 §5–§6）。command replay 只保留为 debug/研究能力。
- **不把「左右眼显示同一张纹理」或「按头动旋转一块 quad」描述成立体 VR。**
  这是 Flat 模式，有价值但不是 stereo。
- **不把 BotW 专属逻辑放进 foundation。**
- **不在 V0 通过前投入 V2+ 的工程。**
- **不在没有实测数据时承诺帧率。** 本文全篇未给出任何预测帧率数字，这是刻意的。

---

## 9. 一句话总结

BetterVR 证明了目标可达，DolphinXR 给出了移动端该怎么组织，
但两者都不能直接搬——**Wii U 的可编程着色器架构决定了立体必然带游戏专属成分，
而移动端的性能预算决定了不能照抄「游戏渲染两遍」**。
推荐路线 C（模拟器侧注入 + multiview + 最小引擎 hook），
但它成立与否取决于 V0 的三项实测，**在那之前不应启动任何 V2+ 的工程投入**。
