# C1-T0 同步前 macOS 构建基线

- 日期：2026-07-26
- 分支：`feature/malos/basic_version`
- 提交：`55be5e2148c29287454b142317e7ac99f8bd8800`
- 结论：CMake 配置成功；Cemu 编译失败。该失败按实施 spec 作为同步前合法基线记录，本任务未修改源码。

## 环境

- macOS 26.5.2（Darwin 25.5.0，Apple Silicon arm64）
- Xcode 16.3，AppleClang 17.0.0
- Git 2.55.0
- CMake 4.4.0
- Ninja 1.13.2
- NASM 3.02
- Homebrew Boost 1.90.0_1
- MoltenVK 1.4.1 privateapi：
  `/opt/homebrew/lib/libMoltenVK.dylib`
  (`sha256: 6979d03daad0474786d7601c835db2d6830dc50b47df9fdbce61acbabae60487`)

MoltenVK 1.4.1 归档中的实际 dylib 路径是
`MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib`，与当前
`BUILD.md` 和实施 spec 给出的 `MoltenVK/lib/libMoltenVK.dylib` 不同。安装时使用
了归档中的实际路径。

## 配置

执行：

```sh
cmake -S . -B build_baseline -DCMAKE_BUILD_TYPE=RelWithDebInfo -G Ninja
```

结果：

- 成功，退出码 0。
- 首次 vcpkg 安装 105 个包，vcpkg 报告耗时 9.8 分钟。
- CMake 配置耗时 610.9 秒，生成耗时 0.2 秒。
- 成功找到 privateapi MoltenVK：
  `/opt/homebrew/lib/libMoltenVK.dylib`。
- 完整输出：`baseline-configure.log`。

## 编译

执行：

```sh
/usr/bin/time -p cmake --build build_baseline -- -d stats
```

结果：

- 失败，退出码 1。
- 生成的构建图共 570 条 Ninja build edges。
- 失败前启动 123 条、完成 122 条；`.ninja_log` 含 121 条构建记录和 1 行表头。
- wall time：`real 4.11`、`user 26.68`、`sys 8.23` 秒。
- 完整输出：`baseline-build.log`。

首个失败命令在 `[107/570]` 编译 `src/Common/cpu_features.cpp`：

```text
src/Common/cpu_features.cpp:121:2: error: no matching function for call to 'strncpy'
strncpy(m_cpuBrandName, cpuName.c_str(), sizeof(m_cpuBrandName) - 1);
```

Apple SDK 的 `strncpy` 需要可写的 `char*` 作为第一个参数，但当前
`m_cpuBrandName` 是 `std::string`。本任务只记录该同步前失败，没有修复。

## C1-T0 退出标准

- [x] 已记录配置结果。
- [x] 已记录编译结果和首个错误。
- [x] 已记录 build edges、Ninja stats 与 wall time。
- [x] 完整配置和编译输出已归档。
