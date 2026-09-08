# FEX 异步停止路径的源码证明（G1 前置）

日期：2026-09-08。FEX pin `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`。

[二周目 spec](../specs/android-fex-round2.md) §3.1 要求：接 `RequestInterrupt`/`WaitStopped` 之前，先在固定版本源码上写明所选 kick 的调用链、host 寄存器 spill、出口 ABI、嵌套 signal 与清理规则。本文件是该证明。**这里记录的是 FEX 已有的机制，不是本仓已实现的功能。**

## 1. 为什么必须走异步 kick

owner 线程进入 `ExecuteThread` 后就在 JIT 里，worker command queue 不会被轮询。一个无 HLE、无 syscall、自跳转的 x86 loop 永远不会自然退出，所以"停止"只能由**外部信号打断**，不能靠协作式检查。

## 2. 完整调用链（全部在 pinned 源码中）

以 Linux frontend 为参照实现，五步：

| 步 | 位置 | 作用 |
|---|---|---|
| 1 | [ThreadManager.cpp:315](../../references/FEX/Source/Tools/LinuxEmulation/LinuxSyscalls/ThreadManager.cpp) | `SignalDelegation->SignalThread(Thread, SignalEvent::Pause)` |
| 2 | [SignalDelegator.cpp:519-522](../../references/FEX/Source/Tools/LinuxEmulation/LinuxSyscalls/SignalDelegator.cpp) | 置 `SignalReason`，然后 `tgkill(PID, TID, SIGNAL_FOR_PAUSE)` |
| 3 | [SignalDelegator.cpp:439-467](../../references/FEX/Source/Tools/LinuxEmulation/LinuxSyscalls/SignalDelegator.cpp) | handler 内 `StoreThreadState`，**改写 ucontext 的 PC** |
| 4 | [Dispatcher.cpp:436-455](../../references/FEX/FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp) | 被改写到的 JIT 代码 spill SRA，调用 `SleepThread` |
| 5 | [Dispatcher.cpp:33](../../references/FEX/FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp) | `SleepThread` 转发给 **`CTX->SyscallHandler->SleepThread`** |

关键点在第 3 步：handler **不做** `siglongjmp`，而是修改 ucontext 的 PC，让 signal 返回时落到 JIT 里一段专门的 spill 代码。spec 明确禁止把 `siglongjmp` 跨 JIT 当停止实现——FEX 自己也没这么做。

### 2.1 spill 地址二选一

`SignalDelegator.cpp:447-457` 按 PC 位置分流：

```cpp
if (CTX->IsAddressInCodeBuffer(Thread, GetPc(ucontext))) {
    SetPc(ucontext, Config.ThreadPauseHandlerAddressSpillSRA);  // 在 JIT 里，SRA 未 spill
} else {
    SetPc(ucontext, Config.ThreadPauseHandlerAddress);          // 不在 JIT，SRA 已 spill
}
SetState(ucontext, reinterpret_cast<uint64_t>(Frame));
```

两个地址相差正好一条 `SpillStaticRegs`（Dispatcher.cpp:436-439）。**选错会破坏寄存器状态**：在 JIT 中却跳到不 spill 的入口，guest 寄存器仍在 host 物理寄存器里，之后的 `SleepThread` 与快照读到的 CPUState 是陈旧值。`IsAddressInCodeBuffer` 是 [Context.h:159](../../references/FEX/FEXCore/include/FEXCore/Core/Context.h) 的公开纯虚函数，embedder 可以直接调用。

这条正是[既有 LLDB 工作流文档](fex-lldb-host-guest-workflow.md)所说"host stop 不等于 guest 指令边界"的机制来源：只有经过 spill 路径停下的线程，CPUState 才是完整的。

### 2.2 出口 ABI 与恢复

- spill 之后 JIT 调用 `SleepThread(CTX, Frame)`；返回后执行 `PauseReturnInstruction`（`hlt(0)`，Dispatcher.cpp:453-455）。
- 该 `hlt` 再次进入 signal handler，命中 `SignalDelegator.cpp:427-434` 的 `PauseReturnInstruction` 分支，`RestoreThreadState(..., TYPE_PAUSE)` 恢复并继续执行。
- 因此"恢复"不是从 `SleepThread` 直接 return 回 guest，而是**经过第二次 fault**。实现必须允许这一次额外的同步 signal，不能把它当异常。
- `SignalHandlerRefCounter` 在暂停时 `++`、恢复时 `--`（SignalDelegator.cpp:431、464）。FEX 用它判断"是否可以安全清 cache"。停止期间做失效必须尊重这个计数。

## 3. 对本仓的直接后果

**`FexSyscallHandler` 目前没有覆写 `SleepThread`。** FEX 的声明是带空实现的虚函数：

```cpp
// FEXCore/include/FEXCore/HLE/SyscallHandler.h:48
virtual void SleepThread(FEXCore::Context::Context* CTX, FEXCore::Core::CpuStateFrame* Frame) {}
```

所以按当前代码发一个 pause 信号，会正确 spill、调用空的 `SleepThread`、立刻返回、`hlt` 恢复——**线程根本不会停**。这不是"接上就能用"，等待逻辑必须由本仓在这个覆写点实现。

好消息是不需要改 FEX：spill 入口、`IsAddressInCodeBuffer`、`SleepThread` 覆写点都是公开 API。**G1 不需要动 FEX 子仓。**

## 4. 实现约束（写代码前先固定）

1. **handler 内不得**分配内存、取普通 mutex、调用 Foundation、做复杂日志。只允许原子读写、`SetPc`/`SetState`、以及 async-signal-safe 调用。等待发生在 `SleepThread` 里，那已经是普通线程上下文。
2. **信号归属**按实际 native TID + generation + PC 判定。不能把所有 SIGSEGV 当 guest fault——app 内 ART 会产生自己的信号。
3. **previous handler** 的安装、转发和恢复要记录；ART 已经装了自己的处理器。
4. **Android 无 `SIGNAL_FOR_PAUSE=63` 的现成约定**。63 在 bionic 是合法的 RT 信号，但要确认不与 ART 冲突后再固定，并写进锁文件。
5. `SleepThread` 中的等待必须可取消，且 ack 只能在 spill 完成、CPUState 已发布之后发出——这正是 `stop_epoch` 与"请求已收到"必须分开的原因。

## 5. 本文件不主张什么

- 没有在 Android 上实测过这条路径。上面全部是 pinned 源码阅读结论。
- 没有验证 Android/bionic 的信号投递、ART 共存或嵌套信号行为。
- `SIGNAL_FOR_PAUSE` 的取值在本仓尚未选定。
- 不构成 T02/T03 的任何验收证据。
