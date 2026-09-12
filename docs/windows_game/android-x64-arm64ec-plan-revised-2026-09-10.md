# Android x64 游戏运行时方案：以 WinNative 为复现起点

版本：修订版 1.0；日期：2026-09-10。

状态：**PLAN_DRAFT / 资料与源码核查完成 / 运行基线尚未复现**。

本文整理 WinNative、Wine bionic、ARM64EC、FEX 与 Proton 的选型及实施路径，修订[原规划](android-x64-arm64ec-plan.md)。原稿保留。本文不是已完成实现的报告，也不授权启动构建、设备安装或新仓库创建；本次交付仅为方案文档。

## 1. 决策摘要

**优先复现 WinNative 已有的 Wine bionic + FEX ARM64EC 配套运行时，确认源码与产物可对应、关键组件可重建，再逐步形成自己的 Android Windows 游戏后端。**

采用这条路线的理由是已有公开装配实现、Android 平台桥接和组件构建脚本可参考。它不保证指定游戏可玩，也不说明现有预编译包都能完整重建。

本方案作出以下选择：

1. WinNative 是首选复现和移植起点；是否正式 fork 整个应用，在 P0 的源码与构建审计后决定。
2. Wine bionic、FEX PE 模块和 UnixLib 是配套运行时，一起复现；不先做一个孤立 FEX 后端、以后再接 Wine。
3. 第一轮沿用基线已有的窗口与图形连接方式，保留定位问题所需的对照。
4. 默认准备现有开发工作站、x86_64 Linux 构建环境和 Android 目标设备。ARM64 Linux builder 不作为统一前置条件。
5. 完整重建官方 ARM64 Proton 是后续可选分支；其构建要求不能推广到 WinNative 的每个组件。
6. 先覆盖 x64、离线、非 VR、D3D11 游戏；Steam 接入、D3D12 和原生呈现分别设门槛。
7. 与 shadPS4 Round 2 分仓或独立工作区、独立版本锁、独立验收；不变更现有 PS4 开发目标。

## 2. 范围与成功定义

首要验证环境为 Android 16 / ARM64 / 4 KiB，优先使用 Swan；辅助 Android 设备用于对照，不能替代目标环境验收。目标 APK 使用普通 app 身份并以 targetSdk 36 验证。具体 GPU、驱动、内存与页大小以设备采集结果为准。

首个可交付里程碑是：在固定设备和版本上，一款合法取得、已列入范围的 x64 D3D11 离线游戏，完成启动、真实场景呈现、输入、音频、存档、退出及再次启动，并保留可以重现的构建和运行记录。

本阶段暂不承诺：通用 Steam 库兼容、反作弊、i386 游戏、PC VR、macOS 宿主、16 KiB host page 和所有 D3D12 游戏。64 位游戏若依赖 32 位安装器或辅助程序，也需单独处理，不能按主 EXE 架构直接判为在范围内。

## 3. 已核实事实与证据边界

| 项目 | 本次确认的事实 | 尚未证明的事项 |
|---|---|---|
| WinNative 运行时 | 历史固定源码具有 ARM64EC Wine、FEX DLL 和 UnixLib 的装配分支 | 某个完整版本组合已在 Swan 普通 APK 中正确运行 |
| GameNative | 固定 bionic launcher 有 ARM64EC 分支，并仍设置 DISPLAY | 它的所有运行路径都使用同一个 CPU 或呈现后端 |
| WinNative APK 构建 | 当前 CI 在 ubuntu-latest 上运行 Gradle 构建 | 我们已复现该流水线，或它重建了全部 Wine/FEX 组件 |
| FEX ARM64EC 构建 | Components 的脚本使用 x86_64 版 llvm-mingw 交叉构建 ARM64EC 与 AArch64 PE 模块 | nightly 构建成功、模块配套正确或游戏验收通过 |
| DXVK / vkd3d-proton | 当前脚本也包含 ARM64EC 交叉构建流程 | ARM64EC 产物已在选定 Android 驱动上正确运行 |
| Wine / Proton 组件 | 目录说明将部分运行包归于 GameNative、REF4IK 或社区来源 | 每个包都具有完整可访问的源码、补丁和构建闭包 |
| shadPS4 FEXCore | 仓内已有限定范围的 NDK/bionic CPU 执行证据 | Wine、完整 Linux 用户态前端、Windows 游戏或 16 KiB 完整执行已验收 |

