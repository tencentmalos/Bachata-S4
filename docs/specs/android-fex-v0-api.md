# V0 CPU API 契约：Dynarmic-like 的组织，x86-64 的语义

所属：[V0 总 spec](android-fex-v0.md)。本文件是**待实现契约**。接口名称可小幅调整，所有权、状态、错误、并发和可验证行为不得靠空实现替代。

## 1. “Dynarmic-like”的准确含义

参考 Dynarmic 的 `UserConfig/UserCallbacks`、`Jit::Run/Step/HaltExecution`、寄存器访问和代码失效组织。借鉴的是调用方体验与职责边界：

| Dynarmic 概念 | 本 API 对应 | 不照搬的部分 |
|---|---|---|
| Jit + Config | CpuContext + CpuConfig | x86 特征、TSO、FS/GS、RFLAGS、MXCSR 自己定义 |
| Run / HaltExecution | Run / RequestInterrupt | 中断带 epoch，不能用无条件 ClearHalt 丢掉新请求 |
| Step | Step + 明确支持范围 | 一个 guest 指令不等于一个 JIT block |
| Get/SetRegisters | Read/WriteRegisters | 停止态及有效字段契约；不能随时复制 FEX CPUState |
| ClearCache / InvalidateCacheRange | Quiescence + Memory transaction / InvalidateCode | 跨线程、执行中旧 block、alias 和映射世代必须同步 |
| Memory callbacks/page table | GuestMemory + DirectMapped contract | V0 不保证普通 JIT loads/stores 调用 C++ Read/Write callbacks |
| Exception/SVC callbacks | GuestFault / HleCall / OrbisSyscall routing | Android syscall 与 PS4 guest syscall 不能透传混用 |

