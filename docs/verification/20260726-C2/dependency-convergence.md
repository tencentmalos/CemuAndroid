# C2 后续依赖收敛验证

- 日期：2026-07-26
- 工作分支：`feature/malos/basic_version`
- 起点提交：`0374fc5d`（foundation 根 CMake 接入）
- 目标：参考 Azahar 和 `tencentmalos` 现有镜像，尽量减少 vcpkg 交叉构建面，
  同时保留 foundation 后续全部能力

## 范围与版本

统一入口为 `cmake/CemuBundledDependencies.cmake`。新增子模块及指针：

| 依赖 | 版本/指针 | 来源 |
| --- | --- | --- |
| fmt | 12.1.0 / `407c905e` | `tencentmalos/fmt` |
| glslang | 15.1.0 / `1062752a` | `tencentmalos/glslang` |
| zstd | 1.5.7 / `f8745da6` | `tencentmalos/zstd` |
| libusb | 1.0.26 / `4239bc3a` | `tencentmalos/libusb` |
| Crypto++ | `f6377f2f` | `tencentmalos/cryptopp`，沿用 Azahar 已验证指针 |
| Crypto++ CMake | `1efc3eb5` | `tencentmalos/cryptopp-cmake`，沿用 Azahar 已验证指针 |

RapidJSON 不再作为独立 vcpkg 包，由 Cemu 适配 target
`Cemu::rapidjson` 复用 `spatial::third_party_rapidjson`。Crypto++ target 会在
foundation 开启时注册，为后续 core/network/profiler/XR 消费准备；因为当前
debugbus 构建闭包没有链接它，所以不会无条件进入产物。

`vcpkg.json` 直接依赖由 33 项降为 27 项，移除 fmt、glslang、zstd、
libusb、RapidJSON 以及重复的 zstd 条目。没有向 vcpkg 添加 Crypto++。

## 暂留 vcpkg 的依赖

- Boost：当前 vcpkg 实际为 1.88；Azahar 的 `ext-boost` 镜像已到 1.90，且
  Cemu 使用 program_options、nowide、context、iostreams 等编译组件。版本升级
  和多平台 target 适配应单独验证。
- SDL3：Cemu 清单要求 3.4.10，`tencentmalos/SDL` 当前可核对的 release tag
  到 3.4.8，暂不降级。
- curl/OpenSSL、pugixml、libzip、zlib、libpng、glm、wxWidgets 及平台依赖：
  当前没有同时满足 `tencentmalos` 镜像、精确版本和 macOS/Android 验证的统一
  替代路径。

这些是后续继续收敛的候选，不是永久保留 vcpkg 的结论。迁移必须按依赖组独立
提交和验证，不通过删除 Cemu 或 foundation 功能来降低依赖数量。

## 构建验证

macOS：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -G Ninja
cmake --build build
```

结果：配置成功（`Configuring done` / `Generating done`），构建成功。链接仍有
既有 vcpkg WebP 静态库的 macOS deployment target warning，没有新增编译或
链接错误。

Android：

```sh
cd src/android
./gradlew assembleDebug
./gradlew assembleRelease
./gradlew testDebugUnitTest
```

结果：

- `assembleDebug`：`BUILD SUCCESSFUL in 1m`，41 个任务中 9 executed。
- `assembleRelease`：`BUILD SUCCESSFUL in 2m 26s`，56 个任务中
  27 executed；native、R8、lint 和 APK 打包通过。
- `testDebugUnitTest`：`BUILD SUCCESSFUL in 3s`，28 个任务中 5 executed。
- Debug APK：`app-debug.apk`，89,370,641 bytes，SHA-256
  `aa28adadea69740b7e6e4589b5e820fa88be1652ed73de73a860810cf1cdf544`。
- Release APK：`app-release.apk`，44,760,440 bytes，SHA-256
  `087d28a4fad64c8cbeeb96a3bb57a5d8c3ff18c0462384a8c26bd5e47c1d7031`。

Crypto++ 后续能力预检：

```sh
cmake --build build --target cryptopp
cmake --build src/android/app/.cxx/Debug/6r5u2b3g/arm64-v8a --target cryptopp
```

结果：macOS 生成 `libcryptopp.a`（170 条 Ninja edge），Android arm64-v8a
生成 `libcryptopp.a`（171 条 edge），两者均成功。普通 Cemu/macOS 和 Android
产品构建日志没有编译该 target，说明它已可供后续 foundation 消费，但仍被
排除在当前默认闭包外。

## 静态与子模块检查

提交前执行：

```sh
git diff --check
git submodule sync --recursive
git submodule status --recursive
git config -f .gitmodules --get-regexp '\.url$'
```

要求：

- 所有新增 URL 都是 `git@github.com:tencentmalos/...`。
- 新增子模块均钉在上表指针，状态无 `-` 或 `+`。
- `_out/`、`build_baseline/` 不进入提交。
- 当前分支仍是 `feature/malos/basic_version`，不修改或推送 `main`。