装配证据：[WinNative launcher][s1]、[GameNative bionic launcher][s2]。构建与来源证据：[APK CI][s3]、[FEX CI][s4]、[DXVK CI][s5]、[vkd3d CI][s6]、[Proton 包来源][s7]、[Wine 包来源][s8]。

## 4. 架构与执行边界

### 4.1 Wine、Proton 与 FEX

Wine 实现 Windows 的程序加载、系统接口、线程、文件、窗口及异常等兼容行为。Proton 包含修改版 Wine，并整合图形、媒体和游戏兼容组件；采用 Proton 时无需再额外叠加一层独立 Wine。FEX 负责把 x86/x64 机器代码转换成可在 ARM64 上执行的代码。[Proton 说明][s9]

ARM64EC 是允许兼容的原生模块与 x64 代码互调的 ABI 机制。**翻译范围是仍为 x64 的代码，不是“仅游戏 EXE”。** 游戏引擎 DLL、插件、第三方运行库、回调和动态生成代码均可能继续经过 FEX；具体范围取决于已装载模块。普通 AArch64 PE DLL 与 ARM64EC DLL 也不能任意互换。[Microsoft ARM64EC][s10]

### 4.2 运行分工

```text
Android 应用：游戏库、配置、会话和诊断
    │ 启动、控制、输入、退出；具体进程模型由基线确认
    ▼
Wine 运行时
    ├─ Unix 侧：ARM64 ELF / bionic、wineserver、宿主桥接
    └─ PE 侧：Windows API 与 ARM64EC 互操作
          ├─ x64 EXE / DLL / 动态代码 ← FEX 翻译执行
          ├─ ARM64EC Wine / DXVK / vkd3d 模块 ← 原生执行
          └─ FEX ARM64EC PE 模块 ← 原生运行的翻译器

图形分支：Direct3D → DXVK / vkd3d-proton → winevulkan
          → 基线窗口/WSI/宿主 Vulkan 连接 → Android GPU / 显示
```

这张图表达职责，不表示模块按图中顺序构成固定调用链，也不表示所有内容位于 APK 的 ART 进程。FEX 生成并缓存译码；不能把图中的每条 API 调用都解释成一次重新翻译。

### 4.3 三组独立决策

| 决策 | 影响 | 不会自动解决 |
|---|---|---|
| ARM64EC + FEX | 哪些代码原生执行、哪些代码翻译执行，以及调用边界 | Android 显示、音频和进程生命周期 |
| Wine Unix 侧采用 bionic | ELF 依赖、Android 链接与系统接口适配 | Wine prefix、PE 模块和辅助进程的管理 |
| 原生 Android 窗口/WSI | Windows 窗口到 Surface 的对应与呈现成本 | Wine/FEX 的 ABI、线程和异常正确性 |

因此，ARM64EC 不等于取消 X11、Vortek 或所有运行时文件系统。第一轮是否存在这些组件，应通过真实启动命令、装载模块和配置确认，不能由项目名推断。

## 5. 路线与依赖选择

| 路线 | 本方案定位 | 进入条件 |
|---|---|---|
| WinNative 现有配套组合 | 首选复现基线 | 锁定实际组件与设备，建立运行记录 |
| 在基线上修改 Wine bionic / FEX / 图形组件 | 主实施路径 | 对应组件源码可追溯，逐项重建与回归 |
| 迁入选定 Proton 11 的更多组件 | 后续版本升级候选 | 先明确需解决的问题及 Wine/FEX/UnixLib 的版本兼容 |
| FEX 完整 Linux 用户态 + x64 Proton | 独立研究与对照候选 | 单独证明 Linux loader、syscall、信号、thunk 和运行环境可用 |

最后一条不能作为自动兜底：当前 shadPS4 的 bionic 构建刻意裁掉了 LinuxEmulation 等组件，CPU 核通过测试不等于完整 Linux 用户态已经可运行。

