# C4 Android debugbus 单进程加固验证

- 日期：2026-07-26
- 分支：`feature/malos/basic_version`
- 设备：`2d6171b6`，2410DPN6CC，Android API 36

## 决策与实现

- D5：采用单 App 进程。删除 Release 的 `:EmulationProcess`；游戏退出调用
  `CafeSystem::ShutdownTitle()` 后只关闭 `EmulationActivity`，不结束 App。
- D6：Release 保留 debugbus。Service 不导出，增加按 application ID 展开的
  signature permission，并为 Service/JNI bridge 增加 R8 keep 规则。
- dumpsys binder 请求经 `DebugDumpRequestExecutor` Post 到 Android 主控制线程，
  超时为 2 秒；拒绝、超时、中断与异常均返回明确错误。
- `status` 输出真实 `title_running` / `title_paused`；`pause` / `resume`
  返回 succeeded、unchanged、unavailable 或 failed，不再只返回 requested。
- 未实现的 `screenshot` 从 help 摘除；JNI 统一使用 foundation
  `HandleDumpsysRequest()`。
- Service 在 Application 初始化时启动，并在 Activity started 时幂等恢复。
  Android 因空闲策略停止 started service 后，下一次 App Activity 启动即可恢复；
  这使它跟随 App，而不是某一局游戏的生命周期。

## 构建与单测

在 `src/android` 执行：

```sh
./gradlew testDebugUnitTest assembleDebug assembleRelease
```

结果：

```text
BUILD SUCCESSFUL in 19s
102 actionable tasks: 11 executed, 91 up-to-date
```

此前两轮增量复验（23 秒）和清理 native 产物后的完整 Debug/Release 构建也通过：

```text
BUILD SUCCESSFUL in 1m 11s
```

`DebugDumpRequestExecutorTest` 覆盖：

1. 已在目标线程时直接执行；
2. binder 侧请求 Post 到目标线程；
3. 目标线程拒绝请求；
4. 1 ms 可触发的超时路径；
5. handler 异常转换为明确错误。

## APK manifest 与 Release R8

用 Android SDK 36.0.0 `aapt2 dump xmltree` 分别检查 Debug/Release APK：

- 两个 APK 都只有默认 App 进程，没有 `android:process`；
- custom permission 分别展开为
  `info.cemu.cemu.debug.permission.DEBUG_DUMP` 和
  `info.cemu.cemu.permission.DEBUG_DUMP`；
- 两个 APK 都保留 `DebugDumpService`，`exported=false`，并声明对应 permission；
- Release APK 构建经过 minify/R8，真机 `help` 与 `status` 仍可进入 JNI，
  证明 Service 与 native bridge 没被裁掉。

## Debug 真机运行中 title

设备未安装正式游戏。为验证控制链路，使用公开的
PayloadLoaderInstaller v0.1.1 WUHB 作为临时 title，通过
`EmulationActivity` 的 `LaunchPath` 启动。

空闲状态：

```text
cemu_debug_status:
native_debugbus=true
emulator_hash=39a06b25
title_running=false
title_paused=false

pause unavailable: no title running
resume unavailable: no title running
```

临时 title 启动后连续执行 20 轮：

```sh
adb shell dumpsys activity service \
  info.cemu.cemu.debug/.utils.DebugDumpService pause
adb shell dumpsys activity service \
  info.cemu.cemu.debug/.utils.DebugDumpService status
adb shell dumpsys activity service \
  info.cemu.cemu.debug/.utils.DebugDumpService resume
adb shell dumpsys activity service \
  info.cemu.cemu.debug/.utils.DebugDumpService status
```

每轮实际结果均为：

```text
pause succeeded
title_running=true
title_paused=true
resume succeeded
title_running=true
title_paused=false
```

20 轮内未出现 dumpsys 超时、ANR、fatal 或状态错乱。`help` 只列出
`help`、`pause`、`resume`、`status`，不再列出空壳 `screenshot`。

## 生命周期与单进程

Debug 和 Release 运行时分别用 `pidof` 检查，均只有一个 App PID，没有
`:EmulationProcess`。Debug 退出临时 title 前后 PID 保持不变；退出后：

```text
title_running=false
title_paused=false
```

debugbus 仍可访问，说明游戏退出只结束 title，Service/registry 随 App 进程
保留。

## Release 真机与验证边界

Release APK 已成功安装。真机只出现一个 `info.cemu.cemu` PID，`help`、
`status`、空闲态 `pause` / `resume` 均可用，输出与 Debug 空闲态一致。

设备没有安装正式游戏；由 adb 写入 Release external-files 目录的临时 WUHB
不具备 App 自身选择文件时的访问条件，因此没有把“Release 运行中 title 的
pause/resume”标为已验证。Debug 的真实 title 控制链路已经覆盖共同 native
实现，Release 的 manifest/R8/JNI 已单独实测，但正式游戏运行态仍应在设备具备
游戏后补验，不能用上述结果推断替代。

临时验证文件在完成后从设备删除，不属于仓库或产品数据。
