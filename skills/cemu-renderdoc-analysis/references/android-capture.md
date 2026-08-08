# Android RenderDoc 抓帧参考

## 产物位置

默认依赖仓库：`~/workspace/renderdoc-for-pico`。

| 产物 | 路径 |
| --- | --- |
| macOS arm64 host library | `build-mcp-native/lib/librenderdoc.dylib` |
| Android arm64 server APK | `build-cemu-android-arm64/bin/com.picoxr.renderdoccmd.arm64.apk` |
| C# MCP server | `renderdoc-mcp-csharp/src/RenderDoc.Mcp.Server/bin/Release/net8.0/RenderDoc.Mcp.Server.dll` |
| Replay marker | `renderdoc-mcp-csharp/src/RenderDoc.ReplayMarker/bin/Release/net8.0/osx-arm64/native/RenderDoc.ReplayMarker.dylib` |
| 当前 ABI manifest | `renderdoc-mcp-csharp/abi/osx-arm64-pico.json` |

## 构建

Host 已配置时直接增量构建：

```sh
cmake --build ~/workspace/renderdoc-for-pico/build-mcp-native -j 10
```

首次配置 macOS host 可使用：

```sh
cmake -S ~/workspace/renderdoc-for-pico \
  -B ~/workspace/renderdoc-for-pico/build-mcp-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_QRENDERDOC=OFF -DENABLE_PYRENDERDOC=OFF \
  -DENABLE_GL=OFF -DENABLE_GLES=OFF -DENABLE_EGL=OFF \
  -DENABLE_METAL=OFF -DENABLE_WAYLAND=OFF
cmake --build ~/workspace/renderdoc-for-pico/build-mcp-native -j 10
```

Android server 已配置时：

```sh
cmake --build ~/workspace/renderdoc-for-pico/build-cemu-android-arm64 -j 10
```

首次配置使用本机 Android SDK/NDK 25.1、API 26、build-tools 35.0.1：

```sh
cmake -S ~/workspace/renderdoc-for-pico \
  -B ~/workspace/renderdoc-for-pico/build-cemu-android-arm64 -G Ninja \
  -DBUILD_ANDROID=ON -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DANDROID_BUILD_TOOLS_VERSION=35.0.1 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/Library/Android/sdk/ndk/25.1.8937393/build/cmake/android.toolchain.cmake" \
  -DENABLE_QRENDERDOC=OFF -DENABLE_PYRENDERDOC=OFF
cmake --build ~/workspace/renderdoc-for-pico/build-cemu-android-arm64 -j 10
```

Java 17+ 构建 server 时，RenderDoc 的 Java 编译必须使用 `javac --release 8`，不能再引用
已移除的 `jre/lib/rt.jar`。

C# MCP：

```sh
cd ~/workspace/renderdoc-for-pico/renderdoc-mcp-csharp
dotnet build RenderDoc.Mcp.sln -c Release
dotnet test RenderDoc.Mcp.sln -c Release --no-build
```

## 脚本机制

`scripts/capture_android_frame.py` 依次完成：

1. 验证设备在线、Cemu debuggable、host/APK 产物存在；
2. 快照 Android GPU debug global settings 与 RenderDoc properties；
3. 覆盖安装 RenderDoc arm64 server；
4. 通过 host C API attach device 并用 `adb://SERIAL` 控制 target；
5. 启动 Cemu、执行 `open_last_game` 和 `warmup_a`；
6. 保存抓帧前截图，按需等待人工确认；
7. 带 `--guest-frame-capture` 时，通过 `renderdoc_guest_capture` 在 Cemu Guest frame
   begin/end 间触发抓帧，等待 debugbus 报告 `completed`，再等待设备 RDC 的
   size/mtime 连续稳定；
8. 拉回本机，计算 SHA-256，写 manifest；
9. detach、恢复 settings/property、停止 server、普通方式重启 Cemu。

设备 RDC 默认位于：

```text
/sdcard/Android/media/info.cemu.cemu/files/RenderDocForPico/
```

## 验收与清理

成功条件：脚本 exit 0；截图为实际 gameplay；RDC 非零且稳定；manifest 的本机 size/hash
与文件一致。菜单帧是合法抓取结果，但不是性能/内部渲染目标结论的合法样本。

结束后复查：

```sh
adb -s SERIAL shell am force-stop com.picoxr.renderdoccmd.arm64
adb -s SERIAL shell settings get global enable_gpu_debug_layers
adb -s SERIAL shell settings get global gpu_debug_app
adb -s SERIAL shell settings get global gpu_debug_layers
adb -s SERIAL shell settings get global gpu_debug_layer_app
```

默认环境应恢复为原值；本项目当前基线均为 `null`。不要删除设备 RDC，remote replay 仍需
读取它；分析和证据归档完成后是否删除应由维护者另行决定。

## Guest 帧与 Android present

Cemu 的 `SwapBuffers`、Vulkan queue submit 和 Android `vkQueuePresentKHR` 可以解耦。
一次普通 present-delimited capture 可能只有一个 submit 片段，下一次可能只有 present。
因此 command translation、draw/pass/copy/submit 数量分析统一使用：

```sh
skills/cemu-renderdoc-analysis/scripts/capture_android_frame.py \
  --serial SERIAL \
  --guest-frame-capture \
  --warmup-args '10 15000 5000 250 60000' \
  --wait-for-gameplay-confirmation \
  --output-dir _out/renderdoc/CASE_NAME
```

开始点位于 `LattePerformanceMonitor_frameBegin()` 之后，结束点位于下一次 Guest
`SwapBuffers()` 之后。状态必须报告相邻的 `start_guest_frame` 与 `end_guest_frame`。
普通 `RENDERDOC_CaptureFrame`/`--capture-frame-count` 只保留给 WSI/present 边界专项。
