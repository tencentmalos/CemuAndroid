# BotW PM4 到 Vulkan 翻译 P1～P3 验证

## 结论

本轮已在 `feature/malos/pm4_optimize` 上完成 P1 表驱动 dirty routing、P2 低风险动态状态去重和
P3 pipeline transition cache 的首轮真机验证。画面与标题运行正确，未知寄存器分类为零，动态
状态与 descriptor 缓存指标符合预期；P3 在稳定 gameplay 中消除了约 `47.7%` 的两层 global
pipeline cache 查找。

P3 的结构收益已经成立，但性能结论仍需克制：单窗口 `draw_translate` 从约 `18.56 ms/帧` 下降
到 `18.14 ms/帧`，而 pipeline 子阶段没有同步下降。当前设备帧率会跳动，因此 P3 尚未达到 spec
要求的三个同场景窗口稳定收益门槛，不能把本轮约 `2.3%` 直接写成确定净收益。

## 验证环境

| 项目 | 值 |
| --- | --- |
| 分支 | `feature/malos/pm4_optimize` |
| 基线 | `feature/malos/basic_version` / `26b614d8` |
| 设备 | AYN Thor，serial `9c2841a4`，8 核，Adreno/Vulkan |
| APK | Gradle `relWithDebInfo`，`android:debuggable` |
| 游戏 | BotW JP v208，神庙内可操作 gameplay |
| 分辨率 | Guest source `1280x720`，output `1920x1080`，render/texture scale 均为 `1x` |
| 正确性窗口 | 90 秒；15 秒延迟后每 5 秒注入 A，共 6 次，再稳定 30 秒 |
| 正式 Tracy 窗口 | 30 秒，405 个带逐帧 counter 的 frame sample |

本地截图：`_out/pm4-optimize/cemu-pm4-p3-gameplay.png`。截图仅用于确认进入 gameplay，不称为
RenderDoc 抓帧，也不作为 GPU 性能证据。

## 构建与运行门禁

- `cd src/android && ./gradlew assembleRelWithDebInfo`：两次增量构建均通过，分别为 18 秒与 26 秒。
- `adb install -r app/build/outputs/apk/relWithDebInfo/app-relWithDebInfo.apk`：通过，未清数据。
- macOS `build_no_vcpkg`：受影响的 `CemuCafe` unity 6、7、32 三个 translation unit 编译通过。
- 旧 `build/` 目录仍缓存已删除的 vcpkg toolchain 路径，CMake regenerate 失败；这是本地旧 build
  tree 污染，不是当前源码依赖。验证改用 `build_no_vcpkg`，未删除用户本地 build tree。
- 标题 PID 在正确性与正式采样结束后仍为 `16518`；点测 PSS `2,370,550 KB`。
- 进程 logcat 中未发现 native fatal、device lost、KGSL/SMMU 或 GPU page fault。

## 采样有效性

第一次 90 秒连接发生在 native profiler 完全进入采集态之前，只得到 Tracy sampling thread 与
hardware samples，没有 application frame、CPU zone、GPU context 或 counter。artifact
`20260808-022424-989-tracy-tracy-live-normalized-only-b110debd` 判定为无效性能样本并丢弃，只保留
其 90 秒进程稳定性结果。

正式窗口在 gameplay 已稳定后重新连接，artifact 为
`20260808-022647-606-tracy-tracy-live-normalized-only-001798f9`：

- 405 帧；
- 3,993,828 个 CPU zone；
- 221,567 个 GPU zone，其中 221,197 个具有硬件 GPU timestamp；
- 11 条被识别线程，包括 LatteThread、PPCRecompiler 和 Guest CPU/GPU semantic lane；
- GPU duration 可用，但 Adreno context 没有 calibrated timestamp，CPU/GPU 绝对位置仍只能按
  CPU submit time 关联，不能把不同层级 inclusive duration 相加。

