# C2 foundation 根 CMake 按需接入验证

- 日期：2026-07-26
- 工作分支：`feature/malos/basic_version`
- 验证前源码提交：`1e9c4ee632f9d566def70a3adeea77965d100d6a`
- foundation 指针：`b01f41c38e1c6a63629c55ac4b03490bb73a6e70`
- foundation API 版本：`1`
- 结论：原有伪造 target 和 debugbus 子目录直连已移除；foundation 根 CMake
  以 `EXCLUDE_FROM_ALL` 注册真实 target；macOS 和 Android 构建通过；子模块
  缺失与 API 版本护栏均通过负向验证；Cemu 的 `vcpkg.json` 无改动。

## 接入取舍

对照 Azahar 的 `feature/malos/build_speedup` 分支后确认：

- Azahar 先建立自己的 externals 层，再接入 foundation；其中 Crypto++ 是
  Azahar 已有的 vendored external，不代表所有 foundation 宿主都应把它加入
  自己的包管理清单。
- Cemu 当前实际消费的只有
  `spatial::foundation_debugbus` 和 Android 下的
  `spatial::foundation_debugbus_dumpsys`。这两个 target 不依赖 foundation
  core、network、profiler、XR 或 Crypto++。
- 根 CMake 使用 `EXCLUDE_FROM_ALL` 后仍会注册完整的真实 target 集合，但默认
  只构建宿主明确链接的依赖闭包。这既消除了伪造 target，也避免为尚未使用的
  组件提前穿插 vcpkg、第三方库和宿主兼容补丁。

曾用不带 `EXCLUDE_FROM_ALL` 的全量默认构建做过诊断：macOS 在补宿主兼容后
达到 912 条 Ninja 命令、`real 93.20s`；Android 随后在未使用的
foundation core 上暴露 NDK 29 编译问题。该路径还要求为未使用组件补
Crypto++ 和预编译头兼容，违背本阶段的内聚目标，因此全部撤销，未进入最终
改动。

最终改动原则：

1. Cemu 根 CMake 只负责启用/禁用、子模块缺失检查和 foundation 根目录注册。
2. Android target 只声明当前真正消费的 debugbus/dumpsys 和 API 版本头。
3. 后续 core/network/profiler/XR 有真实消费者时，再由对应 foundation target
   自洽带入依赖；若依赖闭包过大，推动 foundation 自身组件化。
4. 不在 Cemu 侧伪造 target；该根接入提交不向 vcpkg 添加未使用组件的依赖。

## 环境

```text
macOS 26.5.2 arm64
CMake 4.4.0
Ninja 1.13.2
Apple clang 17.0.0
Gradle 9.3.1
JDK 17.0.12
Android NDK 29.0.14206865
```

## macOS 配置、干净构建与启动