若基线重建受阻，先识别是源码缺失、工具链、版本组合还是目标环境问题。可保留预编译包作为临时对照，或调查另一套可重建组合；没有可重建运行时之前，不宣布产品化基础成立。

## 6. 开发环境与工具链

### 6.1 构建主机

| 工作 | 建议环境 | 已有依据与限制 |
|---|---|---|
| Android 前端、JNI、调试 | 现有 Mac / Windows / Linux 工作站 | 按选定 app 提交的 SDK、JDK、NDK 配置复现；自定义脚本另验 |
| FEX ARM64EC / WoW64 DLL | x86_64 Linux + llvm-mingw | 当前 Components CI 明确下载 x86_64 宿主工具链并交叉构建 |
| DXVK / vkd3d ARM64EC | x86_64 Linux + llvm-mingw + Meson/Ninja | 当前 CI 存在对应 cross-file 构建 |
| Wine bionic 与 UnixLib 全量重建 | 优先调查 x86_64 Linux 交叉构建方案 | 依所选 runtime 的真实构建配方确认，尚未核实全闭包 |
| 官方 Proton ARM64 整包构建 | 按所选版本要求准备 ARM64 Linux 环境 | 参考 Proton 5b89db94 的 README；不是所有 PE 构建的普遍约束 |

无需为了启动 P0 先购置 ARM64 Ubuntu 机器。若后续需要，可评估现有 Apple Silicon 上的 ARM64 Linux 虚拟机或远程 ARM64 runner；实际资源和脚本兼容需验证，不预先承诺可替代全部构建环境。

### 6.2 两套产物世界

| 产物 | 格式 / 目标 | 编译与依赖边界 |
|---|---|---|
| Wine Unix 侧、wineserver、宿主 helper | ARM64 ELF / bionic | NDK 或经确认的 bionic 交叉工具链 |
| Wine PE 侧、FEX DLL、图形 DLL | PE/COFF；按模块区分 ARM64EC、AArch64、x64 | 配套 llvm-mingw、Wine 构建工具和 Windows ABI |
| FEX UnixLib | ARM64 ELF / bionic | 与 PE 模块函数表、结构布局和 Wine 装载机制配套 |
| APK / JNI / 图形和音频桥 | Android ARM64 ELF 与应用资源 | Android 生命周期、链接命名空间及驱动依赖 |

NDK 与 llvm-mingw 可以共存，但“编译器版本不同”既不是必然错误，也不是自动安全。应验证边界上的布局、调用约定、指针和缓冲区所有权、回调、异常以及版本协商。FEX 的静态 C++ runtime 不会消除其 Wine/NT 与宿主服务依赖。

NDK 版本跟随选定 ELF 组件的实际需求锁定。shadPS4 的 bionic FEXCore 因 atomic_ref 使用了特定 NDK，不意味着 B 路线的 PE FEX 也受该 NDK 限制。native API 与 Java targetSdk 分开记录，并核对实际 sysroot 元数据，不能把某个本地安装的 max API 视为整个 NDK 版本的永久属性。

## 7. 版本锁与源码可重建性

### 7.1 研究快照不是运行组合

| 对象 | 本文核查快照 | 用途 |
|---|---|---|
| WinNative launcher | 8ed4ff8013bdb5be923b2253f4c2cacdd5f62fb4 | 历史装配路径证据 |
| GameNative launcher | d9e9764a84e60d792070fb0e6f380ca3f69769db | bionic ARM64EC 与显示环境证据 |
| WinNative app CI | cfc062aeecdbcc361c913ee44801c3b04cbb0e80 | 2026-09-10 当前构建脚本快照 |
| WinNative Components | 1c5253433b5f7cf3c59a677a9634782ced1d6c2d | FEX/图形组件交叉构建及包来源 |
| Proton | 5b89db940e0ebe3a137a6009a3589232fe084c09 | 后续升级候选的构建规则 |

这些提交来自不同用途，**不得直接拼接成“已验证版本锁”**。特别是该 Proton gitlink 锁定 FEX `1cc4b93e7a71c883ec021b71359f136394dc1f3c`，不是 shadPS4 当前自有 fork 的 `385a0cc4d…`。[Proton 固定源码树][s11]

