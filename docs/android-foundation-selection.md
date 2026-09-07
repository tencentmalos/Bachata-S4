# Android 开发基线选择与版本核实

**整合能力结论与接口核查：见 [Android 与 ARM64 C++ 整合审计](android-arm64-integration-audit.md)。已确认真实启动/HLE/控制/输入/音频基础；NDK 原生 Surface 与 16 KiB 仍是必做工作。**

核查日期：2026-09-07。目标：为后续 Android 16 / 16 KiB、原生 shadPS4 + FEXCore 开发选定可取得源码的 Android 前端基础。

## 1. 选定结果

**采用 Bachata S4 的 0.1.8 源码系列，固定提交 `67dbf4e5b54b0467bf8a93de3de0a557a71f9f43`，作为 Android 前端开发基线。** 它包含完整 Android 工程、runtime glue 和 FEX 后端参考；当前公开 fork 保留了该提交。

已经在本仓建立独立子模块：

- 路径：[references/Bachata-S4-android](https://github.com/zFitness/Bachata-S4/tree/67dbf4e5b54b0467bf8a93de3de0a557a71f9f43)
- Android 工程：[android/BachataS4](https://github.com/zFitness/Bachata-S4/tree/67dbf4e5b54b0467bf8a93de3de0a557a71f9f43/android/BachataS4)
- 本地开发分支：`codex/android-foundation`
- 下载来源：`https://github.com/zFitness/Bachata-S4.git`
- 精确版本清单：[android-foundation.lock.json](data/android-foundation.lock.json)

这是**源码与架构基线**，尚不是经过本次构建和设备验收的 Android 16 KiB 可运行基线。当前主仓尚无 `android/` 工程；下一步可以从该子仓提取 Android 层接入主仓的原生 backend。现有 `references/Bachata-S4` 的 SG8275 分支及两个未提交修改保留原状。

## 2. 本地 0.1.0 与远端 0.2.1，到底指什么

这几个版本号属于不同对象：

| 对象 | 本次实际查到的值 | 含义 |
|---|---|---|
| 当前主仓 shadPS4 | `0.18.1 WIP`，HEAD `79b7ddff` | 桌面模拟器核心版本，不是 Android app 版本 |
| 现有本地 Bachata 源码 | `VERSION_NAME=0.1.7`，HEAD `e170f800` | Android app 默认正式版本配置 |
| Gradle 缺少版本配置时 | `0.1.0-dev-<日期>` | 开发构建 fallback，不等于源码停留在 0.1.0 |
| Android scaffold README | `create-android@0.1.0` | 工程模板版本 |
| JICA98/Bachata-S4-Runtimes | `v0.1.0` | 单独发布的旧 managed-runtime 资源包版本 |
| 官方 Bachata 最新预发布 | `v0.2.1` | 2026-09-06 发布的预发布条目；查询时没有上传 APK |
| 官方带 APK 的最新非预发布 | `v0.1.9` | 有约 173.6 MB APK，但其源码链接与 release notes 有不一致 |
| 本次选定源码 | 配置仍为 `0.1.8`，commit `67dbf4e5` | 完整 Android 工程，版本按代码事实命名 |

本地版本证据：[主仓 CMake](../CMakeLists.txt#L215)、[旧 Bachata version.properties](https://github.com/tencentmalos/Bachata-S4/blob/e170f8005970ae416e0f39997d82b4fcc57214fe/android/BachataS4/version.properties#L9)、[旧 Gradle 版本优先级](https://github.com/tencentmalos/Bachata-S4/blob/e170f8005970ae416e0f39997d82b4fcc57214fe/android/BachataS4/app/build.gradle.kts#L49)。CLI `-PVERSION_NAME` 可以覆盖上述值；本次没有读取已安装设备 APK，因此不把源码配置当成设备已安装版本的证明。

远端证据：[v0.2.1 预发布](https://github.com/JICA98/Bachata-S4/releases/tag/v0.2.1)、[v0.1.9 发布](https://github.com/JICA98/Bachata-S4/releases/tag/v0.1.9)、[旧 runtime 发布](https://github.com/JICA98/Bachata-S4-Runtimes/releases/tag/v0.1.0)。v0.2.1 的 GitHub API `assets` 数组为空；网页可能显示两个自动生成的 source archives，它们不是 APK，也不代表包含 Android 工程。

## 3. 为什么不直接 checkout v0.2.1

当前 `JICA98/Bachata-S4` 是发布/兼容性资料仓库，`v0.2.1` 指向 `c9263f39394a16bbb9705db5d06faf8cd61ad6e4`。实际根目录只有 `.github`、文档、兼容性数据和 `release-metadata`，没有 `android/`、Gradle 或完整 emulator source。

其 README 指向的 `zenithblue-oss/shadps4-arm64`，目前提交为 `be6bc2e9c60799e071dd2fafa6216e8d80ec619c`。它有 ARM64/FEX 核心和 runtime 相关内容，**也没有 Android Gradle 前端**。README 中残留的 `android/BachataS4` 链接不能代替真实目录存在性检查。

因此，当前公开材料里的“最新 app 版本”“最新 ARM64 核心源码”“可用 Android 前端源码”没有形成一个可以直接 checkout 的统一版本。

所有核查结果已保存于 [远端证据 JSON](data/android-baseline-sources-2026-09-07.json)，包括发布对象、仓库 tree 和源码关联元数据。

## 4. 如何找到并确认这份完整 Android 源码

官方发布仓库的 [source-links.json](https://github.com/JICA98/Bachata-S4/blob/c9263f39394a16bbb9705db5d06faf8cd61ad6e4/release-metadata/source-links.json) 给出：

| 官方声明 | 历史源码提交 |
|---|---|
| v0.1.8 historical source | `ce8e322eb4ec62c457af8a444466c8978a79e8fd` |
| v0.1.9 historical source | `67dbf4e5b54b0467bf8a93de3de0a557a71f9f43` |

它链接的 `JICA98/Bachata-S4-Fork-Archive` 在本次公开查询中返回 404，匿名 Git 也无法检出；这表示本次无法公开访问，不能据此断言已删除。

继续检查公开 forks 后：

- `Coldboy195/Bachata-S4` 保留 `ce8e322e…`。
- `zFitness/Bachata-S4` 保留 `67dbf4e5…`，与官方元数据指定的完整 SHA 相同。

因此选择后者作为获取渠道。不是根据 fork 名称信任其自定义修改，而是用官方元数据的精确提交确认源码对象。

进一步本地 Git 对比发现：**`ce8e322e → 67dbf4e5` 只有 README 改动，Android/runtime/core 代码相同。** 同时：

1. [version.properties](https://github.com/zFitness/Bachata-S4/blob/67dbf4e5b54b0467bf8a93de3de0a557a71f9f43/android/BachataS4/version.properties#L9) 仍为 `0.1.8`、`26081400`。
2. [package-runtime.mjs](https://github.com/zFitness/Bachata-S4/blob/67dbf4e5b54b0467bf8a93de3de0a557a71f9f43/runtime/scripts/package-runtime.mjs#L335) 仍打包 Box64，lock/UI 也仍有 Box64。
3. v0.1.9 的 release notes 却声称已移除 Box64 并锁定 FEX。

所以 **“与 v0.1.9 官方源码链接一致”不能写成“已取得完整、与 v0.1.9 APK 一致的源码”**。本次没有复现 APK，也没有验证二进制与这份源码逐项一致。开发分支采用独立名字，不把 Gradle 版本号人为改成 0.1.9 或 0.2.1。

## 5. 相比本地旧版本，值不值得换基础

值得作为新的 Android 前端基础。它保留模块化工程，已有：游戏库、设置、驱动管理、session/service、触控/手柄、导入与诊断，以及 FEX guest bridge 参考。Git 中 `android/` 有 516 个跟踪文件，其中 211 个 Kotlin 文件；已具备可复用的 app 骨架。

与本地 `e170f800` 比较，Android 层排除 `vortek/vendor` 后为 **58 文件、增加 4,100 行、删除 1,794 行**；可见变化包括导入逻辑、library/settings UI、运行时配置容错等。含 runtime/native 的大范围差异有很多 vendored 图形代码，不能按总代码增加量推断功能成熟度。

不过它不是本地分支的直接 fast-forward：共同祖先是 `37d0b4fc`，旧分支包含 fork 的构建修复和用户的 SG8275 修复。本次将新版独立放置，后续选择性迁移适用的修复。

特别需要复核旧提交 `e170f800` 中的三项修复：

- F-Droid 缺少 runtime assets 时的下载 fallback；
- 缺少 FEX artifact 时默认改走 Box64；
- glibc Box64 使用对应 loader 启动。

其中第二、三项服务于旧资源包和容器启动，不宜原样成为最终 NDK/FEX native 后端的默认行为。第一项对于仍保留旧兼容 runtime 的产品 flavor 有意义。现有未提交的 `EmulationService.kt`、NDK 版本修改也应在后续迁移时逐项审查，不能盲目覆盖新版。

## 6. 它能否直接满足 Android 16 / 16 KiB

**不能。当前选择解决的是 Android 工程缺失，不代表已经解决原生 backend 与页大小。**

源码中的 [GuestEngine 页大小检查](https://github.com/zFitness/Bachata-S4/blob/67dbf4e5b54b0467bf8a93de3de0a557a71f9f43/src/core/fex/fex_guest_engine.cpp#L1283) 要求 host page 为 4096，其他值返回 `ENOTSUP`。现有运行路径也依然使用 Debian/glibc、显示/图形桥和 managed runtime。

适合复用和替换的范围：

| 层 | 建议 |
|---|---|
| Compose UI、library、settings、Room/DataStore | 作为 app 基础保留，逐步清理不适用的设置 |
| Service、session state、导入、input、生命周期 | 复用状态与用户流程，重新接 native backend 边界 |
| driver 管理 | 保留选择/管理能力；核实与原生 Vulkan loader 的连接方式 |
| managed process、Box64、glibc、X11/Vortek | 可保留作已有兼容路径参考；不作为最终原生执行架构 |
| FEX guest bridge / ARM64 core | 与新版参考一起审计，用版本隔离 adapter 接入 |
| native surface/audio/storage/debug | 按 Android host 服务实现，纳入 16 KiB 和调试门槛 |

推荐组合是：**这份 Bachata Android 前端 + 当前 shadPS4 主仓核心 + 自有 NativeBackend/FEX adapter**。Android 层可以先在单独基线分支收敛，再移入主仓；不需要把旧 Bachata 整棵核心源码覆盖当前主仓。

## 7. 构建入口与实际验证程度

锁定源码中声明的工具版本：

| 项目 | 配置值 |
|---|---|
| minSdk / compileSdk / targetSdk | 31 / 37 / 37 |
| NDK | `30.0.14904198` |
| CMake | `3.22.1` |
| Gradle wrapper | `9.5.1`，附 SHA-256 |
| AGP / Kotlin | `9.1.1` / `2.4.0` |
| Java source/target | 17 |

这些是源码配置事实，不是本次安装/构建通过的工具链组合。compileSdk 37 并不意味着设备必须运行 API 37；运行兼容仍由 minSdk、API 使用、native 库与实际测试决定。

Android CMake 依赖 `externals/winlator-app` 等源码；新子仓的递归子模块和构建产物尚未初始化。本次运行并通过：

```text
node runtime/tests/verify-runtime.mjs --locks-only
runtime locks verified: components=8 inputs=30
```

它只验证 lock 定义，**不证明依赖可下载、runtime 已构建、APK 内容完整或设备可运行**。

后续复现旧 managed runtime 的入口是仓库根目录的 `runtime/scripts/build-runtime-debian.sh`，它要求 Linux ARM64 交叉编译工具等环境，然后才是 Android Gradle APK 打包。不能只运行 `assemble…` 并把 UI APK 当成完整模拟器。

对于我们选定的原生化方向，建议先做一个明确的 `NativeBackend`/mock backend 接口，使 app 的安装、启动、游戏库、设置、session 生命周期可以独立验证；后续接入真正的 NDK FEX harness。mock 必须明确报告尚未启动真实游戏，不返回虚假的 emulation success。这样 Android 前端建设不必等待整套旧 Debian runtime 被恢复。

## 8. 下一轮迭代的具体起点

1. 在新子仓的 `codex/android-foundation` 上验证 app 前端构建和安装；固定独立 applicationId/version，保留原有 app 的数据与版本识别。
2. 抽出 backend 契约：启动、停止、surface、input、audio、状态/错误事件、guest debug snapshot；让 UI 和 managed-process 细节分离。
3. 接入 Android 16 KiB 的最小 NDK/FEX guest harness，先过指令/内存/回调/线程与 LLDB 检查门槛。
4. 将经过整理的 Android 前端和 NativeBackend 接入当前主仓；FEX 版本适配与 shadPS4 核心变更分别审查。
5. 非 VR 可重复场景通过后，再推进 Beat Saber 的 PSVR/XR 链路。

本次已固定源码、分支、Git 子模块与证据；未构建/安装 APK、未将 Android 工程接入主仓、未提交或推送 Git。选定的是可以据此开展工作的源码基础，而不是发布标签所暗示的最新完整产品。

相关：[Android 原生化整体方案](fex-android16-native-guest-host-plan.md)、[LLDB host/guest 调试路线](fex-lldb-host-guest-workflow.md)。
