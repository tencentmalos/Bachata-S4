# FEX Windows 前端：供 Android guest/host adapter 参考

日期：2026-09-07。本文是 [Winlator / WinNative / GameNative 项目审计](winlator-winnative-gamenative-audit.md)
的辅助材料，聚焦仓内 FEX 源码，不把原生 Windows 开发扩入当前目标。未运行 Windows 或 Wine 测试。

## 1. 先区分四种 native

| 说法 | 实际含义 | 不代表什么 |
|---|---|---|
| FEX native Windows / Windows on Arm | FEX 作为 Windows PE 模块，接入 ARM64 Windows 的 x64 执行框架 | 不是 Android NDK 的 CPU `.so` |
| ARM64EC native DLL | Windows 的特殊 ARM64 ABI，允许与翻译后的 x64 代码互调 | 不是任意 AAPCS64/bionic 函数都能直接被 x64 调用 |
| WinNative | `WinNative-Emu/WinNative` 的 Android 应用项目名 | 不意味着 Windows 游戏已经重编译成 ARM64 |
| 游戏在 Android 上 locally/native 运行 | 运算发生在设备本地，而非视频串流 | 不意味着没有 Wine、CPU 翻译、图形转换和辅助进程 |

ARM64EC 是 ABI/工具链/运行时配合形成的混合执行机制；实际 x64 指令仍需要模拟器。
普通 ARM64 ABI 和 ARM64EC 也不能混为一谈。[Microsoft ARM64EC](https://learn.microsoft.com/en-us/windows/arm/arm64ec)、
[ABI 与调用转换](https://learn.microsoft.com/en-us/windows/arm/arm64ec-abi)

## 2. 原生 Windows on Arm + FEX

FEX 官方目前记录了 ARM64EC 模块在真实 Windows on Arm 上替换 x64 执行后端的开发用法，
并单独提示 WoW64 替换的已知问题；两个后端不能按“Windows 支持”一并视为成熟。
与 Wine 路线相比，这里由真实 Windows 的 NT 服务和 loader 承担系统责任。
[FEX ARM64EC 开发文档](https://wiki.fex-emu.com/index.php/Development:ARM64EC)

Microsoft Prism/xtajit64 与 FEX 是不同实现。Prism 不是可提取到 Android 的通用库，
但“Windows 平台限定”不能用于否定开源 FEX Windows 前端本身的架构价值。

## 3. FEX Windows 前端：最值得进一步借鉴的代码

本节依据仓内固定 `50e6eee95ae95d3257672727a9302a30b4a60a9a`；V0 当前仍选旧 FEX
`f2b679f6028ce1c38875233aecfcf5d3f8ebecec`。这两个版本的 `Source/Windows` 之间有
23 个文件变化、457 行增加、239 行删除（Git diff 统计），因此下面的具体实现不能假设旧版完全相同。

### 3.1 它已经是另一套 OS frontend

[ARM64EC CMake](../references/FEX/Source/Windows/ARM64EC/CMakeLists.txt) 将
`FEXCore_object` 链进 `libarm64ecfex.dll`，依赖 CommonWindows、NTDLL 和精简 CRT；
不是“先启动完整 Linux FEXLoader 再套 Wine”。
[BTInterface.h](../references/FEX/Source/Windows/ARM64EC/BTInterface.h) 暴露的关键契约包括：

| 实际接口/代码 | 宿主必须提供或协调的事情 | 对 shadPS4 的借鉴 |
|---|---|---|
| `ProcessInit` / `ThreadInit` / `ThreadTerm` | Context 生命周期、每线程 FEX 状态、TLS/栈 | 明确 Context 与 guest owner 的归属、创建和退出时序 |
| `NotifyMemoryAlloc/Free/Protect`、section map/unmap | 跟踪地址映射及代码权限变化 | VM/HLE 改映射后通知 CPU backend，统一失效入口 |
| `BTCpu64NotifyMemoryDirty`、cache flush | 通知已翻译代码失效 | guest store、host HLE 写入和 DMA/GPU 写入不能各自漏报 |
| `ResetToConsistentState` | 异常/挂起时把宿主现场还原为可继续的 guest 状态 | Debug snapshot 与 Resume 需要同一可信状态契约 |
| `ImageTracker` / `InvalidationTracker` | 模块生命周期、SMC 和元数据 | loader 事件可成为调试模块表与失效登记的共同来源 |

`ThreadCPUArea` 依赖 Windows TEB/CHPE CPU area 和专用 emulator stack。
**应借鉴职责，不复制 TEB 偏移或 NT 数据结构到 Android。** Orbis x64 采用的调用约定也
不同于 Windows x64；不能把 ARM64EC bridge 当作“免费生成”的 SysV→AAPCS64 bridge。

### 3.2 对 host LLDB 分析 guest 的直接价值

固定版本 [Module.cpp](../references/FEX/Source/Windows/ARM64EC/Module.cpp#L392) 的
`ReconstructThreadState` 做了我们关心的几件事：由 host PC 恢复 RIP、根据实际 SRA
映射收集 GPR/FPR、重建 EFLAGS，**并写回 FEX State**。它不是只读打印 helper。

同文件 [ResetToConsistentStateImpl](../references/FEX/Source/Windows/ARM64EC/Module.cpp#L675)
还区分 dispatcher/JIT/普通 host 地址，处理挂起边界和 inline SMC；doorbell 配合 block
入口的检查与 trap，实现协作挂起。这比单独观察 CPUState 更接近一个可信调试后端。

建议在 Android adapter 中采用相同原则：

1. 用实际版本的 mapping 和 host context 解释 JIT 现场，不硬编码一套通用 ARM64 寄存器解释。
2. 调试安全点输出独立、带世代和有效性标记的 snapshot；异步 host stop 另列状态质量。
3. 恢复执行的状态修改只能经受控 adapter，不从 LLDB `expression` 随手调用该重建函数。
4. Windows 异常处理链的锁、日志、stack switch 和 SEH 行为不能直接搬进 Android signal handler。

这增强了现有 [LLDB 工作流](fex-lldb-host-guest-workflow.md) 的源码依据，但没有取消 V0
对受支持单步、无 HLE 死循环中断、跨线程停止与精确快照的测试要求。

### 3.3 UnixLib 的真实含义

WinNative 发布说明中的 UnixLib 容易被误解成“FEX 全部已经放进 bionic `.so`”。
至少在我们审计的上游版本中，CPU 实现仍在 ARM64EC/WoW64 PE DLL；
[UnixLib 构建](../references/FEX/Source/Windows/UnixLib/CMakeLists.txt) 只编译
`FEXUnixLib.cpp`，其 [函数表](../references/FEX/Source/Windows/UnixLib/FEXUnixLib.h)
是 hardware TSO、非对齐原子内核控制、madvise、VMA 命名、共享统计区和文件映射等宿主辅助服务。

[PE 侧加载逻辑](../references/FEX/Source/Windows/Common/FEXUnixLib.cpp) 通过 Wine unix call
访问这些服务，并有 Wine/Proton 不同加载方式的兼容分支。它是“PE frontend → Unix host
services”的桥，不是面向 shadPS4 的 Dynarmic-like CPU API。具体社区包是否另外修改了分工，
仍应以其构建源码和产物为准。

这里的分层思想适合我们的 HostServices；代码却仍有 `librt`、`shm_open`、特殊 `prctl`
等 Linux 假设。函数返回 Unsupported 的内核能力不能当成 Android/骁龙设备的通用优化。
NDK 版应直接使用经过验证的 Android host 服务，不为复用这些函数引入 Wine。

### 3.4 Windows 特有的优化不可照搬

FEX [ImageTracker.cpp](../references/FEX/Source/Windows/Common/ImageTracker.cpp#L43)
会解析 PE volatile metadata，并把有效范围/指令送入 TSO 策略。
这是编译器元数据参与优化的具体机制，不能推导成“Windows 游戏可以安全关闭所有 TSO”。
PS4 ELF 没有自动等价的这套 PE 元数据；每游戏配置也不能把未经证明的内存顺序放宽变成默认值。
[FEX 2504 官方说明](https://fex-emu.com/FEX-2504/)
