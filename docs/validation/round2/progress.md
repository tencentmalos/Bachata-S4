# 二周目进度（2026-09-10 G3 修复进行中：R0 已完成）

分支 `codex/android-fex-round2`，HEAD `8eded461`。前序 G2 源码修复 `470109f6`，H0/H1/H2 分别是
`1f8c7a21` / `0e10defc` / `56cbc7b1`。G3 修复新增：`25189931`（R0 runner 归属 + 未映射 FAIL 安全网）、
`8eded461`（R0 G24 冻结取点修复）。origin 仍在 `0e10defc`，H2 与 G3 修复**仅本地提交，未 push**；推送后须
核对实际 remote ref。FEX/Foundation pin 未改。

**当前进度：R0（runner 记账 + G24 定位修复）已完成；R1（错误立即退出）尚未开始。** 先读
[H2 复核报告](g3-h2-review-2026-09-10.md)、[G24 定位与修复](g3-g24-localization-2026-09-10.md)，按
[修复→H3 spec](../../specs/android-fex-round2-g3-repair-h3.md)继续 R1。

R0 已交付：

- runner 归属：G30a/b（bare-HLT 故障归属，仅映射 R2-C03 fault-priority，**不是** H05）、G31a-c
  （R2-H01 partial aux）、G32a/b（R2-H02 partial aux）已接入 SUITE_MAP/ROUND2_MAP/SUITE_OWNERSHIP；
  正式 R2-H01..H06 仍 NOT_RUN。`sub_cases_owned_by` 现在覆盖 ROUND2_MAP，零输出崩溃也会 taint 这些 ID。
- 新增**未映射 FAIL 安全网**：任何 FAIL/tainted 但不被任何 V0/R2 case 引用的子项强制进报告并使 runner
  非零退出，杜绝"新前缀 FAIL+exit0 被漏记"。runner 自测新增 3 条负例（未映射 FAIL 必抓、未映射 PASS 不误伤、
  映射 G31a FAIL 落 R2-H01），15/15 通过。
- G24 偶发失败已定位并修复（测试侧，生产/固定 FEX 未改）。根因：SIGUSR1 可在 FEX block-link 路径
  （`JIT.cpp ExitFunctionLink`）持有 context 级写优先 `CodeInvalidationMutex` 读锁的极短窗口冻结 owner，
  写优先读写锁把另一 owner 的下一次读锁挡在 futex 上，使其无法到达 block-entry 停止点；真机现场为 held owner
  用户态忙等、second 在内核 futex、second 的 guest progress 已冻结。生产 Pause 只在 block-entry 无锁安全点停线程，
  从不在指令中间冻结，故这是测试冻结手段缺陷。修复：SIGUSR1 冻结后用观测条件确认另一未 drain owner 的 guest
  持续推进（即冻结点无锁），否则释放并在不同指令重冻（有界），不是 settle sleep。AYN Thor/API33/4KiB 上修复前
  约 1/15–1/25 复现，修复后 **60 次完整 suite（每次 202 checks、100 次 G24 迭代）全部 exit 0、ALL PASS、
  零 G24 失败**。
- G24 谓词已拆为逐阶段 `record(...)`，失败 detail 报阶段名/迭代号/second progress/两线程 wchan；仍每次迭代发
  Check，保留 100 次迭代身份与 runner 重复/worst-verdict 记账。

R1–R3 待办（生产代码，仍按 spec）：

1. ~~R0：runner 归属、G24 诊断定位与修复~~（已完成 `25189931` / `8eded461`）。
2. R1：unknown/rejected 调用在后继 store 前**立即退出 guest**（不设全局 `_WIN32`、不依赖逐指令模式、不
   长跳出 C++ 帧），按 thread/generation/invocation/operation/guest PC/category 记录错误。现有
   `syscall;sentinel` 探针 sentinel 仍写 42，必须变 0；错误后自跳 loop 也须有界返回 owner。
3. R2：真正 host/guest FP 切换（进入 guest 前存 owner host fenv，native 调用前装入、返回后恢复 guest
   FPCR/MXCSR/FPSR）；RAII 覆盖解码/pin/分配/native/编码；native 异常在 C++ HLE 边界捕获转错误并走 R1 出口
   （当前未捕获，进程 exit 134）；定义 host errno 与 guest errno 的映射/保存。
4. R3：真实 guest 汇编布置 8 整数/9 double/混合及 stack spill（RCX/R10 参数视图 vs syscall 后 RCX/R11 架构状态、
   callee-saved/RSP 对齐/red zone/XMM/GPR 返回）、有类型/生命周期的注册入口（拒绝 void* unchecked cast）、
   显式 In/Out/InOut/nullable/element size/最大长度/零长语义、先全解码 pin 再进 native、真实乘法溢出
   （`count > UINT64_MAX/sizeof(T)`）、native 持 pin 时另一 owner protect/remap、每拒绝点 native count 不增且
   sentinel 不变、两 owner 独立 FS/GS/errno。
5. S0–S3：R1–R3 通过后才进入。S0 先做单层 callback 原语独立证明（优先评估 owner 普通栈调度；FEX HandleCallback
   需专用 callback return gate、SignalHandlerRefCounter、callret stack、栈/binding 清理核验）；S1 HleScope/
   Invocation 栈与受控 InvokeGuest；S2 两层嵌套与独立 TLS；S3 可取消 WaitingHle 与 G2 交叉。

已实现的 H0/H1/H2 基础（均为 partial，不等于 R2-H01/H02/H05 完成）：frame→thread fault flag；真实 6/8 整数和
4 double gate；count 有界 buffer pin、sum60 和三个坏参数不进 native。主仓没有 HleScope/InvokeGuest/WaitingHle
骨架；FEX HandleCallback 的 callback return trampoline、host/guest 栈、SignalHandlerRefCounter、cancel 清理尚未
接入，普通 HLT return gate 不能直接替代。

历史 [G2 修复记录](g2-exit-repair-2026-09-09.md)的 Pocket DS 97IDs/195checks 及 43/43 是当时结果，不能覆盖当前
设备；旧 V0 10/0/47、24 个 R2 最终 NOT_RUN 也不能手工累加 H1/H2 通过数。G2 Q1–Q3 尾项（Query/decoder 邻接/边界/
remap、跨 mapping/销毁 token、普通 mutator sink、按组合计数、每轮 generation/耗时）仍 NOT_RUN，不因转入 G3 划掉。

Swan Android16/4KiB 普通 APK、JNI/ART/Foundation 生命周期、package ABI/closure/ZIP 和只读 guest snapshot 仍在
G4。V0_IN_PROGRESS；16KiB/finite Step/Vulkan/游戏/VR 后置。
