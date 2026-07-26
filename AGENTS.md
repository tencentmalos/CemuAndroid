# Repository Guidelines

## 项目结构与模块组织

本仓库是 Cemu 的 Android 移植版本。核心 C/C++ 代码在 `src/`：`src/Cafe` 负责 Wii U/Cafe OS 相关实现，`src/Common` 放通用基础设施，`src/audio`、`src/input`、`src/gui`、`src/util` 分别对应音频、输入、界面和工具逻辑。Android 工程位于 `src/android`，Kotlin/Compose 代码遵循 Android 标准目录，Native 入口在 `src/android/app/src/main/cpp`。资源和 game profile 在 `bin/`，打包材料在 `dist/`，第三方依赖和子模块在 `dependencies/`。

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
./gradlew assembleDebug
./gradlew testDebugUnitTest
./gradlew connectedDebugAndroidTest
```

ARM 目标可使用仓库脚本：

```sh
BUILD_TYPE=release ./build_arm.sh
```

更完整的构建、验证和故障排查流程放在 repo-local skills：`skills/cemu-android-build-validation/SKILL.md` 与 `skills/cemu-android-analysis/SKILL.md`。

## 编码风格与命名约定

C/C++ 遵循 `CODING_STYLE.md` 和 `.clang-format`。类成员使用 `m_` 前缀，静态成员使用 `s_` 前缀；变量使用 lower camel case，类和函数使用 UpperCamelCase。优先使用项目固定宽度类型，如 `uint32`、`sint64`，避免裸用 `int`、`long`。大括号单独成行，避免整文件自动格式化。Kotlin 遵循官方 Kotlin Coding Conventions，并贴近 `src/android/app` 中既有 Compose/Android 风格。

## 测试要求

Android 测试位于 `src/android/app/src/test` 和 `src/android/app/src/androidTest`。新增 Kotlin 单元测试使用 `FeatureNameTest` 这类清晰命名；需要设备或模拟器的测试放入 `androidTest`。Native/C++ 行为变更至少应通过相关 CMake 构建验证。涉及 Android UI、Gradle、资源、JNI 或启动路径时，优先执行对应 Gradle 任务；无法运行时在结果中说明原因和替代检查。

## 提交与 Pull Request 规范

提交标题保持短小、祈使句，必要时带作用域，例如 `android: fix input mapping`、`Update android dependencies`。每个提交只解决一个明确问题，避免混入格式化噪声、无关重构或意外子模块指针变化。PR 描述应说明目的、主要改动、验证命令和影响范围；涉及 UI 或平台行为时附截图、日志或关键输出。当前 Android 移植仍属早期实验阶段，提交前优先确认维护者是否接受相关方向。

## Agent 专用约束

默认串行执行 agent 工作，除非用户明确要求并发或多 agent 拆分。修改前先阅读相关模块、脚本和历史提交；实现时优先沿用现有架构与命名。不要直接修改 `dependencies/` 内子模块内容，除非任务明确要求并且变更会提交到对应子仓。

本分支子模块 URL 应指向 `git@github.com:tencentmalos/...`。改动 `.gitmodules` 后必须运行 `git submodule sync --recursive`，并用 `git submodule status --recursive` 检查指针。涉及签名配置时使用环境变量 `ANDROID_STORE_FILE`、`ANDROID_KEY_STORE_PASSWORD`、`ANDROID_KEY_ALIAS`，不得提交密钥或本地配置。

新增或沉淀本项目专用工作流时，优先放入 `skills/` 下的 repo-local skill，并在本文件中加入入口说明。文档、诊断和验证输出应足够具体，能让下一位维护者复现命令和判断依据。
