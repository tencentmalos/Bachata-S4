# 二周目进度（2026-09-10 R0收尾完成：N1/N2 已过，N3 syscall退出原语进行中）

分支 `codex/android-fex-round2`，本地 HEAD `97f149af`。前序 G2 源码修复 `470109f6`，H0/H1/H2 分别是
`1f8c7a21` / `0e10defc` / `56cbc7b1`。G3 修复本地提交：`25189931`（R0 runner 归属+未映射 FAIL 安全网）、
`8eded461`（R0 G24 早期冻结取点，后被 N2 取代）、`0484a0b9`（进度）、`fd1c8a86`（**N1** runner 整体失败退出码）、
`911be847`（**N2** 确定性 Run-entry gate）、`97f149af`（runner ENOEXEC 健壮性）。origin 仍在 `0e10defc`，
**所有 H2/R0/N1/N2 提交仅本地、未 push**；推送后须核对实际 remote ref。FEX/Foundation pin 未改。

**当前进度：N1（runner 失败出口）、N2（G24 确定性握手）已完成并真机验证；N3（syscall 错误立即退出原语）正在进行；
H3 入口仍不接受。** 先读 [本轮复核](g3-r0-review-2026-09-10.md)、[R0收尾/R1 spec](../../specs/android-fex-round2-g3-r0-exit-next.md)；
完整后续 R2/R3/S0–S3 遵循 [修复→H3 spec](../../specs/android-fex-round2-g3-repair-h3.md)。

N1 已交付（`fd1c8a86`/`97f149af`）：

- runner 在决定退出码前先算 R2 报告，新增统一 `overall`（唯一失败 sub 去重、V0/R2 失败父项、未映射 sub、
  失败 suite）；任何 V0/R2/unmapped/crashed-suite 失败都使 runner **非零退出**。G30–G32 各项 FAIL+exit0
  此前 R2 JSON 记 FAIL 却返回 0，现已返回 1。V0 `summary.failed` 保留 V0 范围。
- 失败/超时 suite 单独入 `failed_suites`；missing/not-started/all-SKIP/clean partial 继续 exit 0、NOT_RUN。
- 自测 `run_runner` 不再丢 returncode；accounting 19/19、python 18/18，含真实子进程返回码与逐项 G30–G32
  FAIL/重复冲突/missing-SKIP/crash/timeout/Z99/clean 矩阵。
- host `run_suite` 对跨架构二进制（arm64 误放 host 目录）的 OSError(ENOEXEC) 改为 never-started，不再让 runner
  写 JSON 前崩溃。

N2 已交付（`911be847`）：

- 用**确定性 test-only Run-entry gate**（`FexTestRunGate`，`GUEST_CPU_TEST_HOOKS` 宏 gate，release 不编译）替换
  G24 的任意-PC SIGUSR1 冻结。gate 点在 `Run()` 内 running 已置、execution lease 已持、**全部 coordinator/context/
  address-space 锁已释放、进入 FEX ExecuteThread 之前**；不持协调器需要的锁、不在信号处理器内；按 thread id 键控，
  严格到达/释放/退出代次握手。旧"持锁冻结把第二 owner 挡在 futex"的偶发从协议上消除（非重试/sleep 降率）。
- 旧 CodeInvalidationMutex 锁身份结论标注为**未决假设**（保留历史观测）。G24 全部既有断言、200ms drain、两种
  request 次序、Pause/Cancel/Shutdown、retry/stale-Resume/外部 epoch 保持。
- AYN Thor/API33/4KiB：11 次完整 guest suite（202 checks×11、100 G24 迭代）全 ALL PASS/exit0；canonical runner
  设备 guest 104 sub 0 失败、contract 43/43、bionic smoke 12、runner exit 0，R2-C03/H01/H02 正式 NOT_RUN +
  aux PASS。SIGUSR1 helper 仍供 G15 late-ack 使用（不依赖第二线程在 hold 中停止）。

N3 待办（生产，进行中）：

3. **N3 syscall 错误立即退出原语**：固定 FEX 非 Windows `DEFAULT_SYSCALL_FLAGS` 不含 BLOCK_END，
   `syscall;sentinel store` 同 block，HandleSyscall 正常返回后 sentinel 必执行。需交付简短设计+最小真实探针：
   C++ dispatch 正常完成 RAII 清理后转至已证明的 backend 出口（不 longjmp 出持锁/native/allocator 帧、不设全局
   `_WIN32`、不用 MAXINST=1/逐指令、不改 fixture 成 `syscall;jmp`）；核对 InSyscallInfo/ReturningStackLocation/
   callret/x28-frame/callee-saved/栈对齐/lease。三条最小验收：unknown `syscall;sentinel=42`→GuestFault 且 sentinel=0
   且发生 PC=syscall；registered-buffer 拒绝→native count 不增、pin 回基线、sentinel=0；正常注册调用→后继 store
   执行、返回值正确。另加错误后自跳 loop，单次无外部中断 ≤1 秒返回。
4. **N4 错误归属**：按 context/thread/generation/invocation/operation/fault guest PC/category/system_error 记录；
   去掉任意下一 owner 消费全局 unknown bool；正式 G33+ 测试同提交接入 ownership/ROUND2/N1 退出码负例；G30 保持
   bare-HLT。
5. R2（host/guest FP、native 异常封装、errno）、R3（完整 ABI/buffer/typed registry/TLS）在 N3/N4 之后；S0–S3
   H3/callback 更后，R1–R3 不过不开始。

已实现的 H0/H1/H2 基础（均为 partial，不等于 R2-H01/H02/H05 完成）：frame→thread fault flag；真实 6/8 整数和
4 double gate；count 有界 buffer pin、sum60 和三个坏参数不进 native。主仓没有 HleScope/InvokeGuest/WaitingHle
骨架；FEX HandleCallback 的 callback return trampoline、host/guest 栈、SignalHandlerRefCounter、cancel 清理尚未
接入，普通 HLT return gate 不能直接替代。

历史 [G2 修复记录](g2-exit-repair-2026-09-09.md) 的 Pocket DS 97IDs/195checks 及 43/43 是当时结果，不能覆盖当前
设备；G2 Q1–Q3 尾项仍 NOT_RUN，不因转入 G3 划掉。Swan Android16/4KiB 普通 APK/JNI/ART/Foundation 生命周期/
package ABI/closure/ZIP/只读 guest snapshot 仍在 G4。V0_IN_PROGRESS；16KiB/finite Step/Vulkan/游戏/VR 后置。
