# shadPS4 Android / FEX development context

@AGENTS.md

`AGENTS.md` is the shared project context and working guidance. Read it first; this file provides the Claude entry point without maintaining a second independent policy.

## 快速入口

- [研究索引](docs/README.md)：整体方案、Android 基础、FEX/Dynarmic、guest debugger 与 LLDB。
- [references 源码索引](references/README.md)：用途、固定提交、初始化方法。
- [Android / ARM64 整合审计](docs/android-arm64-integration-audit.md)：后续开发首先引用这份。

## 必须记住

- 目标是 Android 16、ARM64、**16 KiB**；FEXCore 只执行 PS4 x86 guest，shadPS4 host 保持原生 ARM64。
- Android 前端与 ARM64 HLE/guest 桥有可复用代码；当前还不是可运行的 NDK/16 KiB 整合版本。重点是接口和运行时闭环，不是 app 的小版本标签。
- 第一阶段关注 NDK/bionic、16 KiB FEX harness、原生 Vulkan Surface 和停止/重启生命周期；普通手柄接通不等于 PSVR/Move 支持。
- 调试先落地 host LLDB + guest 状态适配；异步 JIT stop 不能直接把 CPUState 当完整寄存器快照。
- 子仓提交、主仓 gitlink、部署 binary Build ID 是三个不同对象。记录和核对实际用到的版本；保留子仓中的独立未提交工作。
