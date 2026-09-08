# shadPS4 Android / FEX development context

@AGENTS.md

`AGENTS.md` is the shared project context and working guidance. Read it first; this file provides the Claude entry point without maintaining a second independent policy.

## 快速入口

- **当前执行：[二周目任务书](docs/specs/android-fex-round2-handoff.md) / [spec](docs/specs/android-fex-round2.md)**。G0–G4 和 24 项验收：证据、运行控制、事务、真实 HLE、普通 APK；尚未实施。有限 Step/Vulkan 是后续 V0 欠项。
- [一周目提交与全量欠项](docs/baselines/2026-09-08-round1-closeout.md)：`85b57cb2` 已推送；Swan contract 34/34、guest 45/45。历史 runner 的 11 PASS 不等于 app 验收，尤其 B02 仅有独立 ELF 证据。
- [事务加固与验证](docs/validation/v0/transaction-hardening-2026-09-08.md)：源码/hash/原始日志；测试时 dirty patch 已纳入上述提交，不倒改历史测试身份。

- [子仓归属与开发分支](docs/subrepository-ownership.md)：动依赖前核对自有 remote、分支和固定版本。
- [Foundation 接入](docs/foundation-integration.md)：DebugBus 已有构建入口；反射、packing、网络优先复用，完整闭包仍待验证。
- [基础版本状态：2026-09-07 / a7128893](docs/baselines/2026-09-07-android-fex-foundation.md)：固定起点、已验证结果、未完成项和恢复方法。
- [V0 总 spec](docs/specs/android-fex-v0.md)：完整目标及 CPU API/验收矩阵；二周目只是其子集。
- [较早的发布失败保护与执行准入](docs/validation/v0/followup-poison-2026-09-08.md)：历史记录，后续增量修复及当前成绩以一周目结项为准。构建入口统一为 `cmake/fex`（`-DV0_ENABLE_FEX=ON -DFEX_BUILD_DIR=…`）。

- [研究索引](docs/README.md)：整体方案、Android 基础、FEX/Dynarmic、guest debugger 与 LLDB。
- [references 源码索引](references/README.md)：用途、固定提交、初始化方法。
- [Android / ARM64 整合审计](docs/android-arm64-integration-audit.md)：后续开发首先引用这份。

## 必须记住

- 当前目标是 Swan / Android 16 / ARM64 / **4 KiB**；用户于 2026-09-08 将 16 KiB 工作后置。FEXCore 只执行 PS4 x86 guest，shadPS4 host 保持原生 ARM64。
- Android 前端与 ARM64 HLE/guest 桥有可复用代码；当前有 NDK/bionic 独立 harness，尚无整合 APK。使用 FEX `385a0cc4d…`，不要按旧初稿回退至 reference 的旧 runtime pin。
- 第一阶段关注 NDK/bionic、4 KiB 设备上的真实 FEX 执行、原生 Vulkan Surface 和停止/重启生命周期；普通手柄接通不等于 PSVR/Move 支持。
- 调试先落地 host LLDB + guest 状态适配；异步 JIT stop 不能直接把 CPUState 当完整寄存器快照。
- 子仓提交、主仓 gitlink、部署 binary Build ID 是三个不同对象。记录和核对实际用到的版本；保留子仓中的独立未提交工作。
- Foundation 不替代 guest CPU API；网络命令投递给 owner thread，停用服务后等待 in-flight 请求退出再销毁 registry。不要将通用反射、序列化或网络设施在主仓重复实现。
