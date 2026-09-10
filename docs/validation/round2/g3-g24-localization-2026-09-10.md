# G24 偶发失败定位与修复（2026-09-10，R0.4）

被审/修复基点：`25189931`（R0 runner 记账）之上，工作区改动仅在 `tests/guest_cpu/guest_execution_tests.cpp::TestCoordinatorRecovery`。生产代码 `fex_context.cpp` / `address_space.cpp` / 固定 FEX 均未改——定位结论是**测试冻结手段的缺陷**，不是生产暂停行为。

## 1. 现象

复核在 AYN Thor / API33 / 4KiB 两次完整 suite（各 202 checks）分别一次 G24a、一次 G24b FAIL，均 exit 1；单独跑 G24 诊断副本 100 次通过。本阶段大量复跑复现率约 1/15–1/25 完整运行，且只在完整套件高负载下出现。

## 2. 拆断言后的逐阶段定位

原 G24 把 `closed && recovered && finished` 十几个阶段谓词压成一个布尔，失败只报 `G24a/b FAIL`。先把每个阶段独立记录，失败时在 FAIL detail 打印阶段名、迭代号和关键值（`record(...)` 辅助 + 每次迭代仍发 `Check`，保留 100 次迭代身份与 runner 重复/worst-verdict 记账）。

复现稳定指向**同一个阶段**：

> second owner reached its stop while the first owner is still held

即"owner 被信号处理器冻结期间，另一个 owner 必须独立到达停止点"。

## 3. 两类候选的决定性区分

加两项真机现场诊断（失败分支打印到 stderr）：

1. second owner 的 guest progress 计数器在冻结窗口内是否继续前进；
2. 两个 owner host 线程的 `/proc/self/task/<tid>/wchan`。

复现现场（两次独立复现一致）：

```
[g24] detail second_guest_progress_advanced_while_owner_held=0
[g24] detail owner_tid=.. wchan=(userspace/running) | second_tid=.. wchan=futex_wait_queue_me
```

- second 的 guest progress **冻结**（不是继续跑 guest）→ 排除"中断请求未送达 / second 未服务中断"。
- owner 在信号处理器里**用户态忙等**（符合 `HoldOwnerSignal` 的自旋），second 阻塞在内核 **futex**。
- `hold_release` 之后 `Await(second_run)` 立即成功（否则会走 `std::_Exit(4)`，实际是 Check FAIL）→ second 只差一把被 owner 间接占有的锁。

## 4. 根因

测试用 `tgkill(SIGUSR1)` 把 owner 冻结在 **FEX JIT 执行中的任意 host 指令点**。FEX 每条 guest block 链接路径
（`references/FEX/.../JIT/JIT.cpp` `Arm64JITCore::ExitFunctionLink`，见 545、568 行）在一个极短窗口内
通过 `GuardSignalDeferringSection<std::shared_lock>(CTX->CodeInvalidationMutex, Thread)` 持有 FEX
**context 级写优先读写锁 `CodeInvalidationMutex` 的读锁**（`FEXCore::Utils::WritePriorityMutex`）。

当 SIGUSR1 恰好落在这个窗口（完整套件高负载下约 1/15–1/25 每运行一次），owner 被冻结时仍持该读锁，且信号处理器
直接返回 FEX 停止 trampoline 之外、不释放 C++ 作用域锁。写优先读写锁语义下：一旦有写者排队，新读者在 futex
上排队等待。second owner 做下一次 block 转换取同一读锁时即阻塞在 futex，无法到达 block-entry 停止安全点，
直到 owner 被释放、作用域锁解锁。

**这不是生产暂停路径的行为**：生产的 Pause 通过 `mprotect(InterruptFaultPage, PROT_NONE)` 让 owner 只在
**block-entry 安全点**（不持任何 FEX 锁的点）停下并走 FEX 停止 trampoline。测试却在任意指令注入冻结，可能冻结在
FEX 内部锁的持有窗口里，构造了一个生产中不存在的"持锁冻结"状态。

## 5. 修复（测试侧）

`TestCoordinatorRecovery` 的冻结改为**观测条件门禁**，不降低任何被断言的语义：

- SIGUSR1 冻结 owner 后，在 30ms 观察窗内确认**尚未 drain 的 second owner 的 guest progress 持续推进**——
  这正是"owner 停在无锁安全点"的可观测性质（owner 持有 `CodeInvalidationMutex` 时 second 会在 futex 上冻结、
  progress 永不前进）。
- 若 second 停滞（冻结落在持锁指令），置 `hold_release` 释放 owner，并以 owner 自己的 progress 重新前进作为
  "已返回 guest 执行"的观测条件，然后在另一条指令上重新冻结；有界最多 100 次（`_Exit(4)` 兜底）。

这不是 settle sleep：没有"睡一觉再 poll 同一状态"，而是用 second 的实际推进证明冻结点无锁，坏落点会被释放并
在不同点重试。被断言的系统契约（drain 期间准入拒绝、concurrent quiesce Busy、drain 超时且 quiescent/lease
拒绝、释放后两者到停止点、retry 恢复两线程、stale Resume 拒绝、forbidden run 表面外部请求、progress 不变、
live Resume 后继续推进、最终 Cancel）全部保留，次数仍是 100 次迭代（50 ordering0 + 50 ordering1）。

阶段诊断（progress/wchan/record stage）保留为失败现场输出，便于以后再出现时直接读阶段。

## 6. 验证

- 修复前：原始 `wait_for(0)` 非阻塞轮询版本与 2 秒观测窗版本均在完整套件复现 G24（复现现场如第 3 节，
  wchan/progress 诊断在 iter=88、iter=89、iter=2 等多次独立复现一致）。修复前连续复跑复现率约
  1/15–1/25 完整运行。
- 修复后：**60 次完整 guest suite（每次 202 checks，含 100 次 G24 迭代）在 AYN Thor/API33/4KiB 上
  全部 exit 0、ALL PASS、`[g24] FAIL` 计数为 0**。原始逐次日志保存在执行机 `/tmp/g3-r0-repro/v1..v60.txt`
  （每台机器路径不同，移植时重跑回填）；本目录由复核证据与本说明构成。

G24 正例/已有负例、contract 43/43 不受影响。修复只触及测试的冻结取点，不改生产暂停/协调代码，因此 G2 Q1–Q3
的生产侧要求不受影响。阶段诊断（second progress、owner/second wchan、`record` 各阶段）保留为失败现场输出：
未来再出现 G24 失败时会直接打印阶段名、迭代号与两个 host 线程的等待点，而非回到一个不透明布尔。

