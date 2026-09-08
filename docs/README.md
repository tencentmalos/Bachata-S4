# Android / FEX 开发资料索引

当前目标：Swan / Android 16 / ARM64 / **4 KiB**，原生 shadPS4 host + FEXCore 执行 PS4 x86 guest。用户于 2026-09-08 将 16 KiB 工作后置。尚未完成完整 NDK backend/app 验收。

**当前状态：[95bc13fa 后续 / 发布失败保护、sink 生命周期与执行准入](validation/v0/followup-poison-2026-09-08.md)**。R1–R5 已关闭：事务期间新执行被拒（Swan 实测 Run/CreateThread 均 Busy），失效失败后旧 JIT 代码不可达且可修复（rax 17→拒绝→修复后 34），注销 sink 等待回调排空；runner 的无输出失败改判 FAIL、skip 不再计 PASS，并区分"从未启动"与"启动后崩溃"。构建收敛回既有 `cmake/fex` 入口并补上真正的 `guest_cpu_fex` target。设备 contract 29/29、guest 41/41；验收 21 PASS / 0 FAIL / 37 NOT_RUN（58 在范围内，2 项 scope 延期）。运行中线程的中断、真实 HLE 与 app 未开始。[本轮复核](validation/v0/review-poison-2026-09-08.md)与[更早记录](validation/v0/followup-publication-2026-09-08.md)保留历史含义。

**固定基础版本：[2026-09-07 / `a7128893`](baselines/2026-09-07-android-fex-foundation.md)**。该记录包含主仓与七个 references 的精确提交、验证结果、未完成项、未纳入基线的本地改动及恢复方法；后续里程碑以它为起点。

**下一个实施目标：[V0 首个验证版 spec](specs/android-fex-v0.md)**。配套 [CPU API 契约](specs/android-fex-v0-api.md)、[验收矩阵](specs/android-fex-v0-acceptance.md)、[执行 AI 任务书](specs/android-fex-v0-handoff.md)。这些是待实现要求，不是新的已通过状态。

## 既有阶段记录

- [最初实施报告](validation/v0/implementation-report.md) 的 `V0_BLOCKED`、21 PASS / 0 FAIL / 39 NOT_RUN 是旧轮次输出；部分 host 单测被映射到完整验收项，不能作为当前完成比例。
- [FEX host page 适配](fex-host-page-size-adaptation.md) 记录早期 guard/InterruptFaultPage 调整。用户已将 16 KiB 工作后置；host 探针和页大小数学检查不代表 Android 16 KiB app 验收。
- [Android bionic 构建](fex-android-bionic-build.md) 记录初始化与线程生命周期；其“未执行 guest / adapter 未实现”是当时状态。
- [Guest 执行接入](fex-guest-execution-bringup.md) 记录后续真实执行、配置顺序及共享缓存失效修复，以最新复核证据为准。

## 推荐阅读顺序

实施前先读 [子仓归属与开发分支](subrepository-ownership.md) 和 [Foundation 接入记录](foundation-integration.md)。
Foundation 最小构建入口已接入；反射/网络闭包与 Android 16 运行仍待 V0 验证。

| 文档 | 解决的问题 |
|---|---|
| [Android 与 ARM64 C++ 整合审计](android-arm64-integration-audit.md) | 哪些接口已有实现、哪些需要适配、哪些阻碍目标运行；优先读 |
| [Android 16 原生 guest/host 方案](fex-android16-native-guest-host-plan.md) | 架构、16 KiB、地址空间、HLE、回调、原子访存、GPU 与实施门槛 |
| [Android 基线来源与选择](android-foundation-selection.md) | Android 工程出处、版本和源码不一致、构建入口 |
| [FEXCore 与 Dynarmic 源码比较](fexcore-dynarmic-source-comparison.md) | 源码规模及统计口径、API 与依赖差异 |
| [FEX host page 适配](fex-host-page-size-adaptation.md) | 三种页大小的区分、两处必经 4 KiB 假设的故障机制与修复、16 KiB 可行而 64 KiB 越界的原因 |
| [FEX Android bionic 构建](fex-android-bionic-build.md) | NDK r29 的 `atomic_ref` 硬前置、五处平台适配、两个未文档化的嵌入方义务、实机初始化证据与边界 |
| [Guest debugger 可行性](fex-guest-debugger-feasibility.md) | FEX 现有调试能力、协议缺口与执行状态接口 |
| [LLDB host → guest 工作流](fex-lldb-host-guest-workflow.md) | 安全点/异步 stop、寄存器来源、地址和反汇编关联 |
| [Winlator / WinNative / GameNative 开源项目审计](winlator-winnative-gamenative-audit.md) | ARM64EC、Wine/FEX 分层、UnixLib 的实际范围、Android 平台复用及游戏兼容性证据 |
| [Vortek / Gladio 图形桥接审计](vortek-gladio-graphics-bridge-audit.md) | Vulkan 与 GL→GLES 分工、client/server、AHB 呈现、BC 解码边界及原生 renderer 的复用取舍 |

这些文档以 2026-09-07 检出的源码为依据；版本、行号和能力判断需要随代码更新复核。仓内链接可随 checkout 使用；跨子模块文件在 GitHub 上若无法直接展开，可从 [references 索引](../references/README.md) 的精确提交进入。

## 源码与可复现检查

- [references/README.md](../references/README.md)：固定版本与初始化说明。
- [Android 基线锁](data/android-foundation.lock.json)：来源与实际验证边界。
- [C++ 传输测试脚本](../scripts/analysis/run_android_arm64_contract_tests.py)：18 项 host C++ 测试；不替代 Android/FEX/GPU 实机测试。
- [源码计量脚本](../scripts/analysis/compare_cpu_core_size.py)：使用固定 Git revision 的源码文本统计。
- [证据索引](data/README.md)：远端快照、差异与测试记录。

## 既有相关资料

- [shadPS4 / citron 架构对比](shadps4-citron-architecture-comparison.md)
- [PSVR 游戏列表](psvr-games-list.md)
- [PKG → ZAR 工作流](pkg-to-zar.md)

Beat Saber 的 PS4/PSVR 兼容性是独立后续目标。先验证非 VR guest 的执行、显示、输入、音频和生命周期，再推进 tracking、双眼呈现及 VR 时序。
