---
name: cemu-guest-runtime-debug
description: Safely debug a running emulator Guest through Cemu's PowerPC GDB Remote stub and spatial_debug_tool guest_debug MCP, including Android warmup, session-owned ADB forwarding, stop epochs and leases, threads/modules/register/memory capture, software breakpoints, and cleanup. Use for Wii U Guest runtime debugging, live PPC breakpoint validation, Guest thread inspection, or collecting device evidence without confusing Guest state with Android host native debugging.
---

# Cemu Guest 真机调试

通过 Guest PowerPC RSP 观察模拟器内的 Wii U 状态。Android LLDB、Java debugger、Host 线程
和 JIT code-cache 地址都不能替代这条链路。

## 先读取

- 阅读 `../../docs/architecture/guest-reverse-debug-mod-pipeline.md` 第 7、8、10、12 节。
- Android 同时使用 `../cemu-android-analysis/SKILL.md`、
  `../cemu-android-build-validation/SKILL.md`；性能采样再使用
  `../cemu-android-performance/SKILL.md`。
- 需要建立/复核 RPX 与 IDA 身份时先使用 `../cemu-guest-executable-ida/SKILL.md`。

## 1. 保护设备状态

1. 确认唯一目标设备序列号，备份 `settings.xml`、controller profile 和相关 graphic pack。
2. 使用 `RelWithDebInfo` 覆盖安装；不卸载、不清数据。
3. 执行 `open_last_game` 和默认 `warmup_a`。
4. 等待 `warmup_status=completed` 后截图；必须看到可操作 gameplay，不能只信状态字段。
5. 崩溃、菜单或控制器提示页都不算有效 gameplay 证据。

## 2. 准备 Cemu Guest GDB server

gameplay 调试在 warmup 后启动：

```sh
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService guest_debugger_start 1337
adb shell dumpsys activity service \
  info.cemu.cemu/.utils.DebugDumpService guest_debugger_status
```

要求 `prepared=true`、`listening=true`、`title_running=true`。入口断点才允许在标题启动前
prepare server，再 `open_last_game`。

标题运行期间不要调用 `guest_debugger_stop`；先断开客户端，标题退出后才销毁 server。

## 3. 使用 `guest_debug.*` MCP

优先使用 `spatial_debug_tool`，不要让裸 GDB 客户端长期占用设备。真实 Android 会话要求：

- `allowNativeDebugControl=true`；
- `allowPortForward=true`；
- 读取 Guest memory 时 `allowNativeMemoryRead=true`。

标准顺序：

1. `start_session`：指定 serial、remote port、`expectedArchitecture=powerpc:common`、
   `byteOrder=big`；先 dry-run，再真实连接；
2. 保存 session ID、revision 和 event cursor；
3. `control pause`，取得新的 `stopEpoch`；
4. 用该 epoch 调用 `list_threads`、`list_modules`、`read_registers`、`read_memory`；
5. `manage_breakpoints add` 只添加经过身份和地址证明的断点；
6. `control continue` 后用旧 cursor 调 `wait_for_event`，只接受更大的 `stopEpoch`；
7. 命中后先保存原始运行事实，再做静态解释；
8. 始终调用 `stop_session` 清理。

每次停止有有界 `stopLeaseMs`，默认 30 秒。复杂采样可提高到 300 秒，但不得取消 lease。
lease 到期自动恢复是安全行为，不是断点未命中。Guest 恢复后，旧 `stopEpoch` 只能作为历史
证据，不能继续读写或控制当前状态。

## 4. 每次命中必须采集

至少保存：

- title ID/version、RPX SHA、module checksum；
- stop reason、`stopEpoch`、event cursor；
- Guest PC、LR、CTR、r1、r3-r10；
- Guest thread ID/name、模块 text/data base；
- 断点附近原始 Guest bytes；
- 场景截图、warmup 参数、设置备份位置；
- 断点创建、命中、移除和恢复结果。

`list_threads` 返回 Cafe OS Guest OSThread；不要把它与 Android/Cemu Host 线程合并。RSP
寄存器同时保留 raw/interpreted 表达，PowerPC big-endian 结果要用内存字节或静态指令复核。

## 5. 无条件清理

正常、超时、工具异常或网络 EOF 都执行：

1. `stop_session`；
2. 确认 session `state=cleaned`、breakpoints 为空；
3. `adb forward --list` 确认只移除了本会话的精确 forward；禁止 `--remove-all`；
4. `guest_debugger_status` 确认 `connected=false`；
5. 截图确认 gameplay 已恢复；
6. 对比当前 `settings.xml` 与备份 SHA；
7. 只清理本轮创建的精确 `/data/local/tmp/<probe>`，不删除应用数据根。

如果断点原指令不能恢复、Guest 仍暂停、身份漂移或配置哈希变化，停止后续分析并先恢复状态。
