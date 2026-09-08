# shadPS4 Android / FEX development context

@AGENTS.md

`AGENTS.md` is the shared project context and working guidance. Read it first; this file provides the Claude entry point without maintaining a second independent policy.

## 快速入口

- [子仓归属与开发分支](docs/subrepository-ownership.md)：动依赖前核对自有 remote、分支和固定版本。
- [Foundation 接入](docs/foundation-integration.md)：DebugBus 已有构建入口；反射、packing、网络优先复用，完整闭包仍待验证。
- [基础版本状态：2026-09-07 / a7128893](docs/baselines/2026-09-07-android-fex-foundation.md)：固定起点、已验证结果、未完成项和恢复方法。
- [V0 执行任务书](docs/specs/android-fex-v0-handoff.md)：首个验证版的 spec、CPU API、验收矩阵与交付要求，执行中。
- [2026-09-08 复核与修复](docs/validation/v0/followup-2026-09-08.md)：修复主仓 adapter 的共享译码缓存漏失效，Swan 4 KiB guest harness 36/36；quiesce/HLE/runner/app 仍待完成。结合[原始审核](docs/validation/v0/review-2026-09-08.md)阅读，不把测试断言数当作完整验收项数。

- [研究索引](docs/README.md)：整体方案、Android 基础、FEX/Dynarmic、guest debugger 与 LLDB。
- [references 源码索引](references/README.md)：用途、固定提交、初始化方法。
- [Android / ARM64 整合审计](docs/android-arm64-integration-audit.md)：后续开发首先引用这份。

## 必须记住

- 当前目标是 Swan / Android 16 / ARM64 / **4 KiB**；用户于 2026-09-08 将 16 KiB 工作后置。FEXCore 只执行 PS4 x86 guest，shadPS4 host 保持原生 ARM64。
- Android 前端与 ARM64 HLE/guest 桥有可复用代码；当前还不是可运行的 NDK/16 KiB 整合版本。重点是接口和运行时闭环，不是 app 的小版本标签。
- 第一阶段关注 NDK/bionic、4 KiB 设备上的真实 FEX 执行、原生 Vulkan Surface 和停止/重启生命周期；普通手柄接通不等于 PSVR/Move 支持。
- 调试先落地 host LLDB + guest 状态适配；异步 JIT stop 不能直接把 CPUState 当完整寄存器快照。
- 子仓提交、主仓 gitlink、部署 binary Build ID 是三个不同对象。记录和核对实际用到的版本；保留子仓中的独立未提交工作。
- Foundation 不替代 guest CPU API；网络命令投递给 owner thread，停用服务后等待 in-flight 请求退出再销毁 registry。不要将通用反射、序列化或网络设施在主仓重复实现。