基线完整 trace 本地保存为
`_out/pm4-optimize/pm4-p1-p2-baseline.tracy`，P3 trace 保存为
`_out/pm4-optimize/pm4-p3-transition-cache.tracy`；它们不提交 Git，便于本机后续复查。

## P1：dirty routing

30 秒基线窗口中：

| Domain | classified words | marks | consumes |
| --- | ---: | ---: | ---: |
| Texture | 30,525 | 9,768 | 0 |
| Vertex buffer | 7,392 | 4,750 | 4,750 |
| VS uniform buffer | 927,747 | 907,282 | 900,770 |
| PS uniform buffer | 894,334 | 873,869 | 867,357 |
| GS uniform buffer | 358,260 | 352,447 | 350,412 |
| VS ALU constants | 358,974 | 90,354 | 90,354 |
| PS ALU constants | 0 | 0 | 0 |
| Unclassified | 0 | 0 | 0 |

Texture 当前仍由 sequence-break 消费，不使用与 buffer 相同的 consume counter，因此其 consume
为零是已知计数边界，不代表 texture dirty 丢失。Resource packet 跨多个 7-word descriptor 时会
标记所有相交 index；正式 gameplay 窗口的 unclassified 仍为零。

## P2：动态状态与 descriptor

正式 gameplay 的逐帧平均值：

| 指标 | requested/hash | emitted/hit | elided/miss | 解释 |
| --- | ---: | ---: | ---: | --- |
| Blend constants | 22.51 | 5.00 | 17.51 | 约 77.8% 重复提交被跳过 |
| Depth bias | 1,268.20 | 157.03 | 1,111.18 | 约 87.6% 重复提交被跳过 |
| Descriptor | 2,464.28 | 2,464.24 | 0.042 | 命中率约 99.998%，但 hash 仍执行 |

动态状态 cache 在 command-buffer reset 时失效，下一次使用会重新 emit。当前结果只证明重复
`vkCmdSet*` 被消除；descriptor 高命中率反而说明 P4 应重点减少不必要的 hash/lookup，而不是继续
优化 miss 路径。

## P3：pipeline transition cache

比较 P1/P2 基线和加入 256-entry direct-mapped transition cache 后的 30 秒窗口：

| 指标（逐帧平均） | P1/P2 基线 | P3 | 变化 |
| --- | ---: | ---: | ---: |
| Pipeline hash calls | 1,230.20 | 1,232.14 | 场景数量级一致 |
| Transition hit | 0 | 587.12 | 新增 L1 命中 |
| Global hit | 1,230.20 | 645.02 | `-47.6%` |
| Pipeline miss | 0 | 0 | 无新建或语义 fallback |
| Full-draw pipeline stage | 673.16 us | 692.44 us | 单窗口未改善 |
| Draw translate | 18,560.41 us | 18,137.58 us | `-2.3%`，待复验 |

transition entry 匹配 source pipeline、target vertex base hash、完整 target state hash 以及 shader/
fetch identity。冲突或不匹配回退原两层 global cache；`PipelineInfo` 注销时清理所有 source/target
entry，并清空可能悬空的 active pointer。P3 没有改 stable pipeline cache 格式。

## 当前判断与下一步

1. P1/P2 可以保留：指标覆盖完整，未观察到行为回归，且动态状态 emitted 显著小于 requested。
2. P3 可以留在实验分支继续复验：结构上将近一半 lookup 从两层哈希表转为一次 direct-map probe，
   但尚未证明 pipeline stage 或 frame time 的稳定改善。
3. 下一步先在完全相同的神庙视角再采两个 30 秒窗口，报告 frame median/P90/P99 与 pipeline stage；
   不要每次退回最原始版本，也不要用单点 StatusLayer FPS 归因。
4. P3 达标后再进入 P4：按 shader-stage dirty mask 跳过不相关 descriptor hash，并为 Host resource
   generation 建立完整 binding snapshot。P3 与 P4 不在同一提交中混合。