### 7.2 P0 必须产出的锁文件

锁文件至少包含：app commit、每个 runtime 包的下载地址与 SHA-256、Wine/FEX/UnixLib/DXVK/vkd3d 的源码 SHA 和补丁摘要、工具链包校验值、构建参数、native API/targetSdk、ELF/PE 身份、prefix 初始状态、图形与音频后端、驱动包和有效配置。

对无法关联源码的组件明确写 `source_binding: unknown`，不得用文件名或应用版本替代。nightly 跟随上游分支的脚本只能作为构建配方参考；实验应固定源码、补丁与工具链。

复现允许先使用已知预编译包；形成自己的可维护后端之前，关键 runtime 必须能从干净环境重建。初期要求输入、过程和产物身份可追溯，不未经验证就宣称构建结果逐字节一致。

## 8. 分阶段实施与出口

### P0a：复现已有组合

目的：建立可以持续对照的 WinNative 运行基线。

- 固定 app 和 runtime 包，确认真正选中的 FEX、Wine、UnixLib、图形与音频路径。
- 运行 x64 控制台程序，并尝试一款符合范围的简单离线游戏；失败也归档环境和日志。
- 记录装载模块和架构，不能仅以“FEX 已解压”的日志认定它在执行游戏。
- 输出版本锁、启动说明和一次完整运行记录。

出口：至少 x64 探针运行可重复，组件身份与执行路径明确。只有 shell 结果时标为 shell 辅助证据，不标为 app 验收。

### P0b：源码重建与混合架构探针

目的：把可运行的外部组合变成可维护的构建基线。

- 查清 Wine bionic 及 UnixLib 的来源，逐个重建关键组件，并逐个替换后回归。
- 验证 x64 EXE 调用 x64 DLL、x64 调用 ARM64EC DLL及反向回调；覆盖整数、浮点和基本缓冲区参数。
- 覆盖线程创建/退出、TLS、基本异常、函数返回及退出码；增加缺 DLL、错误架构与损坏文件的失败用例。
- 核实 FEX 的 TSO、原子与可选宿主能力配置；设备缺少某项能力时保持正确性回退，不以关闭语义检查换取通过。

出口：关键组件可重建，替换后探针通过，失败能定位到组件和版本。Hello World 是入口探针，不是混合 ABI 的完整证明。

### P0c：Android 16 普通 APK

目的：尽早排除 shell 与目标产品环境之间的差异。

- 以普通 app UID、targetSdk 36、4 KiB host pages 运行同一批探针。
- 明确 Activity/service、Wine 客户进程、wineserver 和 helper 的创建、通信、终止与回收关系。
- 验证原生可执行文件的合法部署位置、动态依赖、链接命名空间及文件/FD 访问。
- 验证停止、异常退出与再次启动，不能靠遗留进程或上次 prefix 的隐含状态成功。

Android 对 app 可写目录中的 execve 有限制，因此不得用 adb shell 的权限环境替代此门槛。[Android 行为说明][s12]

出口：最小 APK 在目标设备完成启动→运行→停止→重启；建议做 100 次循环，记录残留进程/线程/FD 和内存趋势，泄漏阈值在运行前写入测试配置。

P0a–c 可以交错推进；首次普通 APK 探针应尽早执行，无需等待全部运行时重建完成。任一阶段阻塞时先分类根因和可选修复，不自动切换到尚未验证的完整 FEX Linux 路线。

### P1：首款完整游戏，沿用基线呈现

- 先选本地启动、不依赖 Steam 客户端服务、离线、非 VR 的 x64 D3D11 游戏。
- 验证实际游戏场景、输入、音频、存档、退出、重新启动；菜单或单帧出图不算完成。
- 配置错误和不支持功能必须返回可诊断失败。
- 从本阶段建立小规模游戏与设备回归，不等产品化后补。

出口：固定场景连续运行至少 30 分钟，完成一次存档后冷启动读取，并记录帧时间、内存与温度。此时只宣布选定游戏/配置通过。

### P2a：Android 呈现与宿主桥优化

