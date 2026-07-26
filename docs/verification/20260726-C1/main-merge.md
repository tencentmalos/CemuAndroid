# C1 main 完整合流与分支主线确认

- 日期：2026-07-26
- Android 产品分支：`feature/malos/basic_version`
- 合并提交：`fc8845960b4928c9080514d96f9230220d87c810`
- 第一父提交：`2588c674a8efc0c7a1b9f73241062d5162f3d2a7`
- 第二父提交（main）：`b8f2cf4b431df7c1669ec926a5ea8b9fc146f310`
- 结论：官方 main 的完整历史已成为 `basic_version` 的祖先；后续以
  `basic_version` + 官方 main 为主线，`android-port` 仅作为可选 Android
  改动来源。

## 历史分流点

Git 拓扑核对结果：

- Android 历史最早从官方线分出于
  `8193ebf7d4c451ea412404711681f464b27a871f`
  (`Remove wxMessageBoxes from non-gui code`，2023-06-07)；其父提交
  `ae4cb45cf3f84f2e0a479aa116707b74ca480004` 仍在官方 main 历史中。
- 本轮前最后一次完整 main 合并是
  `0d6d0d471d93ecf7848f320ccb5edbcfc9e21ec1`（2026-04-19），第二父提交为
  `02383542b22d1e680d3e95d25f79e748a662d2e7`。
- 合并前 `main...basic_version` 为 `90 310`；合并后为 `0 311`。

合并前创建了本地安全分支
`backup/basic_version-pre-main-merge-20260726`，指向 `2588c674`。它没有推送，
仅用于本地回看。

## 合并与冲突处理

执行：

```sh
git switch feature/malos/basic_version
git merge --no-ff --no-commit main
```

共处理 21 个内容冲突。处理原则：

- CMake、输入、Android surface/JNI 和 soft keyboard 回调保留 Android 平台接入。
- Debugger、Latte shader/state hasher、下载窗口等采用 main 的新接口和实现。
- Vulkan 保留 Android 设备/纹理兼容逻辑，但跟随 main 删除已废弃的
  transform-feedback extension 路径，避免继续引用已移除的
  `FeatureControl.mode`。
- `.gitmodules` 保持全部 `git@github.com:tencentmalos/...` URL；
  `dependencies/Vulkan-Headers` 更新到 main 指针 `01393c3d`；
  `dependencies/foundation` 保持 `b01f41c`。

## 验证

执行并通过：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -G Ninja
cmake --build build -- -d stats
cd bin && ./Cemu_relwithdebinfo
cd src/android && ./gradlew assembleDebug
cd src/android && ./gradlew testDebugUnitTest
git submodule sync --recursive
git submodule update --init --recursive
git submodule status --recursive
```

实际结果：

- macOS CMake 配置成功。
- macOS 首轮构建在 Vulkan 旧 transform-feedback 接口处暴露语义冲突；修正后
  128 个增量目标全部成功并生成 arm64 `bin/Cemu_relwithdebinfo`。
- GUI smoke test 运行 8 秒仍存活，随后由验证命令主动结束。
- Android `assembleDebug`：`BUILD SUCCESSFUL in 47s`，含 arm64 Native 编译和
  APK 打包；APK SHA-256 为
  `51a5a7c65b567647f8e75b72c7d2a30b3af3effdd2f60948805d3b06488697c9`。
- Android `testDebugUnitTest`：`BUILD SUCCESSFUL in 8s`。
- `git ls-files -u` 为空；新增 diff 中无冲突标记。
- 所有递归子模块已初始化且无脏指针；全部顶层 URL 指向 tencentmalos 镜像。
- GitHub Actions workflow 可由 Ruby YAML 解析。

合并后拓扑验证：

```text
git merge-base --is-ancestor main feature/malos/basic_version = 0
git rev-list --left-right --count main...feature/malos/basic_version = 0 311
```

## 后续分支规则

1. 每个阶段边界先将本地 `main` 快进到 `upstream/main`，再推送
   `origin/main`。
2. 用 merge 把 `main` 汇入 `feature/malos/basic_version`，不 rebase 已推送历史。
3. `android-port` 不进入必经同步链；仅在逐项确认 Android 改动仍有价值后，
   选择性 cherry-pick 或显式 merge，并复跑 macOS 与 Android 验证。