来源：[Dynarmic a64.h](https://github.com/tencentmalos/dynarmic/blob/a593d9262388e3216985b982e400f19ef9ce9749/src/dynarmic/interface/A64/a64.h)、[config.h](https://github.com/tencentmalos/dynarmic/blob/a593d9262388e3216985b982e400f19ef9ce9749/src/dynarmic/interface/A64/config.h)。V0 不是 Dynarmic 源码/API 的二进制兼容层。

## 2. 对象与寿命

- **CpuContext**：持有 backend、共享 code cache、配置和 GuestMemory 关联。V0 同一 app 只允许一个活跃 context；Stop/Destroy 后必须允许重建，第二个同时创建返回 AlreadyActive。
- **GuestAddressSpace / GuestMemory**：独立拥有 guest reservation、mapping、alias、权限与 publication generation；不是借用任意 host 指针的无主表。
- **ThreadHandle**：opaque ID + generation，绑定 context 与 owner host thread；创建和销毁在 owner 上完成，后续 stale handle 操作返回 InvalidHandle。
- **InvocationId**：每次 Run/InvokeGuest 的序号；nested callback 保存父序号与深度，禁止复用单个全局 return PC。
- **StopEpoch / QuiescenceToken**：说明哪些 guest threads 在什么 epoch 停止、哪些 HLE writers 已退出；用于内存事务与可写快照。
- **AndroidSession**：拥有 workers、CPU context、Java/native window 和事件队列；JNI 暴露 session handle，不暴露 FEX 指针。

Context 必须晚于 Threads 销毁，GuestMemory 必须晚于 Context 的所有执行引用释放。销毁失败返回错误/诊断，不由析构器从任意线程执行跨线程 Run、隐式杀进程或忽略未释放对象。

## 3. 核心类型

类型布局要求，具体 C++ 声明由执行者实现：

| 类型 | 至少包含 |
|---|---|
| CpuConfig | api_version、guest ISA/profile、内存模式、host page size（从平台发现）、内存顺序策略、event sink、bounded diagnostics 配置 |
| BackendCapabilities | FEX upstream + downstream revision、guest features/CPUID profile、Step 支持范围、DirectMappedOnly、state schema、允许的 stop/恢复能力 |
| RegisterFile | 16 个有明确枚举编号的 GPR、RIP、RFLAGS、FSBase/GSBase、MXCSR、16×128-bit XMM；额外扩展状态及 validity mask |
| ThreadInit | 初始寄存器、guest stack/code 的合法映射引用、guest TID；不是任意 host function pointer |
| RunResult | primary_reason、pending_reason_bits、thread/invocation/stop epoch、可信 guest PC、snapshot、可选 fault/step 信息 |
| GuestFault | x86 vector/类型（可用时）、guest RIP 与 fault VA 的有效位、访问类型、host signal/si_code/PC、恢复策略 |
| CpuSnapshot | schema、thread/generation、capture kind、validity mask、寄存器、mapping/code generation、host PC（可用时） |
| Error | 类别、操作、thread/epoch、errno 或 backend code、结构化短说明；禁止只返回 bool 吞掉原因 |

GPR 使用公开枚举 `RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8…R15`，不能暴露 FEX 的内部数组下标。RSP 只有一个权威槽。snapshot/trace 的序列化写明字节序与版本，不序列化带 padding 的 C++ struct 原始字节。

最小 guest profile 必须包含 fixture 使用的整数和 SSE2。XMM 始终有效；如果暴露 AVX，则 YMM high、AVX CPUID/XCR0 语义和返回恢复也必须实现。未暴露的扩展返回 Unsupported，不以全零数组伪装有效。x87 等未验收状态标为 invalid；不得声称快照能用于完整 PS4 savestate。

## 4. 接口与线程约束

伪签名仅表达契约，不是已存在或可直接编译的头文件：

```cpp
Result<Context> CreateContext(CpuConfig, GuestMemory&);
BackendCapabilities QueryCapabilities(const Context&);

Result<ThreadHandle> CreateThread(Context&, ThreadInit);   // owner
Result<RunResult> Run(ThreadHandle, RunOptions);           // owner, blocking
Result<RunResult> Step(ThreadHandle, StepOptions);         // owner, limited V0 scope
Result<InterruptTicket> RequestInterrupt(ThreadHandle, InterruptReason); // cross-thread
Result<StopReceipt> WaitStopped(InterruptTicket, Timeout); // controller

Result<CpuSnapshot> ReadRegisters(ThreadHandle);           // stopped boundary
Result<void> WriteRegisters(ThreadHandle, RegisterPatch, StopEpoch); // owner + stopped
Result<CallResult> InvokeGuest(HleScope&, GuestCodePtr, GuestCallFrame); // owner
Result<QuiescenceToken> Quiesce(Context&, Timeout);         // controller
Result<void> InvalidateCode(QuiescenceToken&, GuestRange, InvalidationReason);
Result<void> ClearCodeCache(QuiescenceToken&);

Result<void> DestroyThread(ThreadHandle);                 // owner + no invocation
Result<void> DestroyContext(Context&);                    // all threads destroyed
```

配置和 Context 初始化可由 session controller 编排，但 owner 相关操作必须通过 worker command queue 调度。JNI/main thread 不能直接 Run，也不能给正在 Run 的 Thread 调 SetRegisters。

| 状态 | Run/Step | ReadRegisters | WriteRegisters / mapping 修改 |
|---|---|---|---|
| Ready / Stopped（可信边界） | owner 可执行 | 可读取已发布快照 | owner/事务持有者可修改，检查 epoch |
| InJit | 返回 AlreadyRunning | 返回 Busy；可另取标为 LastSafePoint 的旧快照 | 禁止 |
| InHle / InCallback | 禁止递归 Run | 当前 HleScope 的边界记录可只读 | 禁止任意改 RIP/栈；仅允许规定的 callback API |
| WaitingHle | 不可从另一线程重入 | 只读 HLE 边界快照 | 必须取消/完成等待并到达可写停止态 |
| Faulted | 按明确恢复策略；V0 不要求所有 fault 可恢复 | 仅返回有 validity 标注的快照 | 不可盲目修改再继续 |
| Destroyed | InvalidHandle | InvalidHandle | InvalidHandle |

事件在正常线程上下文发布。event sink 不可同步重入 Run/Destroy；异步请求进 command queue。等待控制操作不得持有 CPU/HLE 内存锁，也不得在自己等待退出的 owner/callback 上调用阻塞 Quiesce。

## 5. Run、停止与竞态语义

- Run 同步执行 guest，返回只发生在定义好的边界。普通 fixture return 必须命中已登记 return gate；真实 guest HLT/非法 opcode 不当作正常 Returned。
- Stop reasons 至少区分 Returned、PauseRequested、Cancelled、StepComplete、HleBoundary、GuestFault、Unsupported、BackendFailure。
- GuestFault/BackendFailure 不能被同时到达的 Pause 隐藏；取消与暂停同时存在优先 Cancelled；其他 pending bits 保留。
- RequestInterrupt 是唯一必须支持的执行中跨线程 CPU 操作。ticket 含递增 epoch；Acknowledgement 必须在 owner 已退出 JIT、状态已发布且不会继续写 guest state 后发生。
- 未处理的请求不能因一次 Resume 清掉。Run 消费指定已确认的 epoch；更晚到达的请求仍有效。
- Runaway x86 loop 无 HLE/无显式 yield 时也必须响应。实现可用 dispatcher 检查、FEX interrupt 机制等，但要完成 Android signals 和 16 KiB 独占 interrupt page 的审计。
- 没有精确 instruction budget 时不得把 block 数量称为 instruction count；V0 不要求生产 Run 实现精确指令配额。
- 阻塞 HLE fixture 使用可取消 wait；取消必须唤醒或使 owner 在有界时间回到安全点，不做跨 C++ 栈 longjmp。
- 正常 Stop 先请求取消、等待 owner 完成 invocation/cleanup，再销毁。force-stop 是测试 supervisor 对失败的清理手段，永远不是 Stop 测试成功。
- 两个 guest threads 可同时运行；V0 不支持 owner 迁移和多 context 并发。Context 共享缓存必须有明确串行化/并发策略，不能从“一线程验证通过”推断多线程安全。

## 6. Step 的 V0 边界

V0 必须实现有限且真实的一指令 Step：

- 必须覆盖：普通整数运算、taken/not-taken 分支与自跳转、普通 load/store、基础 SSE2 fixture。
- 结果是完成一条 guest 架构指令、或该条指令的真实 fault。RIP 没变不代表未执行；不能用“运行一个 block”代替。
- HLE callgate、REP/string、系统/异常控制指令及未支持类别，可在执行前返回 HleBoundary/UnsupportedStep，guest 状态和内存不变；必须记录支持范围并有负例测试。
- guest 自己设置的 TF 与 debugger stepping intent 分开。若借助 FEX TF 路径，证明普通已支持指令没有可观察状态污染；POPFQ/IRET 等未覆盖语义必须在执行前拒绝，不可执行后粗暴恢复旧 flags。
- 暂不要求自动软件断点重插、step-over、RSP server；也不能将上述有限 Step 宣称为通用 x86 debugger。
- 暂时无法实现 Step 时允许继续其他阶段，但最终状态不能为 V0_ACCEPTED。

## 7. GuestMemory 与代码发布

### 7.1 API 边界

必须提供 Map/Unmap/Protect、Read/Write、RegisterAlias、PublishCode/Invalidate、ValidateRange、AcquirePinnedSpan/Release 等功能语义。可以合并部分方法，但必须保持事务和所有权。

Read/Write 是 **HLE、loader、debugger 的显式访问路径**；DirectMapped JIT 的普通访存不保证走这些方法。V0 不提供虚假的通用 Read8/Write8 callback 模式；如果外部请求 `SoftwareCallbacks`/未支持 MMIO，初始化返回 UnsupportedMemoryMode。

Guest 地址与 guest code 地址分别为强类型。对齐、加法溢出、零长度、跨映射、权限和寿命均需校验。返回给 HLE 的 host span 有 pin/lease；guest unmap/保护修改不能与仍在使用的 HLE span 竞争。

### 7.2 失效事务

V0 可以采取昂贵但正确的 stop-the-world publication：

1. Quiesce 所有相关 guest owner，禁止新 invocation/线程进入；等待已开始的 HLE 写入结束。
2. 在 GuestMemory 事务中验证 range、permissions、alias/backing generation，实施写入/重映射。
3. 失效所有共享/每线程翻译缓存与对应 alias；ARM64 JIT publication 按需完成 I-cache 同步。
4. 更新 generation，发布可恢复状态，再解除 quiescence。

Invalidate 成功意味着之后恢复执行不会进入旧译码；不能只“已排队”。若仅获得不完整停止状态，事务返回 Timeout/Busy，不改权限或覆写映射。

这里的 CPU quiescence 只冻结已纳入的 guest/HLE writers，不自动冻结 GPU/音频/DMA。V0 的 GPU observer 是 test double；将来真实异步 writer 必须加入对应同步协议。

guest 自修改代码测试必须经过真实写入与失效：通过写保护 tracking 捕获后在安全路径处理，或使用明确的协作式 `PublishGuestCode` HLE 协议。**V0 允许后一种 ExplicitPublication 模式，但必须在 capabilities/report 中声明，并在初始化时拒绝要求 TransparentSMC 的 caller。** 这不等于已支持任意游戏的透明 SMC。host/HLE 写入和 alias publication 仍必须测试。

## 8. HLE / syscall / callback

- HLE registry 使用 typed signature、operation ID 与 guest veneer 区间。callgate 只能从登记入口进入；随机 guest syscall 不可按 RAX 当 operation ID 执行。
- V0 的真实 Orbis syscall 可统一返回 GuestFault/UnsupportedSyscall，记录编号与 PC，**不能执行 Android native syscall**。
- 固定 ABI fixture 必须覆盖 8 个整数参数（超过 6 个 GPR）、9 个 double 参数（超过 8 个 XMM）、混合整数/浮点、整数和 double 返回、callee-saved、guest stack 对齐/red zone，以及第 4 参数 RCX 不被 syscall callgate 破坏。
- 输入/输出 buffer 的长度和 guest 权限必须匹配；未登记 host 地址、过期 handle、越界 span 返回错误。V0 只接受支持集合中的显式签名；禁止任意 reinterpret_cast 调 host。
- guest→HLE→guest callback 只能通过当前 HleScope 的 InvokeGuest，同一 owner、受控重入，至少支持两层嵌套整数 callback；outer/inner 寄存器、返回点、TLS 和 invocation 都需正确恢复。
- V0 不要求任意线程发起异步 callback；发给非 owner 的请求须调度或明确拒绝，不能直接递归 FEX ExecuteThread。
- C++ exception 在 native HLE 边界捕获，转成失败/guest fault；正常栈展开不得跨过 JIT 非 C++ 帧。
- 单个 invocation 的取消不能泄漏到后续 session；Context 级关闭也不能提前释放仍在 callback 使用的 return gates。

## 9. 调试和 signal 适配

必须提供稳定的只读 DebugRegistry/快照格式，包含 native TID、guest TID、generation、invocation、InJit/InHle/Stopped 状态、最近安全点、guest mapping 基址和有效字段。

V0 提供一个可设 host breakpoint 的 native hook（例如 `OnGuestStopPublished`），以及 LLDB 使用示例或 Python helper，用于：

1. 列出 guest/native 线程对应；
2. 在安全点读取真实 guest 寄存器与内存；
3. 显示 guest RIP 附近 x86 bytes/反汇编；若所用 Android LLDB 不支持 cross-disassembly，使用导出的 bytes + 明确版本的离线反汇编器；
4. 关联 fault 的 host PC 与 guest 信息；无法精确重建时显示 Unknown/LastSafePoint，不能伪造完整状态。

异步 handler 只做设计允许的最小捕获与跳转，不分配内存、加一般 mutex、做 JNI、socket I/O 或复杂日志。与 ART/bionic/FEX 的 signal owner、altstack、信号 mask 和恢复时机形成明确表；不吞掉未归属自己的 signal，不把默认信号处置当普通 C 函数调用。

寄存器 getter 不得从调试器里执行有副作用的 inferior FEX reconstruction helper。内部正常执行路径若需有副作用的 spill/reconstruction，应先完成后发布不可变快照，LLDB 只读它。