先测基线成本，再决定是否替换 X11/Vortek 或已有 compositor。目标路径可为 winevulkan/窗口驱动 → Android Surface/ANativeWindow，但必须补全 HWND 生命周期、WSI、resize、前后台、Surface 重建、present 同步与输入焦点。

出口：D3D11 场景在新旧路径间可对照；Surface 销毁/重建后恢复正确，无明显资源持续增长。GDI 显示和 Vulkan swapchain 分别验收，AHardwareBuffer 的存在不等于全链路零拷贝。

### P2b：图形版本与 D3D12

按目标游戏所需能力决定升级 DXVK、vkd3d-proton 或 Proton 的其他组件，避免同步升级整个运行时。

- 为选定提交导出实际 Vulkan extensions/features/limits/格式需求，采集目标驱动能力。
- D3D12 使用独立探针和游戏出口；D3D11 通过不能替代它。
- 第二款 GPU 代际设备在此阶段进入回归；厂商驱动与 Turnip 分别标识和验证。

出口：每项版本升级有独立 A/B 结果。若 D3D12 未达成，保持该功能延期，不能据“DLL 编译成功”宣称支持。

### P3：Steam 接入

原生登录、库管理、下载可以参考 GameNative/Pluvia；游戏运行时服务另立验证项。明确区分账户访问、文件下载、Steamworks API、Steam DRM wrapper、第三方 DRM 与启动器依赖。[Steamworks API][s13]、[Steam DRM][s14]

首批游戏按服务依赖建立白名单。若不运行官方客户端，明确支持的 API/服务子集和已测游戏，不能承诺任意 Steam EXE 可直接启动。无法满足的服务依赖作为兼容性限制记录，不默认采用绕过方案。

出口：选定正版游戏完成登录/库→下载→安装→启动→存档全流程，且能够描述它实际使用哪些客户端服务。媒体解码按首批游戏需求纳入；不能把音频与所有媒体能力一并裁掉后仍称完整运行。

### P4：兼容性与维护

扩充游戏×设备×驱动×runtime 矩阵，建立升级回归、崩溃分层、组件回退和更新机制。兼容性结论附版本和配置；ProtonDB、WinNative/GameNative 社区记录可作为调查线索，均不能直接继承为本产品评级。

## 9. 验证与性能记录

每次验证至少记录以下内容：

| 类别 | 必填内容 |
|---|---|
| 软件身份 | app/runtime/驱动 hash、源码绑定、工具链和有效配置 |
| 设备环境 | 型号、SoC/GPU、系统 build、API、真实页大小、内存、运行 UID、targetSdk |
| 执行事实 | 命令和生命周期事件、已加载 PE/ELF、CPU 后端、显示/音频路径 |
| 结果 | PASS/FAIL/NOT_RUN、退出码、超时、日志、截图/录屏与失败阶段 |
| 性能 | 同场景帧时间分布、卡顿、RSS/峰值内存、CPU/GPU负载、温度及持续运行时间 |
| 构建与回归 | 编译日志、产物身份、探针结果及已知限制 |

对性能只作同设备、同游戏版本、同场景、同分辨率、同功耗/温度条件的比较；驱动及 shader cache 的冷热状态必须对应。关闭或单独报告插帧，不能将生成帧数当作游戏实际渲染吞吐量。先比较基线与单项修改，再讨论 FEX/Box64 或不同架构的收益。

构建检查必须区分 ELF 与 PE：ELF 检查 machine、解释器/依赖、允许的符号版本、动态装载与对齐；PE 检查适用的 machine/hybrid 元数据、imports、Wine builtin 标记与配套模块。不要把“所有 ELF 都不得有任何符号版本”作为 bionic 判据，应按实际依赖和平台允许项检查。

失败用例必须影响总结果和退出码；“未启动”“错误架构”“损坏文件”“guest fault”“超时”分别记录。CLI 辅助 PASS 不提升为目标 APK PASS，构建 PASS 不提升为游戏 PASS。

## 10. 风险、范围控制与处理方式