执行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -G Ninja
cmake --build build --target clean
/usr/bin/time -p cmake --build build -- -d stats
```

结果：

- CMake 配置成功，foundation 全部真实 target 可在配置阶段解析。
- 干净构建成功，`FinishCommand = 559`。
- 计时：`real 62.86s`、`user 657.94s`、`sys 58.74s`。
- 产物：`bin/Cemu_relwithdebinfo`，32,706,512 bytes。
- SHA-256：
  `c127763ddb7af6d59350fac9d056dddce601f44db80cd1e25d4908672de0da04`。
- 链接器仍报告部分缓存 vcpkg 静态库以 macOS 15.4 构建、目标为 13.4 的已有
  warning；没有 foundation 新增编译或链接错误。

启动 smoke test：

```sh
./bin/Cemu_relwithdebinfo
```

进程保持运行 8 秒，随后由验证脚本主动终止，结果：

```text
Cemu stayed alive for 8 seconds
```

### 与 C2 前基线对比

| 指标 | C2 前 | C2 最终 | 变化 |
| --- | ---: | ---: | ---: |
| Ninja `FinishCommand` | 558 | 559 | +1 |
| wall time | 75.88s | 62.86s | -13.02s（仅参考） |
| user time | 693.35s | 657.94s | -35.41s |
| 产物大小 | 32,706,512 bytes | 32,706,512 bytes | 0 |

两次 clean build 的机器负载不同，wall time 不用于宣称加速；关键结论是默认
构建仅增加 1 条命令、产物大小不变，未使用 foundation 组件没有进入 macOS
默认构建图。C2 最终 559 条命令是 C3 的起点基线。

## Android Debug、Release 与单测

执行：

```sh
cd src/android
./gradlew assembleDebug
./gradlew assembleRelease
./gradlew testDebugUnitTest
```

结果：

- `assembleDebug`：护栏恢复后的首次检查为 `BUILD SUCCESSFUL in 4s`；提交前
  强制重新配置后的最终检查为 `BUILD SUCCESSFUL in 40s`，41 个任务中
  6 executed，native CMake 配置和构建通过。
- `assembleRelease`：`BUILD SUCCESSFUL in 6m 3s`，56 个任务中 55 executed；
  arm64-v8a RelWithDebInfo native 编译、R8、lint 和 APK 打包全部通过。
- `testDebugUnitTest`：`BUILD SUCCESSFUL in 3s`，28 个任务中 4 executed。
- Debug APK：103,129,682 bytes，SHA-256
  `3df6f4dcc7602eef49f014bc41f0d1729181347690bc371db5c2255191064323`。
- Release APK：45,131,372 bytes，SHA-256
  `701c0238695201bf3e41d767d8fc042a8b0245fecb5d42e884907c852bf9ffed`。

Release native 编译输出含 Cemu 和既有第三方库的弃用/类型转换 warning；没有
foundation 新增错误。

## 子模块缺失负向测试

验证时将 `dependencies/foundation` 精确重命名为临时目录，并用 shell trap
保证恢复，然后重新配置。

预期且实际得到：

```text
foundation submodule missing, run: git submodule update --init --recursive
```

CMake 以非零状态退出。恢复目录后执行：

```sh
git submodule status --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -G Ninja
```

foundation 恢复为 `b01f41c38e1c6a63629c55ac4b03490bb73a6e70`，重新配置
成功。说明开启 foundation 时不会因子模块未初始化而静默降级。

## API 版本负向测试

将 `NativeDebugDump.cpp` 的断言临时改为：

```cpp
static_assert(SPATIAL_FOUNDATION_API_VERSION >= 999, "Unsupported spatial foundation API version");
```

执行 `./gradlew assembleDebug`，预期且实际编译失败：

```text
static assertion failed due to requirement '1 >= 999':
Unsupported spatial foundation API version
```

随后恢复阈值 `>= 1` 并重新执行 `assembleDebug`，构建成功。最终工作树不存在
测试用的 `>= 999`。

## 静态检查与退出标准

执行并通过：

```sh
git diff --check
git diff -- vcpkg.json
git submodule status --recursive
```

`vcpkg.json` diff 为空；foundation 子模块无指针或内部内容改动。

- [x] 根 CMake 注册 foundation，删除伪造 target。
- [x] 未使用组件不进入默认构建，根接入提交不向 vcpkg 新增 Crypto++。
- [x] macOS 配置、clean build 和 GUI smoke test 通过。
- [x] Android Debug、Release 和单测通过。
- [x] 子模块缺失明确失败，恢复后重新配置通过。
- [x] API 版本不兼容明确失败，恢复后重新构建通过。
- [x] C2 前后 edges、耗时和产物大小已记录。

## 后续决策

本文记录的是提交 `0374fc5d` 之前的 C2 根接入验证，因此“`vcpkg.json` 无改动”
和“未新增 Crypto++”描述的是该提交的历史状态。维护者随后确认 foundation 的
core、network、profiler、XR 都属于后续范围，并决定复用 Azahar /
`tencentmalos` 现有依赖、尽量收敛 vcpkg。后续实现与重新验证见
`dependency-convergence.md`；没有删除本文已验证的 foundation target 或能力。
