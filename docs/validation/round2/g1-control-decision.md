# G1 修正设计：在 JIT 入口停止并返回 owner

2026-09-09。取代 `b3bfabce` 的异步 PC 备份 / SleepThread 停驻方案。实现决定与验证分离：[修复报告](g1-repair-2026-09-09.md)和[原始证据](g1-2026-09-09/README.md)。

复核探针已发现：停驻 Cancel 不唤醒、Cancel 返回没有 ack、旧 epoch Resume 被接受、恢复后旧 ticket 仍读取可变 CPUState、同一回执每读一次就增加 stop_epoch。原 70/70 未覆盖这些路径。G14 也没有检查 B 的实际进度。

固定 FEX `385a0cc4d…` 提供另一个无需改子仓的入口：

- `FEXCore/Source/Interface/Core/Core.cpp` 在 `Config.GdbServer` 时开启 `NeedsPendingInterruptFaultCheck`。在 Core-only 链接范围，此配置只启用检查，不创建网络服务。
- `JIT/JIT.cpp` 的 `EmitEntryPoint` 在 guest 指令之前发出对该线程 `InterruptFaultPage` 的 store；`EmitSuspendInterruptCheck` 也用于 multiblock 后向分支。
- 此验证 backend 明确关闭 MULTIBLOCK，并按固定 header/ARM64 store 编码，只接受该 block 的第一个 probe 为入口安全点。IR 内部后向边（如 REP）即使在此配置下也可能发出 probe；内部 probe 仅跳过，避免把部分完成的指令作为可写边界。识别采用有界扫描，布局变化时不能猜测安全点。
- controller 在锁内将线程的独占 fault page 设为 PROT_NONE；新 Run 的 pending 检查与登记执行状态使用同一把锁，消除入 JIT 的丢请求窗口。
- SIGSEGV 只认当前 owner binding 的独占 fault page / ACCERR。JIT 入口处保存 bounded host GPR/PSTATE 和 block entry RIP，改 SP/PC 到既有 stop-spill stub；stub 正常返回 ExecuteThread。没有堆栈 backup、第二次 SIGILL、SleepThread 停驻、异步跨 C++ 锁栈跳转。
- FEX dispatcher/DeferredSignalRefCountGuard 也访问该页；这些 host 清理检查不作为可写 guest 安全点，仅跳过该探测 store，保持页面保护，直到 JIT entry。
- owner 在正常 Run 栈恢复 fault page、整理 flags/RIP、发布 immutable snapshot/stop epoch，再返回 PauseRequested 或 Cancelled。Run 尾部释放 execution lease；回执不等同于 mapping transaction token，G2 仍须等待 lease 实际排空。控制者 Resume 仅解除对应暂停的准入，owner 随后再次 Run；owner 可在两次 Run 之间 WriteRegisters。
- 每个 Run 安装/恢复 owner 的 alt signal stack，handler 无日志、锁和分配；与 ART 的完整共存仍归 G4 app 验证。

锁规则：所有请求、thread running 状态、ack 和 Resume 在 context mutex 下线性化；WaitStopped 的 condition_variable 等待释放这把锁。handler 只操作本次 Run 的固定 binding。快照由 owner 发布，WaitStopped 仅复制相应 stop record，不读取运行中的 FEX state，也不创建新 stop epoch。

代价和边界：开启入口检查并关闭 multiblock 是当前验证 backend 的明确策略，未做性能结论；不启用 GDB server、单步、透明 SMC 或软件 MMU。不可将新的正常 Stop 直接应用于未来任意不可取消 HLE；G3 必须使用自己的有界取消协议。

已经停止的线程收到新请求时，controller 可以用现有 owner 快照发布相同 stop_epoch 的新回执，不读取 mutable FEX state。不可恢复 fault 拒绝 Resume/Run/setter。FEX 的进程级 allocator 只初始化一次；该 pin 的 ClearHooks 故意保留全局 arena，不作为 per-context 回收方法。

浮点进入/退出：WriteRegisters 修改 mxcsr 并不改变 host FPCR。Run 进入时根据该线程 mxcsr 的 rounding/FTZ 建立 guest FPCR，映射遵循 pinned FEX `JIT/MiscOps.cpp::SetRoundingMode`；AFP/FIZ 由 FEX `Arm64Emitter::FillSpecialRegs` 处理。ExecuteThread 返回后恢复完整 host fenv。已用负数转换和相反的 host 舍入方向覆盖四种模式；x87 全语义和任意异常精确性不由本测试代表。G3 还须在 HLE/callback 边界独立保存恢复 FP 环境。
