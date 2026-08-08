# Repository Guidelines

## 项目结构与模块组织

本仓库是 Cemu 的 Android 移植版本。核心 C/C++ 代码在 `src/`：`src/Cafe` 负责 Wii U/Cafe OS 相关实现，`src/Common` 放通用基础设施，`src/audio`、`src/input`、`src/gui`、`src/util` 分别对应音频、输入、界面和工具逻辑。Android 工程位于 `src/android`，Kotlin/Compose 代码遵循 Android 标准目录，Native 入口在 `src/android/app/src/main/cpp`。资源和 game profile 在 `bin/`，打包材料在 `dist/`，第三方依赖和子模块在 `dependencies/`；只读实现参考放在 `references/`。

## Reference 仓库与文档

- `references/BotW-BetterVR`：BotW v208 的 VR graphic-pack、PPC patch、Cemu HLE hook、输入与 Vulkan/OpenXR 实现参考。
- `references/botw`：BotW C/C++ 逆向结果，供符号、类型和引擎语义参考；其目标是 **Switch v1.5.0**，不能直接套用到 Wii U v208 的地址、ABI、结构偏移或指令。
- `references/decaf-emu`：Wii U Latte/PM4 packet、寄存器和 Vulkan 翻译的只读语义参考。它用于交叉验证语义，不作为现代 Host 性能实现模板；不得直接修改子模块或复制 GPL 实现。
- `games/reverse/botw/wiiu-v208`：绑定 JP v208 RPX SHA/module checksum 的 Wii U identity manifest、运行时符号证据与逆向入口；最终 `.i64` 位于 private 子模块 `ida-database`，子模块内由 Git LFS 管理。原始 RPX 不提交，其他版本不得复用其中硬编码地址。
- `docs/bettervr/README.md`：上述仓库、BetterVR 文档快照、Android 迁移分析和真机验证证据的统一入口。

PM4/Vulkan 优化还可只读对照本机同源仓库：`/Users/bytedance/workspace/emulations/switch/citron` 用于表驱动 dirty-state、pipeline key/transition cache，`/Users/bytedance/workspace/emulations/3ds/azahar` 用于 Android/Adreno Vulkan 动态状态与 scheduler，`/Users/bytedance/workspace/emulations/ps4/shadps4` 用于现代 AMD PM4 翻译边界。三者都不是 Latte 语义来源；工作树可能承载其他任务，不得修改或清理。完整边界与实施阶段见 `docs/plans/pm4-vulkan-translation-optimization-spec.md`。

`references/` 默认只读。需要吸收实现时，在 Cemu 主仓中重写并验证；如确需修改参考仓库，应在对应子仓单独提交，不得把 detached HEAD 中的未提交改动留作依赖。BetterVR 应按 Guest patch、Host/HLE、renderer/XR 三层分别审计，不能用“补丁已应用”替代运行时功能验证。

## 构建、测试与开发命令