| 风险 | 优先处理方式 |
|---|---|
| 预编译 Wine 包缺少可关联源码/补丁 | P0b 追溯，不能重建则不通过可维护性门槛 |
| Wine/FEX/UnixLib 版本不匹配 | 固定完整组合，逐组件替换并保留旧包 |
| Android app 权限与链接环境不同于 shell | P0c 提前验证，记录真实 UID 和进程部署 |
| 混合调用、回调、异常或线程状态错误 | P0b 探针与负向用例，按具体边界定位 |
| GPU 功能不足或驱动缺陷 | 按固定图形组件列需求，逐驱动/机型验证 |
| 原生 WSI 改动过大 | 保留基线呈现路径，独立实施 P2a |
| Steam 服务覆盖不足 | 白名单和 API 子集，按游戏明确限制 |
| 工具链与组件持续漂移 | 固定 source/patch/toolchain，升级独立回归 |

反作弊游戏首期不承诺支持。不能写成“EAC/BattlEye 在任何本地方案都不可行”：Valve 记录了部分 Proton 支持，Android ARM64 组合仍需独立证据。[Valve 反作弊说明][s15]

16 KiB 延期保持不变。现有 FEX host-page 修复和布局探针只覆盖局部，不代表 Windows frontend、Wine PE 映射、JIT、信号和图形路径的完整 16 KiB 验收。

模块来源及许可证在选择复用边界时登记；正式分发前按实际代码和组件清单处理对应义务，不用一个笼统的“GPL 传染范围”替代逐项确认。

## 11. 人力、时间与投入门槛

角色至少覆盖 Android app/系统集成、Wine/FEX/混合 ABI、图形/驱动、构建与验证。可由同一人承担多个角色，但工期需按实际投入重新估计，不将角色数自动视为已安排人员。

P0 可设 2–3 周的初始调查预算，目标是产出复现、构建来源和目标 APK 可行性的具体结论；它不是“时间一到就证明 B 不可能”的期限。P1–P3 在 P0 结果出来后按缺口估算，原稿 16–23 周总工期不作为交付承诺。

投入门槛：P0a 获得可对照组合；P0b 获得关键 runtime 重建能力；P0c 获得目标 APK 证据；P1 获得首款完整游戏；此后才扩大原生呈现、Steam 和兼容性投入。

## 12. 与 shadPS4 的复用与隔离

Windows 游戏运行后端和 PS4/Orbis 后端分别拥有其 loader、系统接口、线程/异常及图形转换语义。

| 资产 | 复用方式 |
|---|---|
| FEX 通用修复 | 先判断对应版本/执行路径是否适用，在 Windows 独立分支验证 |
| Orbis typed HLE、guest_cpu adapter | 不直接承担 Wine 的系统层；保留在 PS4 后端 |
| 停止、代码失效、状态与失败处理经验 | 转化为 Windows 测试和审计问题，不另外接管 Wine 已拥有的线程/VM |
| 部署身份、日志和回归方法 | 可复用方法与适当脚本，需适配 PE/ELF 和多进程事实 |
| Android 输入、音频、窗口经验 | 依据基线接口逐项复用，不假设 JNI/ART 同进程集成已成立 |

新方向使用独立仓库或工作区、版本锁和验收记录。不要为接 Wine 修改现有 Round 2 的规范或 reference pin；共用 fork 的变化需独立审阅，不能因同源而直接替换。

## 13. 首轮执行清单

1. 选定 WinNative app 与完整 runtime 包组合，核对有效配置及下载来源。
2. 确认目标设备环境，建立 shell 与普通 APK 两种结果的独立记录。
3. 跑 x64 最小探针，采集实际模块和后端身份。
4. 追溯 Wine bionic / UnixLib 的构建闭包，同时复现 FEX PE 和图形组件交叉构建。
5. 逐项替换组件，增加混合 ABI、回调、线程和失败用例。
6. 完成普通 APK 的生命周期与重复启动验收。
7. 按 P0 结果选择正式 fork/移植范围，再确定 P1 游戏和后续预算。

## 14. 原规划的主要修订

