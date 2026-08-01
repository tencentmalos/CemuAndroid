# BotW-BetterVR 立体渲染与 Hook 架构分析

> 记录时间：2026-07-26
> 本地仓库：`~/workspace/BotW-BetterVR`
> 分析基线：`27262df Improve DLL-less assembly patches for debugging`（v0.9.17）
> 上游项目：[Crementif/BotW-BetterVR](https://github.com/Crementif/BotW-BetterVR)
> 对照文档：`~/workspace/emulations/wii/dolphinxr/docs/DolphinXR-OpenXR-Stereo-and-Frame-Pacing.md`

## 1. 结论先行

BetterVR **不在模拟器图形管线里做立体转换**。它的立体来源是让**游戏自己把整帧渲染两遍**，
每遍用不同的眼睛相机。整个架构由三层组成，缺一不可：

| 层 | 载体 | 作用 |
|---|---|---|
| **PPC 汇编补丁** | Cemu graphic pack（`resources/BreathOfTheWild_BetterVR/*.asm`，7692 行） | 改 BotW 自己的代码：让 `procFrame` 跑两遍、过滤右眼的状态更新、把相机/投影查询重定向到 mod |
| **Cemu HLE 回调** | `osLib_registerHLEFunction("coreinit", "hook_*", ...)`，67 个 | 补丁里的 `bl import.coreinit.hook_X` 落到 mod 的 C++ 函数，拿到完整 `PPCInterpreter_t` 寄存器状态和 guest 内存 |
| **Vulkan layer** | `vkroots`，`VK_LAYER_CREMENTIF_bettervr` | 拦截 Cemu 的 Vulkan 调用，识别并截获两眼的 color/depth image，提交 OpenXR projection layer |

这与 DolphinXR 是**两条完全不同的技术路线**：

- DolphinXR 在**模拟器内部**拿到 guest 的 view-space position，用 OpenXR 的 per-eye
  projection 替换游戏原投影，靠 GS 复制或 Vulkan multiview 输出两眼。**游戏本身只跑一遍。**
- BetterVR 在 **guest 游戏代码内部**替换相机，让游戏引擎完整跑两遍。**模拟器不知道自己在做 VR。**

路线差异的根因是主机架构：GameCube/Wii 的 GX 是**固定功能 transform**，投影矩阵在 XF
寄存器里，模拟器天然能拿到；Wii U 的 Latte 用**可编程着色器**，投影矩阵藏在游戏自定义的
uniform buffer 里，没有通用拦截点。所以 Wii U 上做立体，要么逆向具体游戏的 uniform
布局，要么就像 BetterVR 这样直接改游戏代码。

## 2. 立体渲染的具体机制

权威描述在 `resources/BreathOfTheWild_BetterVR/patch_RND_StereoRendering.asm` 文件头注释里。
逐条拆解：

### 2.1 让 procFrame 跑两遍

```
custom_sead_GameFramework_procFrame 被改为把所有 render & update 步骤跑两次，
每只眼一次，并把 currentEyeSide 置为 0 或 1
```

`currentEyeSide` 是补丁在 codecave 里自己开的一个 `.int`，作为整套机制的全局状态。
另有 `currentFrameCounter`（0-1 计数器）支持两帧 in-flight。

### 2.2 过滤右眼的状态更新（最关键也最脆弱的部分）

第二遍必须**只重绘、不推进游戏状态**，否则物理、动画、AI 会以两倍速运行。补丁通过
以下方式过滤：

- `custom_GameScene_calcAndRunStateMachine`、`hook_sead_MethodTreeNode_callRec` 以及
  actor job 函数（`patch_RND_StereoRendering_ActorJobs.asm`）在 `currentEyeSide == 1`
  时跳过会改状态的调用。
- 但 `Actor::update()` 同时承担「更新状态」和「请求绘制 / 把自己加进
  `gsys::ModelSceneContext`」两个职责，过滤掉它会导致右眼没有 actor 可画。
- 解法是 `preventUnrequestingDraw` 和 `preventModelQueueClear`：不让游戏在右眼渲染开始时
  清空 model queue，从而复用左眼建立的绘制列表。

**这是整套设计里最需要逐游戏调试的地方**——「哪些调用是状态更新、哪些是绘制请求」
没有通用答案，只能靠对引擎的理解逐个甄别。

### 2.3 相机与投影替换

```
agl__lyr__Layer__getRenderCamera 和 agl__lyr__Layer__getRenderProjection 被 hook，
用于修改左右眼的 draw call。修改后的相机与投影矩阵由 OpenXR 数据与游戏原相机组合算出
```

对应 C++ 侧在 `src/hooking/camera.cpp`（1188 行，全仓最大的 hook 文件之一）：

- `hook_BeginCameraSide` / `hook_EndCameraSide` — 由 `hCPU->gpr[0]` 判定当前眼别
- `hook_GetRenderCamera` / `hook_GetRenderProjection` — 返回改造后的相机/投影
- `hook_OverwriteSeadPerspectiveProjectionSet` / `hook_ModifyProjectionUsingCamera`
- `hook_ModifyLightPrePassProjectionMatrix` — light pre-pass 需要单独处理
- `hook_CheckIfCameraCanSeePos` — 可见性剔除必须跟着改，否则视野边缘物体会被错误剔除

它直接操作游戏引擎的 `BESeadPerspectiveProjection`（Nintendo sead 引擎的大端投影结构），
把 OpenXR 的 `XrFovf` 与游戏原始 `fovYRadiansOrAngle`/`aspect` 融合
（`RenderUtils::ResolveGameProjectionFov`）。

**注意它做的是 FOV 融合而不是直接替换**——游戏的 UI 布局、特效缩放都依赖原 FOV 语义，
硬替换会破坏它们。

### 2.4 眼别识别：magic clear values

Vulkan layer 在 Cemu 外部，看不到 `currentEyeSide`。补丁用一个巧妙但很 hack 的办法传递：
`patch_RND_Find3DFrameBuffer.asm` 让游戏用**魔数清屏色**区分左右眼——

```
magic3DColorValue_leftSide:   .float 0.123456789
magic3DColorValue_rightSide:  .float 0.987654321
magic3DDepthValue_leftSide:   .float 0.0123456789
magic3DDepthValue_rightSide:  .float 0.163987654
```

layer 侧在 `src/hooking/framebuffer.cpp:87,324` 检查 clear value 匹配魔数及其顺序，
据此判断这张 image 属于哪只眼、是 3D 还是 2D buffer。同类补丁
`patch_RND_Find2DFrameBuffer.asm` 处理 UI 层。

**这是「模拟器与 mod 分离」强加的代价。** 如果 mod 编译在模拟器内部，直接读
`currentEyeSide` 即可，整个 magic-value 通道可以删掉。

### 2.5 UI / HUD 处理

BotW 把大部分 UI 画在 3D color buffer 之上。BetterVR 的做法（`patch_ImproveGUI.asm`）：

- 用 2D UI 的 **alpha 通道作为遮罩**，在 3D framebuffer 上叠一层透明 HUD
- `hook_FixUIBlending` 修正小地图、对话框等元素的 blend 设置——它们原本会覆写
  之前 2D 元素的 alpha，在 VR 里会形成图像空洞
- `patch_ImproveGUI.asm` 还提供 accessibility 配色（准星、按钮等对比度增强）

layer 侧只取**右眼**的 2D 输出给 ImGui overlay 用（`framebuffer.cpp:247-249`，
注释说右眼「看起来更好」）。

### 2.6 性能代价（作者自己的评估）

补丁注释里写得很直白：

```
Its not as optimized as it could be since Cemu has to translate the draw calls twice
which is usually the bottleneck for emulation, but it provides a great stable image
which can later be interpolated so that performance is less of an issue.
```

**游戏跑两遍 → Cemu 翻译两遍 draw call → CPU 侧接近两倍开销**，而 draw call 翻译正是
Cemu 的主要瓶颈。作者选择用「稳定的图像 + 后续插值」来对冲，而不是优化立体路径本身。

这条对移动端移植是**头号风险**：PC 上尚可用高单线程性能硬扛，Android 上不成立。

## 3. Hook 面的规模与版本绑定

| 指标 | 数值 |
|---|---|
| `osLib_registerHLEFunction` 注册数 | 67（`src/hooking/cemu_hooks.h`） |
| 补丁中不同的 `import.coreinit.hook_*` | 68 |
| PPC 汇编补丁总行数 | 7692（31 个 `.asm` 文件） |
| C++ hook 层代码 | ~7000 行（`src/hooking/`） |
| 渲染层代码 | ~3400 行（`src/rendering/`） |

**版本绑定极强：**

- 所有补丁统一 `moduleMatches = 0x6267BFD0`——BotW **update v208 的单一 RPX**
- `rules.txt` 限定三个 title ID（`00050000101C9300/9400/9500`）
- 补丁里大量硬编码 guest 地址（`0x024A6F20`、`0x10463EB0`、`0x031FB1B4` 等），
  换游戏版本全部失效
- README 明确「Only Cemu 2.6 is tested to work」

也就是说：**这不是一个可复用的 VR 框架，而是一份针对「BotW v208 + Cemu 2.6 + Windows」
的精确逆向工程成果。**

## 4. Hook 是怎么装上去的

### 4.1 Windows 侧（当前实现）

1. 用户把 `BetterVR_Launcher.exe` 放到 `Cemu.exe` 同目录
2. launcher 生成 Vulkan layer manifest（`resources/BetterVR_Layer.json` 模板，
   替换 `${BETTERVR_LAYER_DLL_PATH}` 等 token，见 `launcher/main.cpp:444-469`）
3. 通过 `enable_environment` 环境变量启用 layer，拉起 Cemu
4. layer DLL 加载进 Cemu 进程后，`CemuHooks` 构造函数用
   `GetModuleHandleA(NULL)` + `GetProcAddress` 取得三个 Cemu 导出符号：
   - `gameMeta_getTitleId` — 校验是不是 BotW
   - `memory_getBase` — 拿 guest 内存基址
   - `osLib_registerHLEFunction` — 注册 67 个 HLE 回调
5. graphic pack 的 `.asm` 补丁由 Cemu 自己应用到 RPX，补丁里的
   `bl import.coreinit.hook_X` 解析到第 4 步注册的函数

Cemu 侧的导出定义在 `src/Cafe/OS/common/OSCommon.cpp:105`：

```cpp
extern "C" DLLEXPORT void osLib_registerHLEFunction(
    const char* libraryName, const char* functionName,
    void(*osFunction)(PPCInterpreter_t* hCPU))
```

### 4.2 这套装载机制在 Android 上全部不适用

- 没有 Vulkan implicit layer 的外部注入机制
- 没有独立进程 + `GetProcAddress`
- 没有 launcher exe

但**这恰恰是移植时的简化而非障碍**：Android 上我们拥有 Cemu 源码，hook 模块可以直接
编译进 `CemuAndroid`，在进程内调用 `osLib_registerHLEFunction`，
`PPCInterpreter_t` 直接用 Cemu 自己的定义（不必像 `include/cemu.h` 那样手抄一份
ABI 兼容的镜像结构体——那份镜像本身就是版本脆弱点）。

## 5. 游戏玩法层（与立体渲染正交）

除了立体渲染，BetterVR 有大量纯玩法的 hook，构成「好的 VR 体验」而非「能看到 3D」：

| 模块 | 文件 | 内容 |
|---|---|---|
| 武器/手部 | `weapon.cpp` (496)、`skeleton.cpp` (456)、`patch_CTRL_HookWeaponHands.asm` | 把手柄 pose 绑到 Link 的骨骼，武器/火把/盾牌握在手里 |
| 弓箭 | `bow.cpp` (1428，全仓最大) | 用实体弓而非左右手控制器瞄准；瞄准弧线透明度可调 |
| 实体控制 | `entity_controller.cpp` (1057) | 手势装备/投掷、与世界互动解谜 |
| 相机/移动 | `camera.cpp` (1188)、`patch_CTRL_CameraControls.asm` | roomscale、第一人称、蹲伏/游泳/骑乘的高度偏移（硬编码常量） |
| 输入 | `controls.cpp` (1221)、`utils/controller_bindings.h` | VR 手柄 → Wii U GamePad 映射 |
| 震动 | `rumble.cpp` (23)、`patch_CTRL_Rumble.asm` | 触觉反馈 |
| 菜单 | `imgui_menus.cpp` (1327) | 游戏内 VR 设置菜单（长按右摇杆打开） |

注意 `camera.cpp` 里的写法：

```cpp
float hardcodedSwimOffset = 0.0f;
float hardcodedRidingOffset = 0.65f;
float hardcodedCrouchOffset = 0.3f;
```

**这种量级的「按游戏状态微调」是 VR 舒适度的实际来源，也是无法从任何框架里白拿的部分。**

## 6. 与 DolphinXR 的对比

| 维度 | DolphinXR | BotW-BetterVR |
|---|---|---|
| 立体来源 | 模拟器替换 per-eye projection | 游戏自己渲染两遍 |
| 游戏跑几遍 | 一遍 | **两遍** |
| 游戏耦合 | 基本通用（+ per-game 分类 DB） | **单游戏单版本** |
| 需要 guest 代码补丁 | 否 | **是（7692 行 PPC 汇编）** |
| 主机架构前提 | GX 固定功能 transform | 无（绕过了架构问题） |
| HUD/skybox 处理 | 模拟器侧 per-draw 分类 | 游戏侧改 blend + alpha 遮罩 |
| 剔除正确性 | 需模拟器侧处理 | **游戏引擎天然正确**（hook 了可见性检查） |
| 手部/武器实体 | 无 | **有**（游戏内真实骨骼绑定） |
| multiview 可用 | 是（Vulkan multiview） | 否（两遍是 guest 层面的，模拟器看到的是两个独立帧） |
| 移动端友好度 | 较好（single-pass + multiview） | **差（2× draw call 翻译）** |
| 装载方式 | 编译进模拟器 | 外部 Vulkan layer + DLL |

**两者不是互斥的。** 一个务实的组合是：用 DolphinXR 式的「模拟器侧 per-eye projection +
multiview」拿到低成本立体，用 BetterVR 式的「游戏侧 hook」拿到手部、武器、舒适度调整
这些引擎级体验——前提是能在 Wii U 的可编程着色器架构下找到通用的投影注入点，
而这正是最大的未知数。

## 7. 对 Cemu Android 移植的可借鉴性

### 7.1 可直接复用（高价值）

1. **7692 行 PPC 汇编补丁** — 与平台无关，是纯 guest 侧改动。理论上在 Android 上的
   Cemu 里同样能被 graphic pack 系统应用。**这是整个项目最值钱的资产**，它编码了对
   BotW 引擎（sead / agl / gsys）的全部逆向理解。
2. **67 个 hook 的语义与切点** — 即使重写实现，「该在哪里 hook、hook 到之后要做什么」
   的答案已经有了。
3. **`controller_bindings.h` + `controls.cpp`** — VR 手柄到 Wii U GamePad 的映射取舍。
4. **`camera.cpp` 的舒适度处理** — 蹲伏/游泳/骑乘偏移、roomscale 逻辑。
5. **README 的 Known Issues** — 现成的验收用例（黑屏、爬梯子卡住等）。

### 7.2 需要重做

1. **装载机制** — Vulkan layer + `GetProcAddress` → 直接编译进 `CemuAndroid`，
   进程内注册 hook。这是简化。
2. **magic clear value 眼别识别** — 有源码后直接读 `currentEyeSide`，删掉整条通道。
3. **`include/cemu.h` 的 ABI 镜像结构体** — 直接用 Cemu 自己的
   `PPCInterpreter_t` 定义。
4. **D3D12 路径**（`src/rendering/d3d12.cpp`，822 行）— Android 不需要。
5. **OpenXR 集成** — 应改走 foundation 的 `spatial::foundation_xr`，而不是移植
   `src/rendering/openxr.cpp`。

### 7.3 无法迁移

1. **性能模型** — PC 高单线程 + 串流 vs Android arm64 本机运行，完全不同。
   「2× draw call 翻译」在 PC 上作者认为可接受，在移动端需要实测才知道是否可行。
2. **Cemu 2.6 release 的二进制假设** — Android 侧是自己的 fork，导出符号、
   内存布局、graphic pack 行为都需重新验证。
3. **对 SteamVR/ALVR/Virtual Desktop 的依赖** — Android 本机 OpenXR runtime。

### 7.4 移植时必须先回答的问题

1. **Cemu Android 的 graphic pack 系统是否支持 `.asm` 补丁与 codecave？**
   `GraphicPack2Patches.cpp` 在核心代码里（跨平台），但需要实测 Android 上补丁的
   加载路径、`moduleMatches` 匹配和 codecave 分配是否正常。
2. **2× 渲染在目标头显上的实际帧率是多少？** 这决定整条路线是否成立。
   必须在做任何 VR 工作之前，先测出 BotW 在目标设备上的**平面模式**基准帧率——
   如果平面模式已经跑不到 30fps，立体模式没有讨论价值。
3. **是否存在只跑一遍的替代方案？** 即 DolphinXR 式的模拟器侧注入。在 Wii U 上
   需要找到 BotW 的投影矩阵在哪个 uniform buffer 的哪个偏移，然后用 Cemu 的
   shader patch 机制注入 per-eye 变换。这条路能省掉一半开销，但需要独立验证。

## 8. 源码索引

### Hook 层（`src/hooking/`）

- Cemu 接口与 67 个 hook 注册：`cemu_hooks.h`
- 相机与投影：`camera.cpp`
- Vulkan layer 入口（vkroots）：`layer.cpp`、`layer.h`
- 眼别识别与帧捕获：`framebuffer.cpp:87,222-249,324-394`
- 输入：`controls.cpp`
- 弓箭瞄准：`bow.cpp`
- 骨骼/手部：`skeleton.cpp`、`weapon.cpp`
- 游戏内菜单：`imgui_menus.cpp`
- OpenXR 运动桥接：`openxr_motion_bridge.h`

### 渲染层（`src/rendering/`）

- OpenXR session/layer：`openxr.cpp`、`openxr.h`
- 渲染器与 projection view：`renderer.cpp`、`renderer.h:220-239`
- swapchain：`swapchain.cpp`
- 纹理格式/拷贝：`texture.cpp`
- Vulkan 工具：`vulkan.cpp`、`utils/vulkan_utils.h`
- VR 内 ImGui：`vulkan_imgui.cpp`

### PPC 补丁（`resources/BreathOfTheWild_BetterVR/`）

- **立体渲染主逻辑（先读这个）**：`patch_RND_StereoRendering.asm`
- actor job 过滤：`patch_RND_StereoRendering_ActorJobs.asm`
- 相机与投影：`patch_RND_StereoRendering_CameraAndProjection.asm`
- 优化：`patch_RND_StereoRendering_Optimizations.asm`
- 帧缓冲识别：`patch_RND_Find3DFrameBuffer.asm`、`patch_RND_Find2DFrameBuffer.asm`
- UI：`patch_ImproveGUI.asm`
- 第一人称：`patch_FirstPersonMode.asm`、`patch_FirstPersonMode_Events.asm`
- 输入/骨骼/武器：`patch_CTRL_*.asm`
- 可见性修正：`patch_FixVisibilityChecks.asm`、`patch_CTRL_FixScreenChecks.asm`
- graphic pack 定义：`rules.txt`

### Cemu 侧接口

- `osLib_registerHLEFunction`：Cemu `src/Cafe/OS/common/OSCommon.cpp:105`
- graphic pack 补丁系统：Cemu `src/Cafe/GraphicPack/GraphicPack2Patches*.cpp`

## 9. 一句话总结

BetterVR 证明了「**在 Cemu 上做出高质量 BotW VR 是可行的**」，但它的实现方式
——游戏渲染两遍 + 7692 行单版本 PPC 补丁 + 外部 Vulkan layer——
其中只有**补丁的逆向知识**能低成本迁移到 Android，
**装载机制会变简单**，而**性能模型必须重新论证**。