常规依赖同步与桌面构建：

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=release -G Ninja
cmake --build build
```

Android 构建与测试：

```sh
cd src/android
./gradlew assembleRelWithDebInfo
./gradlew testDebugUnitTest
./gradlew connectedDebugAndroidTest
```

Android 可运行 APK、真机功能验证和性能验证统一使用 Gradle `relWithDebInfo`
variant，安装使用 `adb install -r app/build/outputs/apk/relWithDebInfo/app-relWithDebInfo.apk`。该 variant 使用正式包名、允许
debug/run-as、不启用 R8，并由 variant 名称保证 Native CMake 构建类型是
`RelWithDebInfo`。`debug` 仅用于当前只提供 debug variant 的单元测试和
instrumentation test，不得作为运行性能基线。

ARM 目标可使用仓库脚本：

```sh
BUILD_TYPE=release ./build_arm.sh
```

更完整的构建、验证和故障排查流程放在 repo-local skills：`skills/cemu-android-build-validation/SKILL.md` 与 `skills/cemu-android-analysis/SKILL.md`。CPU/GPU Tracy 采集、FPS 概要面板和跨平台 debugbus 验证使用 `skills/cemu-android-performance/SKILL.md`；Android Vulkan 的 gameplay 抓帧、RenderDoc 远程回放和结构化图形分析使用 `skills/cemu-renderdoc-analysis/SKILL.md`。Wii U 标题版本检查、汉化资源烘焙和 WUA 打包使用 `skills/cemu-wua-packaging/SKILL.md`。Guest 游戏逆向与 Mod 使用 `skills/cemu-guest-game-patching/SKILL.md`；运行中 RPX 提取/IDA 建库、Guest 真机断点、IDA 静态/动态联合校准分别使用 `skills/cemu-guest-executable-ida/SKILL.md`、`skills/cemu-guest-runtime-debug/SKILL.md`、`skills/cemu-guest-ida-correlation/SKILL.md`。

## 编码风格与命名约定

C/C++ 遵循 `CODING_STYLE.md` 和 `.clang-format`。类成员使用 `m_` 前缀，静态成员使用 `s_` 前缀；变量使用 lower camel case，类和函数使用 UpperCamelCase。优先使用项目固定宽度类型，如 `uint32`、`sint64`，避免裸用 `int`、`long`。大括号单独成行，避免整文件自动格式化。Kotlin 遵循官方 Kotlin Coding Conventions，并贴近 `src/android/app` 中既有 Compose/Android 风格。

## 测试要求

Android 测试位于 `src/android/app/src/test` 和 `src/android/app/src/androidTest`。新增 Kotlin 单元测试使用 `FeatureNameTest` 这类清晰命名；需要设备或模拟器的测试放入 `androidTest`。Native/C++ 行为变更至少应通过相关 CMake 构建验证。涉及 Android UI、Gradle、资源、JNI 或启动路径时，优先执行对应 Gradle 任务；无法运行时在结果中说明原因和替代检查。

## 提交与 Pull Request 规范

提交标题保持短小、祈使句，必要时带作用域，例如 `android: fix input mapping`、`Update android dependencies`。每个提交只解决一个明确问题，避免混入格式化噪声、无关重构或意外子模块指针变化。PR 描述应说明目的、主要改动、验证命令和影响范围；涉及 UI 或平台行为时附截图、日志或关键输出。当前 Android 移植仍属早期实验阶段，提交前优先确认维护者是否接受相关方向。

## Agent 专用约束

默认串行执行 agent 工作，除非用户明确要求并发或多 agent 拆分。修改前先阅读相关模块、脚本和历史提交；实现时优先沿用现有架构与命名。不要直接修改 `dependencies/` 或 `references/` 内子模块内容，除非任务明确要求并且变更会提交到对应子仓。

`feature/malos/basic_version` 是本仓库的 Android 产品主线，后续以官方 Cemu `main` 为主要跟踪来源：本地 `main` 先与 `upstream/main` 对齐并推送到 `origin/main`，再用 merge 汇入 `feature/malos/basic_version`，不得 rebase 已推送历史。`android-port` 只作为 Android 旧实现的可选来源；仅在逐项确认改动仍有价值后选择性 cherry-pick 或合并，不得让它成为官方 main 同步的前置或并行主线。

foundation 是 Cemu 的跨平台硬依赖，不得重新增加启用宏、CMake 关闭选项或无 Foundation fallback。foundation 的 core、network、profiler、ImGui、XR 等组件属于后续计划能力，即使当前未被产品 target 链接，也不得仅以“暂时未使用”为由删除。Cemu 的 ImGui core 只由 `spatial::foundation_imgui` 提供，不得恢复 `dependencies/imgui`；renderer status 与未来 XR 必须复用 foundation 的 layer、共享 font atlas 和 backend 生命周期，不得再维护平台专用的第二套 UI 栈。依赖收敛优先复用 Azahar 已验证或 `tencentmalos` 已镜像的子模块，并集中放在 CMake 依赖适配层；不得重新引入 vcpkg，也不得通过裁剪功能来减少依赖数量。

本分支子模块 URL 应指向 `git@github.com:tencentmalos/...`。改动 `.gitmodules` 后必须运行 `git submodule sync --recursive`，并用 `git submodule status --recursive` 检查指针。涉及签名配置时使用环境变量 `ANDROID_STORE_FILE`、`ANDROID_KEY_STORE_PASSWORD`、`ANDROID_KEY_ALIAS`，不得提交密钥或本地配置。

Android 设备 Root 操作统一使用 `~/workspace/devices` 中的设备适配框架，不得在本仓直接执行裸 `adb shell xsu` 或复制厂商后端。AYANEO Pocket DS 操作前先完整阅读 `~/workspace/devices/skills/ayaneo-root/SKILL.md`，并从 devices 仓库执行 `skills/ayaneo-root/scripts/root.sh probe`；新连接或固件变化时再用 `skills/ayaneo-root/scripts/root.sh exec -- id` 验证 Root 上下文，实际命令统一走 `skills/ayaneo-root/scripts/root.sh exec -- "<逐行审核过的命令>"`，多设备在线时必须传 `--serial SERIAL`。每次 Root 操作都要重新 `probe`，确认 manufacturer、model、build、adapter 与 Bootloader 状态匹配目标设备；已验证的 Pocket DS Root 上下文为 `uid=0(root)`、`u:r:xsud:s0`。适配器负责把命令放入 `/data/local/tmp`、通过 `/product/bin/xsu` 执行并清理 Host/设备临时文件，不得绕过该流程。除非用户明确授权已通过只读检查解析出的精确破坏性目标，否则不得传 `--allow-destructive`；不得把网络下载内容直接送入 Root shell，也不得为调试刷写 `boot`、`init_boot`、`vendor_boot`、`vbmeta`、ABL 或修改 Bootloader。完整设备评估位于 `~/workspace/devices/ayaneo/docs/AYANEO_Pocket_DS_Root_Assessment.md`；不支持的新设备按 `~/workspace/devices/docs/ADDING_DEVICE_SUPPORT.md` 新增 Adapter 和专用 Skill，禁止套用其他机型的 Root 方法。

新增或沉淀本项目专用工作流时，优先放入 `skills/` 下的 repo-local skill，并在本文件中加入入口说明。文档、诊断和验证输出应足够具体，能让下一位维护者复现命令和判断依据。
