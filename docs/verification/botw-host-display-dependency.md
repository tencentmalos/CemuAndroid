# BotW Host Display Dependency 真机验证

## 1. 验证结论

BotW JP v208 的 VSYNC counter fence 已能从 Guest RAM busy-poll 转换为 Host event
dependency。新路径保持了约 23 ms 的显示 pacing 等待，却把每次等待的条件检查从平均约
34.1 万次 Guest RAM 读取降为约 22.5 次 condition-variable wakeup；同 APK 的本轮顺序
A/B 中，FPS 提升 5.4%，Cemu 进程 CPU 下降 3.45 个百分点。

这证明 P1 的主要价值是减少 LatteThread 主动轮询，而不是消除帧 pacing。一次顺序 A/B
不能排除温度、频率和游戏局部场景漂移，因此 FPS 数字是当前观测值，不是长期收益承诺。

## 2. 环境与变量控制

| 项目 | 值 |
|---|---|
| 设备 | Pico B3110 / swan，8 核 |
| Android package | `info.cemu.cemu` |
| APK | Gradle `relWithDebInfo`，覆盖安装 |
| emulator hash | `ab6e8f2b` |
| 游戏 | BotW JP v208 / DLC 80 |
| module checksum | `0x6267BFD0` |
| render/static texture scale | 1.0 / 1.0 |
| 两组共有能力 | Structured Draw Fast Path 开启 |
| 唯一切换变量 | Host Display Dependency graphic pack |
| warmup | 延迟 15 秒，6 次 A、间隔 5 秒，稳定等待 60 秒 |
| 稳定采集 | 每组 35 秒 Tracy CPU + Vulkan GPU |

配置文件在实验前备份到
`_out/backups/20260807-host-display-dependency/settings.xml`。A/B 只移动独立 graphic pack，
实验结束后已恢复 pack，并用 `cmp` 确认设备上的 `settings.xml` 未变化。没有卸载应用或
清除数据。

## 3. 版本绑定与回退

Guest pack 位于 `tools/guest-mods/botw-v208-host-display-dependency/`，包含双重身份限制：

- `titleIds = 00050000101C9300`；
- `moduleMatches = 0x6267BFD0`；
- Host HLE 再检查前台标题为 BotW JP v208；
- counter 地址由 patch 注册，generic Host 代码不硬编码游戏地址。

Host 只在地址匹配、mask 为 `0xFFFFFFFF`、Guest compare op 为 `6`（GEQUAL）时生成
`IT_HLE_WAIT_DISPLAY_ORDINAL`。不匹配时仍生成原生 `WAIT_REG_MEM`；GX2 driver reset 会
关闭注册状态。

## 4. 生效证据

启用路径完成 warmup 后的 debugbus 状态：

```text
enabled=true
counter_phys_addr=0x1046d420
register_count=1
emitted=2442
consumed=2442
fallback=0
wait_count=2442
```

日志同时显示：

```text
Applying patch group 'BotW_JP_v208_HostDisplayDependency'
Display ordinal dependency enabled for BotW JP v208 counter 1046d420
```

`emitted == consumed` 证明每个新 PM4 packet 都被 LatteThread 消费；`fallback == 0` 证明
本场景目标 fence 形态完全匹配。warmup 的 `vpad_a_reads` 分别增长到 32 和 36，不能把仅
启动到菜单的阶段误当成 gameplay。

## 5. 同 APK A/B 数据

| 指标 | Legacy，session `s8` | Host event，session `s7` | 变化 |
|---|---:|---:|---:|
| frame 数 | 381 | 408 | +27 |
| 平均 FPS | 11.14 | 11.74 | +5.4% |
| 平均 frame time | 89.86 ms | 85.28 ms | -5.1% |
| Cemu CPU | 26.44% | 22.99% | -3.45 个百分点 / -13.0% |
| draw/帧 | 3967.88 | 3966.15 | -0.04% |
| pacing wait 平均值 | 23.94 ms | 23.08 ms | -3.6% |
| pacing wait p50 | 22.81 ms | 23.51 ms | +0.70 ms |
| 条件检查平均值 | 340570 RAM polls | 22.53 event wakeups | 约减少 15117 倍 |
| 条件立即满足 | 0 / 383 | 极少数 | 不影响主体结论 |

两组 draw/帧只差 0.04%，说明渲染负载接近。wait wall time 的平均值和 p50 没有同方向大幅
下降，符合“保留 pacing、只改变等待机制”的设计。FPS 与 CPU 同时改善，且新代码明确让
LatteThread 在 1 ms slice 内睡眠，因此当前证据支持 busy-poll 是可回收的 Host CPU 开销。

GPU 侧仍以约 3967 个 `vulkan.draw`/帧为主；该 P1 没有改变 draw 数、shader、resource
prepare、readback 或 GPU command stream，不能用它解释或解决全部 11～12 FPS 瓶颈。

## 6. Tracy 入口

ProfilerStudy 为两次 live Tracy 保存了 normalized artifact：

| 组 | session | artifact ID |
|---|---|---|
| Host event | `s7` | `20260807-040957-184-tracy-tracy-live-normalized-only-c0f13341` |
| Legacy | `s8` | `20260807-041445-483-tracy-tracy-live-normalized-only-017e08fb` |

artifact 根目录为 `~/.profilerstudy/traces/captures/`，每个 ID 下的 `normalized/` 可由
ProfilerStudy MCP 的 `load_trace_artifact` 重新载入。本轮 live normalized session 尝试
导出 `.tracy` 时触发 ProfilerStudy writer 的重复 source-location key 错误，因此没有把
失败的文件冒充为可复用原始 trace；查询和表格均来自已成功解码并持久化的 artifact。

## 7. 后续验证

- 进行至少三轮 Legacy/Host event 交错 A/B，并记录温度与 CPU/GPU 频率；
- 固定 save、镜头和输入 replay，避免局部场景漂移；
- 增加 Android sched/Perfetto 数据，直接比较 LatteThread on-CPU，而不只看进程 CPU；
- 运行至少 10 分钟，确认 ordinal 单调性、标题 reset、暂停/恢复和停止路径无 hang；
- P2 单独处理 readback/renderer completion batching，不能把它与本 P1 的 pacing 结果混合。
