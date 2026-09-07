# 用 LLDB 断 host，分析 FEX guest：可行性、操作路径与工程化建议

日期：2026-09-07。源码基线：`references/FEX` 提交 `50e6eee95ae95d3257672727a9302a30b4a60a9a`；目标为 Android 16 / ARM64 / 16 KiB page 的原生 shadPS4 + FEXCore。

## 1. 结论：适合作为第一条调试主线

**可以，而且对早期 Android/FEX/HLE 接入，我建议优先做“host LLDB + guest 只读视图 + 少量可控 guest 停止点”。完整 guest RSP 可以随后再接。** 这比一开始修复 FEX Linux gdbstub 更直接，也更容易同时看到 bionic、signal、C++ HLE、Vulkan 与 guest 状态。

FEX 官方 crash 指南确实采用 host GDB 断住 FEX、查看 ARM64 指令和 CPUState、再反查 guest 二进制的流程。但官方示例是 GDB，并非一套已完成的 LLDB guest 插件；其中固定偏移和一些环境细节已过时。下面将这一思路按当前源码转成 LLDB 路径。[FEX crash debugging](https://wiki.fex-emu.com/index.php/Development:Debugging_Crash)。

这条路可分三个等级：

| 等级 | 能解决的问题 | 是否需要 guest RSP |
|---|---|---|
| H0：host stop 后看 guest | crash PC、模块、寄存器来源、内存、HLE 参数与调用关联 | 不需要 |
| H1：受控 guest 停止点 + LLDB | 按 guest 地址/HLE 条件停止，在一致边界读写状态、执行一步 | 不需要，但必须建设执行控制 |
| H2：标准 guest debugger | IDA/GDB 直接显示 x86 寄存器、断点、线程与单步 | 需要 RSP 或相应 debugger 后端；可复用 H1 |

**LLDB 能替代早期的调试界面和 transport，不能自动替代 FEX 的架构状态恢复与 guest 单步机制。** H1 做通后，才具备可靠 H2 的基础。

## 2. 先区分三个 PC 和两种寄存器

同一次停止至少应显示：

- **host PC**：当前真正执行的 ARM64 地址，LLDB 的 `$pc`。
- **guest block RIP**：当前译码块来自哪里，用于快速归因。
- **mapped guest RIP**：通过 block 中 host→guest offset 表，找到 host PC 对应的 guest 指令位置；附精度标签。

寄存器也有两份来源：CPUState 保存值，以及正在 host 寄存器中的静态分配值。当前 [CPUState:103](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/include/FEXCore/Core/CoreState.h#L103) 自己就说明 `rip` 在 JIT 活跃时可能不完全准确。

| 停止位置 | 应优先读什么 | 容易犯的错误 |
|---|---|---|
| 确认处于普通 FEX JIT block 内 | 当前 host 寄存器 + CPUState + block 映射 | 只打印 CPUState，把旧值当成当前值 |
| 已完成 spill 的 HLE/调试边界 | 此次 guest invocation 的 CPUState/快照 | 用 native 函数当前 x4 等当 guest GPR |
| dispatcher、spill/fill 中途、inline syscall | 分阶段元数据/保存 context | 套用统一 GPR 映射，混合新旧状态 |
| 已进入 host signal handler | 信号参数中的原始 ucontext + 对应 FEX frame | 用 handler 自己的寄存器代替故障现场 |
| 普通 Android/渲染线程 | host 栈/寄存器 | 把任意 `$x28` 都当作 FEX frame |

`InSyscallInfo`、signal 嵌套和 callback 层级必须参与判断。无法确认的字段显示 raw/unknown；不要给一个看似完整但错误的 guest 状态。

## 3. LLDB 的具体操作：哪些已验证，哪些需要目标联调

### 3.1 Android 连接与符号条件

使用 Android Studio native attach，或设备端 `lldb-server` + 主机 LLDB。`lldb-server` 调试的目标仍是 ARM64 app。Android 的进程调试权限、运行身份、debuggable 配置按设备/app 实际情况准备，不能假设 shell UID 可直接 attach 任意 app。[LLDB remote debugging](https://lldb.llvm.org/use/remote.html)、[lldb-server](https://lldb.llvm.org/man/lldb-server.html)。

端口已转发且 server 已可 attach 时，LLDB 侧可用 `gdb-remote 127.0.0.1:<转发端口>`。不要把它与 FEX 自带 x86 guest gdbstub 混为一个 server。

调试构建保留匹配 Build ID 的未剥离 `.so` 和完整 DWARF；只保留行号不足以可靠解释 FEX 内部结构。native frame pointer 有助于 C++ 栈，不会自动补齐 JIT unwind。优化构建也应保留符号，避免所有问题只能在改变了时序的 Debug 构建中观察。

### 3.2 首次停止：先分类 host 现场

以下为 LLDB 命令模板，地址和线程以实际现场为准：

```text
thread list
thread backtrace all
frame select 0
register read pc sp x28 cpsr
disassemble --start-address $pc --count 12 --bytes
memory region $pc
image list
```

判定 PC 在 native 模块、dispatcher 还是 JIT 区。JIT 地址区最好来自调试 registry；不要仅凭“匿名 RX/RWX 映射”或某条原子访存指令就断定是 FEX。发生真正 host 崩溃时，native 栈与 fault address 仍应优先保留。

### 3.3 找到 FEX frame

对本次提交的普通 ARM64 backend，`STATE=x28` 是源码确认的事实，见 [Arm64Emitter.h:33](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.h#L33)。**仅在确认处于相应 JIT 上下文后**，可以手动读：

```text
image lookup -t FEXCore::Core::CpuStateFrame
memory region $x28
expression --allow-jit false -- (FEXCore::Core::CpuStateFrame*)$x28
expression --allow-jit false -- ((FEXCore::Core::CpuStateFrame*)$x28)->State
expression --allow-jit false -- ((FEXCore::Core::CpuStateFrame*)$x28)->Thread
```

这些表达式只做类型转换/取值；`--allow-jit false` 用来阻止不支持解释的表达式回退到目标内执行。它不是对任意表达式的“只读保证”，不要加入赋值、函数调用或有副作用的 formatter。

如果停在 HLE，优先读取该 C++ frame 中传入的 `Frame/Thread` 参数，或宿主维护的 guest thread registry。参考 fork 有 [ActiveFexExecution TLS:74](https://github.com/zenithblue-oss/shadps4-arm64/blob/be6bc2e9c60799e071dd2fafa6216e8d80ec619c/src/core/fex/fex_guest_engine.cpp#L74)，可以辅助关联，但 TLS 符号解析、生命周期和优化可见性都需验证；不把它当最终稳定接口。

### 3.4 同一 LLDB 会话中看 guest 反汇编

先获得可信的 guest RIP，再指定反汇编架构：

```text
disassemble --arch x86_64 --start-address <guest_rip> --count 12 --bytes
memory read --format x --size 1 --count 32 <guest_rip>
memory read --format x --size 8 --count 32 <guest_rsp>
```

**本次做了一个本地实际验证：** 用 `lldb-1700.0.9.46` 打开 ARM64 Mach-O 测试文件，其中放置已知 x86 字节；未启动进程，仅从文件读取，执行：

```text
disassemble --arch x86_64 --start-address &guest_probe_code --count 4 --bytes
```

正确输出 `48 89 d8 → movq %rbx,%rax`、`48 83 c0 01 → addq $1,%rax`、`cc → int3`、`c3 → retq`。说明该 LLDB 构建确实支持 ARM64 target 上的 x86 cross-disassembly。**这不是 Android 远程读取/FEX 状态验证。** [验证记录与复现源码](data/lldb-cross-disassembly-2026-09-07.txt)。

同一版本若额外指定 `--flavor intel` 会因 target 仍为 ARM64 而报错；去掉该选项即可。这一具体差异说明操作指南需要固定客户端版本，而不是直接照搬 GDB 命令。使用的 Android NDK/Studio LLDB 仍需复验 `help disassemble` 和上述 probe。

不要把整个 attached target 改成 x86-64：host 的 register context、unwind 和实际执行仍是 ARM64。cross-disassembly 只切换解码方式。

## 4. 从 host PC 反查 guest RIP：可以在 LLDB 侧离线完成

当前 Core 的算法在 [RestoreRIPFromHostPC:144](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/Core.cpp#L144)，结构在 [CPUBackend.h:66](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/CPUBackend.h#L66)。适合转成 host 侧只读脚本：

1. 读取 `CpuStateFrame.State.InlineJITBlockHeader`，得到 `block_begin`。
2. 读 `JITCodeHeader.OffsetToBlockTail`，定位 `JITCodeTail`。
3. 验证 header/tail 在已知、仍有效的 code buffer 内，且 host PC 落在 `[block_begin, block_begin + Size)`。
4. 以 `Tail.RIP` 和 `block_begin` 为初始 guest/host 地址，从 `OffsetToRIPEntries` 读取 `NumberOfRIPEntries` 个映射项。
5. 按当前提交的 `vl64pair` 格式解码、累加 host/guest delta，找到未超过 host PC 的最后一项。
6. 无法完成时只报告 block RIP 或保存的 RIP，并注明 fallback；不要无条件报告精确 RIP。

编码源：[variable_length_integer.h:194](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Utils/variable_length_integer.h#L194)。存在多种长度和有符号 delta，不能按固定大小数组解析。

**无需在停止的 app 内调用 `RestoreRIPFromHostPC()`。** LLDB Python 使用 `SBProcess.ReadMemory` 等 API 读取映射，在主机端重建，避免让崩溃进程执行 C++。结构偏移从匹配 DWARF 或构建时导出的版本描述读取；未知版本拒绝套用，尤其不能照抄旧 wiki 的 184 偏移。当前 `InlineJITBlockHeader` 已在 CPUState 的首字段。[LLDB Python API](https://lldb.llvm.org/python_api.html)。

如果断点落在刚建立 header、正在切换 block 的少数指令中，元数据可能尚未更新完。插件必须识别过渡区，保留 host PC 和原始块信息。host→guest 映射仍不保证所有架构状态均处于该指令执行前。

## 5. guest GPR、flags、SIMD 怎样读取

### 5.1 GPR：当前固定分配能让手工诊断立即有用

本次提交普通 ARM64、x86-64 guest 的 SRA 表如下。仅对相应 JIT 稳定区域适用，**不是永久 ABI，也不是所有 host 架构通用表**。

| Guest | Host | Guest | Host | Guest | Host | Guest | Host |
|---|---|---|---|---|---|---|---|
| RAX | x4 | RCX | x7 | RDX | x5 | RBX | x6 |
| RSP | x8 | RBP | x9 | RSI | x10 | RDI | x11 |
| R8 | x12 | R9 | x13 | R10 | x14 | R11 | x15 |
| R12 | x16 | R13 | x17 | R14 | x19 | R15 | x29 |

证据：[SRA:46](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.cpp#L46)、[guest 枚举:10](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/include/FEXCore/Core/X86Enums.h#L10)。例如 JIT 中的 guest 栈通常从 x8 找，LLDB 的 host `$sp` 并不是 guest RSP。

可直接 `register read x4 x7 x5 x6 x8 x9 x10 x11 x12 x13 x14 x15 x16 x17 x19 x29` 保存原始值。但优化指令中间的 SRA 值也可能已经部分更新；需与停止精度一起展示。

工程化时优先读取已生成的 `SignalDelegatorConfig.SRAGPRMapping/SRAFPRMapping`，它们来自实际 dispatcher 配置，见 [MakeSignalDelegatorConfig:2646](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp#L2646)。在稳定边界已 spill 的寄存器读 CPUState；inline syscall/部分 spill 的混合场景先标记不完整，随后按实际 spill 阶段扩展，不能只看一张映射表。

### 5.2 flags：不能直接复制 ARM64 CPSR

FEX 重建逻辑使用 host NZCV、PF/AF 原始值、CPUState 中的 TF/DF 等。当前普通 ARM64 的 PF/AF 暂存于 x26/x27；CF 在内部表示中还有取反。见 [Core.cpp:185](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/Core.cpp#L185)、[Arm64Emitter.h:44](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.h#L44)。

这里有一个特别容易忽略的事实：`ReconstructCompactedEFLAGS(Thread, true, …)` 会先把 host PF/AF 写回 `Frame->State`。名称叫“重建”，但并不是纯只读函数。因此，LLDB 的 guest-view 脚本应该在主机副本上复现该版本的纯计算，不要用 `expression` 调用它。

### 5.3 SIMD：SVE256 与分离高半两种布局

普通 x64 SRA 将 16 个 SIMD 低半映射到 v16–v31。没有 SVE256 的 AVX 路径把 YMM 高半放在 `State.avx_high`；有 SVE256 时为另一种合并布局，并需要获得对应 SVE 状态。[SRAFPR](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.cpp#L87)、[ReconstructXMMRegisters:258](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/Core.cpp#L258)。

停止位置和 feature 配置共同决定读取来源。远程 LLDB/server 没有提供某段寄存器时，显示 unavailable；不能用 128 位值补零冒充完整 YMM。x87、MXCSR、FS/GS base 同样按 FEX/PS4 的实现读取，不与 host FPCR/FPSR/TLS 寄存器简单等同。

## 6. 这种方式能做到断点与单步吗

### 6.1 几乎不改 Core 就能用的部分

可以在 native HLE 函数、FEX compile 入口、异常处理函数设置普通 LLDB host 断点：例如观察某个 HLE 的 guest 参数、返回值、TLS、callback，或在新 block 编译时检查 guest RIP。

但 **编译一次与执行一次不同**。在 `CompileBlock` 上按 guest RIP 设条件断点，只能捕获编译事件；旧缓存命中和 block 直链不会再次经过它。它适合追查生成代码，不等于在该 guest 地址设置执行断点。

已知某一 guest 指令对应的当前 host JIT 地址时，也可临时在该 host 地址断住分析 ARM64 指令。需要追踪 code buffer generation、失效/释放、同一 guest 地址的多个翻译版本；缓存重建后旧 host 地址不能继续被当作同一逻辑断点。

### 6.2 不可以直接照常用的命令

- `breakpoint set --address <guest_va>` 在 ARM64 target 下不是 FEX guest 断点。host debugger 会按 host 断点机制处理该地址；若修改 guest 字节，可能破坏 x86 指令，而 CPU 实际执行的还是别处的 JIT block。
- LLDB `thread step-inst` 走的是一条 ARM64 指令，不是一条 x86 指令。把它循环到 mapped guest RIP 改变也不能保证正确 guest 一步，遇到 REP、自跳转、call/HLE 会有歧义。
- 在 JIT 中途修改 `CPUState.gregs` 后继续，值可能被 live host 寄存器覆盖；直接写对应 host 寄存器，又可能破坏当前 guest 指令执行到一半的内部状态。

这些限制不会因为换 LLDB 命令包装而消失。

### 6.3 更可行的 H1：让 guest 在一致边界调用 host 调试钩子

建议在 shadPS4 adapter 中增加一个明确的 native 调试停点，例如概念上的 `GuestDebugStopHook(snapshot)`：

1. guest 地址断点/单步完成/指定 HLE 事件由 guest 执行层判断。
2. 完成必要 spill，建立可读写的架构快照和 continuation，退出到受控 native 边界。
3. 调用保留符号的 native hook；LLDB 在该函数设置普通 host 断点。
4. LLDB 读取稳定 snapshot，可通过受限命令修改待提交寄存器或设置下一步请求。
5. LLDB continue 后，由 hook/adapter 校验并提交操作，再让 owner thread 进入 FEX。

这样，LLDB 只需要处理它擅长的 ARM64/C++ 函数断点，guest 的语义由我们掌握。首版甚至无需网络 RSP；但仍需要 [guest debugger 分析中的 D1/D2 执行控制](fex-guest-debugger-feasibility.md)。

hook 中用于修改的快照必须与只读 crash snapshot 区分，并有停止 epoch；不能让脚本直接改任意历史 CPUState。调试函数断点还会触发 host 全进程暂停，这是交互调试状态，不用于 VR 实时测量。

## 7. 以低侵入方式工程化：推荐的 LLDB 插件范围

建议开发一个放在 shadPS4 工具目录中的 host-side Python 扩展，首版命令语义如下；**这些是待实现接口，本次没有声称已提供插件**。

| 拟定命令 | 首版职责 |
|---|---|
| `fex status` | FEX/shad Build ID、schema、host page、stop 分类、是否可重建 |
| `fex threads` | host TID ↔ guest TID、当前 FEX frame、HLE/callback 层级 |
| `fex pc` | host PC、block RIP、mapped RIP、模块+offset、精度 |
| `fex regs` | raw 与重建 GPR/flags/SIMD，逐项标明来源和有效性 |
| `fex disasm` | guest 字节和 x86 解码，并列当前 ARM64 片段 |
| `fex bt` | 有依据的 guest unwind / 候选栈，关联 native HLE 栈 |
| `fex dump` | 保存原始寄存器、CPUState、code-map、代码/栈片段和模块表 |

首版仅用 SBFrame/SBValue/SBProcess 的读接口，所有分析在主机执行。LLDB 提供寄存器、类型、内存和线程访问基础，但 FEX 的识别与重建规则需要我们实现。[SBFrame](https://lldb.llvm.org/python_api/lldb.SBFrame.html)。

最值得在 shadPS4 加的辅助结构是一个 **有版本的、无需执行目标代码即可读取的 DebugRegistry**：线程 ID、current frame、状态阶段、signal 原始 context、callback depth、模块表、JIT 区间/generation、SRA 映射、停止事件。使发布与回收具有可识别的世代；在读取时若遇到发布中途或字段不完整，保留原始快照并标记未知。

它能让 LLDB 插件不依赖 STL 私有布局、某个 TLS 符号、固定 x28 或每次变化的结构偏移。早期可从 DWARF 手工读，registry 是将这条非常规路线变成可维护工具的关键。

## 8. 两个仍需单独处理的问题

**信号。** FEX wiki 的 `SIGBUS/SIGILL/SIG63` 配置是 Linux FEX 示例，Android 不能整套照搬。先 `process handle` 查看策略；对已确认由 FEX 消化的内部事件，可以用 `process handle -s false -n false -p true <signal>` 透传。但 SIGBUS 也可能是真 bug，SIGILL/SIGSEGV 可能承担多种含义，SIGTRAP 还涉及 LLDB 自身断点。按事件来源分类，不能仅按信号编号全部忽略。常规只读 host 断住不会让 guest signal handler 自动先运行；现场应明确是投递前还是处理后。

**JIT 符号插件。** FEX 虽有 `__jit_debug_register_code`，其 [GDBJIT.cpp:63](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/GDBJIT/GDBJIT.cpp#L63) 写的是自定义 `info_t/blocks_t` 数据，配合 [FEXGDBReader](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/Source/Tools/FEXGDBReader/FEXGDBReader.cpp#L1) 解码。不能由“LLDB 支持某类 JIT 调试接口”推断它会自动解析这个 payload；reader 的 guest unwind 本身也未完成。首版用我们的 code-map/registry 做 guest 关联；未来再考虑 LLDB 专用符号适配或标准对象格式的 JIT 描述。

## 9. 下一步顺序与验收

建议把调试投入改为：

1. **H0 先行**：在现有 ARM64/FEX harness 的 native HLE 边界与受控 JIT fault 处用 LLDB 停住；对照已知 guest 指令和寄存器哨兵，做可信只读视图。可以先在已有 Linux ARM64 路径验证，再迁移 Android；后者必须补做 16 KiB、bionic 和远程寄存器验证。
2. **H1 接续**：实现统一 guest 停止原因和边界快照，通过 native hook 提供软件断点、一步、修改状态后继续。多线程/HLE 回调测试沿用 D1/D2 门槛。
3. **H2 后接**：只有确实需要 IDA/GDB 原生 x86 调试体验时才接 RSP；复用已有 registry、state codec、breakpoint 与执行控制，避免维护两套互不一致的 guest 状态解释。

H0 的最小验收应覆盖：在 JIT 中、HLE 中、signal handler 中停住，各自找到正确 guest 线程；冷/热 block 的 RIP 映射一致；每个 GPR 和 SIMD 高低半读取正确；未知布局/不可重建状态明确失败；一次 snapshot 导出不调用目标函数、不改变目标内存。之后才给 H1 开放写与恢复。

本次已完成：源码路径核对、LLDB 命令帮助核对、ARM64 target 上 x86 cross-disassembly 的本地静态验证。尚未完成：设备 attach、FEX 现场寄存器重建、LLDB 插件、native debug hook，以及可恢复 guest 单步。

**对当前项目，先做 LLDB guest-view 的性价比高；但应从一开始共享 guest 调试状态契约，避免后续迁移到标准 debugger 时推倒重来。**
