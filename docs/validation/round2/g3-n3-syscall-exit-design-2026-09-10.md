# N3 设计：syscall 错误后的安全立即退出原语（2026-09-10）

> 后续审计：本文仍是未验证候选。固定Dispatcher有非spill stop入口，两个stop入口都不自行恢复SP；G1是在signal handler中先改SP。wrapper连接点及C++正常返回边界尚未落实，不能据本文宣称出口能力已验证。当前按 [N1/N2复核§3](g3-n12-review-2026-09-10.md) 与 [N3分步实验](../../specs/android-fex-round2-g3-n12-fix-n3-probe.md) 执行。

状态：**设计 + 待最小探针验证**。依据：[R0 收尾/R1 spec](../../specs/android-fex-round2-g3-r0-exit-next.md) N3/N4、
[修复→H3 spec](../../specs/android-fex-round2-g3-repair-h3.md) R1。事实来自固定 FEX `385a0cc4d` 源码，不依赖行号猜布局。

## 1. 问题精确化

非 Windows 构建 `X86Tables::DEFAULT_SYSCALL_FLAGS = FLAGS_NO_OVERLAY`（**不含 `FLAGS_BLOCK_END`**）。因此
`OpcodeDispatcher::SyscallOp`（[OpcodeDispatcher.cpp:38](../../../references/FEX/FEXCore/Source/Interface/Core/OpcodeDispatcher.cpp#L38)）
在 `_Syscall()` 后**不**发 `ExitFunction`，guest 的 `syscall; mov [r12],42; …return-gate` 被编译进**同一个 JIT block**。

JIT 侧 `Arm64JITCore::Syscall`（BranchOps.cpp:279）顺序：
`PushDynamicRegs → SpillStaticRegs(GPR/FPR 全 spill) → 置 InSyscallInfo → blr SyscallHandlerFunc(HandleSyscall)
→ FillStaticRegs → 清 InSyscallInfo → PopDynamicRegs`，然后 block 继续执行后继的 sentinel store。

当前 `HandleSyscall` 对 unknown/rejected 只置 per-thread fault flag 然后**正常返回到 JIT 尾声**，于是后继
sentinel 执行；fault 直到这次 Run 的 block 循环最终退出才在 `BuildRunResultLocked` 变成 GuestFault，且
fault RIP 可能已到 return gate。探针实测 sentinel 被写 42。

## 2. 不能采用的路（spec 明确禁止）

- 改 `Frame->State.rip` 等下个 block：同 block 后继指令不重读 rip，无效。
- 设全局 `_WIN32` 强制 syscall 带 BLOCK_END：改全局构建状态，且改变所有 syscall 语义，禁止。
- MAXINST=1 / 逐指令模式：只能诊断，不能是生产解法。
- 从 HandleSyscall/native/allocator/持锁 scope 直接 `longjmp`/跨 JIT 抛异常：syscall handler 在 JIT `blr` 的
  host 帧里，长跳会跳过 FillStatic/PopDynamic 的栈与寄存器收尾、C++ 自动对象析构、frame 注册/lease/FP 恢复。
- 把失败 fixture 改成 `syscall; jmp` 或删除后继 store：篡改测试。

## 3. 候选出口（spec 方案 1 优先：主仓拥有的 ABI wrapper + 已证明 backend 出口）

handler（C++）不直接退出 JIT，而是：

1. **先完成全部 C++ 边界清理**：pin 释放、fenv 恢复、frame/binding 状态收尾、RAII 析构正常走，把 guest
   `State.rip` 设为 fault 发生 PC（syscall 那条），设置归属错误事件（N4）。
2. 在 handler **正常返回 JIT 之前**，用一个**主仓拥有的汇编出口 trampoline**，在确认无 C++ 清理责任后，
   复用 FEX 已存在的 stop 收尾（`SpillStaticRegs` 已在 syscall prologue 做过；出口需要 `ReturningStackLocation`
   恢复 host 栈、`PopCalleeSavedRegisters`、`ret` 回 `ExecuteThread` 的调用者），让 `context_->ExecuteThread(...)`
   像在停止点一样**返回到 Run 的 C++ 收尾**。

关键区别于 G1 的 InterruptFaultHandler：G1 是 SIGSEGV 在 block-entry probe、寄存器仍是"guest 跑了一半"的视图；
syscall 点的 guest 寄存器已经在 syscall prologue **全部 spill 到 CPUStateFrame**，并且 handler 已在 host C++ 帧里。
出口必须使用 syscall 点自己的栈帧关系（`ReturningStackLocation` 在 ExecuteDispatch prologue 记录，对应整个
ExecuteDispatch 的 callee-saved 保存区），而不是 block-entry probe 的 sp。

## 4. 待探针必须回答的不确定项

N3 spec 表格逐项，最小探针实测，不靠推断：

- **host 栈**：HandleSyscall 返回地址在 ExecuteDispatch 栈上的相对位置；从 syscall handler 帧能否安全到达
  ThreadStopHandlerAddressSpillSRA（它做 SpillStaticRegs? 还是假设已经 spilled？）。注意 syscall 已 spill 全部
  GPR/FPR，stop_spill 入口 `SpillStaticRegs` 再 spill 是否幂等/有害。
- **x28 / frame / LR**：syscall handler 里 STATE（x28）、callee-saved、LR 的实际值；出口如何把 sp 设回
  `ReturningStackLocation`、把 LR 设成 ExecuteDispatch 的返回点。
- **InSyscallInfo / callret 栈 / t_binding**：提前离开时 InSyscallInfo 仍非 0（FillStatic 之后才清），出口
  必须清零；callret_sp 未被 syscall 推进（与 callback 不同），保持；frame 注册、execution lease、fenv 由
  Run 的 C++ 收尾负责，出口只需让 ExecuteThread 返回。
- **guest rip**：fault RIP = syscall 指令地址（prologue 已存 `State.rip` 为 syscall PC，见 SyscallOp
  `NewRIP=GetRelocatedPC(Op,-InstSize)`），不要用返回后的 rip。
- **normal continuation 不受影响**：成功 HLE 走原 FillStatic/PopDynamic 返回，sentinel 必须执行（证明不是
  无条件停所有 syscall）。

## 5. 最小探针（out-of-tree，链接真实 FEX，先不动生产 HandleSyscall）

独立 ELF（复用复核的 review_probe 模式：真实 harness + 初始化 + canonical backend），三个观测，全部真实 FEX、
同一生产路径：

1. unknown operation：guest `syscall; mov qword[r12],42; …HLT return-gate`。期望 GuestFault、`[r12]` 保持 0、
   fault PC == syscall PC、Run 有界返回。
2. registered 但 buffer 拒绝：native call count 不增、pins 回基线、sentinel 保持 0、GuestFault。
3. 正常 registered 调用：后继 store **执行**、返回值正确——证明出口只在错误路径触发。
4. 错误后 guest 自跳 loop：无外部 Cancel，单次 ≤1 秒返回 owner。

探针先用于验证第 4 节的栈/寄存器事实（可在出口点 dump sp/LR/x28/InSyscallInfo/rip 与 ReturningStackLocation 的
关系），确认 trampoline 正确后再把出口移入主仓 `fex_context.cpp`，把 HandleSyscall 的 fault 分支接到它。

## 6. 出口失败时的保守路径

若实测证明在不修改只读 FEX pin 的前提下，没有能保证 C++ 析构 + 正确栈收尾的出口（例如 stop_spill 的栈假设与
syscall 点不兼容、且无主仓可安全设置的状态），则**不冒充完成**：提交最小失败探针、已核查的栈事实、候选 FEX
依赖改动（含子仓贡献约束），交 spec 流程决定，而不是退用 `_WIN32`/逐指令/长跳。

## 7. N3 通过判据（spec）

- 上述 1/2/3 各先单 owner 100 次，sentinel 行为/PC/count/pin/返回值全部正确。
- 错误后自跳 loop 100 次，无外部中断 ≤1 秒返回；owner host 栈/FP 恢复、线程可销毁重建。
- N1/N2 的 runner 记账与 G24 协议保持绿色。
- 之后进 N4（错误归属事件）与正式 G33+ 用例（同提交接 ownership/ROUND2/退出码负例）。