| 原结论 | 修订后 |
|---|---|
| 仅翻译游戏 EXE | 翻译所有仍为 x64 的模块及动态代码 |
| bionic + ARM64EC 没有公开证据 | 已有公开源码路径；指定组合和目标环境待复现 |
| ARM64EC 自动去掉 X11/Vortek | CPU ABI 与窗口/WSI 独立决策 |
| FEX DLL 自身属于被翻译层 | FEX DLL 是原生翻译器，执行它生成的代码 |
| 必须先有 ARM64 Ubuntu 构建机 | APK/FEX/图形组件有 x86_64 构建路径；Wine 全闭包另核实 |
| 当前 bionic FEXCore 可直接兜底完整 Proton | Linux 用户态前端未由现有证据覆盖 |
| P0 只看 Hello World，失败就切 C | 复现、可重建性、混合执行和普通 APK 分门槛 |
| shadPS4 工作复用为零 | 产品语义分离，测试/诊断和通用经验仍可复用 |
| FEX 16 KiB 已完成 | 仅局部修复与探针，完整执行验收延期 |
| ProtonDB 只对 C 有效 | 各路线均可参考，评级均不能直接继承 |

## 15. 来源与核查说明

本文引用的固定 SHA 是研究快照，实际实施须另建配套 runtime lock。动态官方文档以 2026-09-10 查阅内容为依据。本文编写期间未安装 APK、编译这些 Windows runtime 或运行商业游戏。

- [S1：WinNative 固定 launcher][s1]
- [S2：GameNative 固定 bionic launcher][s2]
- [S3：WinNative APK CI 快照][s3]
- [S4：Components FEX 构建快照][s4]
- [S5：Components DXVK 构建快照][s5]
- [S6：Components vkd3d 构建快照][s6]
- [S7：Proton 组件来源][s7]；[S8：Wine 组件来源][s8]
- [S9：Proton 固定版本说明][s9]；[S11：Proton 固定源码树][s11]
- [S10：Microsoft ARM64EC][s10]；[S12：Android app 执行限制][s12]
- [S13：Steamworks API][s13]；[S14：Steam DRM][s14]；[S15：Proton 反作弊支持][s15]
- [S16：DXVK 驱动要求][s16]：用于说明必须按版本核对功能需求，不替代本方案所选 pin 的能力矩阵。

[s1]: https://github.com/WinNative-Emu/WinNative/blob/8ed4ff8013bdb5be923b2253f4c2cacdd5f62fb4/app/src/main/runtime/display/environment/components/GuestProgramLauncherComponent.java
[s2]: https://github.com/utkarshdalal/GameNative/blob/d9e9764a84e60d792070fb0e6f380ca3f69769db/app/src/main/java/com/winlator/xenvironment/components/BionicProgramLauncherComponent.java
[s3]: https://github.com/WinNative-Emu/WinNative/blob/cfc062aeecdbcc361c913ee44801c3b04cbb0e80/.github/workflows/pr-ci.yml
[s4]: https://github.com/WinNative-Emu/Components/blob/1c5253433b5f7cf3c59a677a9634782ced1d6c2d/.github/workflows/nightly-fexcore.yml
[s5]: https://github.com/WinNative-Emu/Components/blob/1c5253433b5f7cf3c59a677a9634782ced1d6c2d/.github/workflows/nightly-dxvk.yml
[s6]: https://github.com/WinNative-Emu/Components/blob/1c5253433b5f7cf3c59a677a9634782ced1d6c2d/.github/workflows/nightly-vkd3d.yml
[s7]: https://github.com/WinNative-Emu/Components/blob/1c5253433b5f7cf3c59a677a9634782ced1d6c2d/Proton/README.md
[s8]: https://github.com/WinNative-Emu/Components/blob/1c5253433b5f7cf3c59a677a9634782ced1d6c2d/Wine/README.md
[s9]: https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/README.md
[s10]: https://learn.microsoft.com/en-us/windows/arm/arm64ec
[s11]: https://github.com/ValveSoftware/Proton/tree/5b89db940e0ebe3a137a6009a3589232fe084c09
[s12]: https://developer.android.com/about/versions/10/behavior-changes-10#execute-permission
[s13]: https://partner.steamgames.com/doc/sdk/api
[s14]: https://partner.steamgames.com/doc/features/drm
[s15]: https://partner.steamgames.com/doc/steamhardware/proton
[s16]: https://github.com/doitsujin/dxvk/wiki/Driver-support
