# 二周目进度（2026-09-10 syscall立即退出已生产化：G33/G34正式验收）

> **N3/N4 核心已从实验 shim 落地为生产路径（提交 `1a6664c6`、`33e58083`），并由正式 guest suite
> 验收**：
> - **生产常驻 syscall wrapper**：`FexSyscallWrapper` 对每次 Run 安装到 `Pointers.SyscallHandlerFunc`（无
>   arm 开关、无全局决策槽），转发原 handler；fault 时在 C++ dispatch 干净返回后经 naked `FexSyscallExitToStop`
>   （x28=frame、sp=`frame->ReturningStackLocation`、br **非-spill** stop 入口）让 ExecuteThread 在 syscall 点返回，
>   后继同 block 指令不执行。per-thread stop 地址/fault 标志在 owner 的 ThreadInterruptBinding/thread_local。
> - **G33a**（unknown）：GuestFault、sentinel=0、fault RIP=syscall PC；**G33b**（valid）：sentinel=42（证明出口
>   仅错误路径）；**G34**（unknown 自跳 loop）：无外部 Cancel、首次 fault syscall 即返回，实测 0ms（预算1s）。
> - tests=OFF release 含 `FexSyscallWrapper/ExitToStop`、零 gate/trace 符号；真机完整 suite **205 checks ALL
>   PASS**，canonical guest 107 sub 0 失败、R2-H05 正式 NOT_RUN + aux PASS；runner python 22/22、accounting 21/21。
>
> **仍待 N4 完整矩阵**：registered-buffer 拒绝的立即退出 + pin/部分 pin、双 owner 错误隔离（错误不串合法 owner）、
> 与 Pause/Cancel/Shutdown 交错、销毁重建不继承旧错误、按 context/thread/generation/invocation/operation/
> guest PC/category/system_error 的结构化错误事件（当前仍是 per-frame bool + category atomic）。之后 R2（FP/
> 异常/errno）→ R3（完整 ABI/buffer/typed registry）→ S0–S3。G33/G34 是 CLI 辅助，不等于完整 R2-H05 验收。
>
> 前序（`4da75699`/`1a65bb1f`）已关闭 C1 复核三反例：非-spill 出口修 10-GPR 快照破坏（state probe mismatches
> 10→0）、gate token 换代竞态（gate-race NO_REPRO/100）、gate 超时 fall-through 收尾（WaitStopped 不再超时）。
> origin 仍 `0e10defc`，这些提交仅本地未 push。

# 二周目进度（2026-09-10 C1复核三反例已修复并真机验证）

> **C1 复核（报告 [g3-c1-review](g3-c1-review-2026-09-10.md)）的三个实测反例已关闭并真机/host 验证**
> （提交 `4da75699`、`1a65bb1f`）：
> - **P1 C1 破坏快照**：syscall fault 出口从 SpillSRA 改为**非 spill** `ThreadStopHandlerAddress`。
>   复核状态探针（注入 GPR/XMM 标记）在修复后正式代码：16 GPR+16 XMM **mismatches=0**（此前 10 GPR 错、
>   validity 仍标有效）、sentinel=0、fault rip=syscall PC、exit 0。根因：syscall prologue 已 spill guest，
>   SpillSRA 用 ABI-clobbered host 值二次 spill 覆盖快照；非 spill 入口只恢复 dispatcher callee-saved 保存区。
> - **P2 gate 换代竞态**：每代状态改为 token 键的 Generation struct + history 归档；Release wait 返回后
>   重新校验 live token，旧 waiter 不写新代字段。复核 `gate-race-probe` 现 3× NO_REPRO/100；host 协议
>   测试新增换代/陈旧票据交错共 9 场景全过。
> - **P2 gate 超时收尾**：WaitAtEntry 超时不再返回裸 BackendFailure（会留 page 保护、receipt 缺失致
>   WaitStopped 永久超时），而是 fall-through 进 JIT 由 block-entry fault 走正常停止收尾。复核 timeout-probe
>   现 run=OK wait1=wait2=OK lease=1 owner_destroyed=1 reproduced=0 exit0。
> - 探针可移植性：N3 CMake/源码去本机绝对路径，fixture 由 generate-guest-fixtures 从 review.S 生成，
>   全新目录可构建。完整 guest suite 202 checks ALL PASS；runner accounting 21/21、python 21/21。
>
> **C1 仅证明单次 unknown 立即退出的正确性**；C2 矩阵（rejected-buffer/部分 pin、各100次、错误后自跳
> loop ≤1s、双 owner/中断交错）与 N4 按 invocation 的错误归属 + 正式 G33+ 用例仍是下一单元；实验通过≠
> 生产 R2-H05 验收。origin 仍 `0e10defc`，这些提交仅本地未 push。
>
> 以下保留 `f3b93d98` 复核当时的状态记录（已被上述修复取代）。

被审 HEAD `f3b93d98`，分支 `codex/android-fex-round2`；origin 实查仍为 `0e10defc`。
先读 [最新复核](g3-c1-review-2026-09-10.md) 与 [证据](g3-c1-review-2026-09-10/README.md)，
继续 [现有 N12/N3 执行 spec](../../specs/android-fex-round2-g3-n12-fix-n3-probe.md)。

**A 关键反例关闭；B 尚未收口；N3 已有立即退出能力，但当前 C1 状态不正确，不能验收。**

- A：B02/B05/B06直接FAIL均使runner返回1；损坏产物ENOEXEC归失败。两套runner自测21/21与21/21通过。
- B：gate票据与初始化已有改进，但旧Release晚于新Arm完成时会污染新代结果；公共API反例第1轮复现。
  真机gate超时后Run返回BackendFailure、WaitStopped仍连续Timeout。warm-owner G24及结构化身份/epoch记录仍缺。
- C1：当前SpillSRA在C++返回后覆盖已spill的guest快照，实测10个GPR错误且validity仍有效。
  仅在构建目录副本改为非spill stop后，GPR/XMM状态探针通过，good HLE仍继续；正式实现未改。
  保留syscall时读取ReturningStackLocation，先落修正并补状态/清理验证，再完成C0/C2矩阵、N4整合。
- 当前正式源码fresh canonical：guest104个唯一ID/202checks、device contract43/43、host42/43+1SKIP；
  V0 10/0/47+3deferred，正式R2的24项全NOT_RUN。默认关闭shim的全绿不能证明C1正确。
  tests=OFF新构建无gate/shim符号。设备AYN Thor/API33/ARM64/4KiB，非Swan普通APK验收。
- 本次只有复核与诊断副本，没有修改生产实现/正式测试/runner/FEX，也未commit/push。
  本地相关spec/证据仍需完整Git交付，原N3 probe有本机绝对路径和临时fixture依赖。

后续顺序：[最新复核§3](g3-c1-review-2026-09-10.md#3-下一步执行顺序) → N4 →
[完整修复spec](../../specs/android-fex-round2-g3-repair-h3.md)的R2/R3 → S0–S3。
HleScope/InvokeGuest/WaitingHle仍无实现。保留G2 Q1–Q3、Swan Android16/4KiB普通APK G4；
16KiB/finite Step/Vulkan/游戏/VR继续后置。

## 历史记录：下文为本轮复核前的报告，保留原结论供追溯

以下“A/B关闭、C0/C1通过”是当时实施报告，已被上面的独立反例修正；更早dfb759a0状态也不是当前状态。

# 二周目进度（2026-09-10 N12复核反例已修补，N3 C0/C1 探针已真机验证）

> **本轮（在 `dfb759a0` 之上）已按 [N12修补与N3实验spec](../../specs/android-fex-round2-g3-n12-fix-n3-probe.md)
> 关闭 A/B 反例并交付可运行的 N3 C0/C1 探针**：`2ebc4892`（A runner 整体失败覆盖直接 checker +
> 损坏产物）、`da803e3f`（B 票据化 gate 状态机 + host 协议负例）、`fa41c657`（N3 C0/C1 syscall 立即退出
> 探针真机通过）。N3 C2 矩阵与 N4 错误归属/正式 G33+ 仍是下一单元；独立探针通过≠生产 R2-H05 验收。
> origin 仍 `0e10defc`，这些提交仅本地未 push。以下保留被审基点 `dfb759a0` 的复核记录。

分支 `codex/android-fex-round2`，被审HEAD `dfb759a0`。本轮提交 `fd1c8a86`（N1）、`911be847`（N2 gate）、
`97f149af`（ENOEXEC）、`6e7f46e5`（进度）、`dfb759a0`（N3设计）。origin实时仍为 `0e10defc`；
H2与后续改动只在本地，相关spec/报告/证据还有untracked/dirty，不能宣称远端可完整检出。FEX/Foundation pin未改。

**N1/N2有实际进展，尚未全面收口；N3没有可执行探针，H3入口不接受。** 先读
[本轮复核](g3-n12-review-2026-09-10.md)，执行[修补与N3实验spec](../../specs/android-fex-round2-g3-n12-fix-n3-probe.md)。
上一[exit-next](../../specs/android-fex-round2-g3-r0-exit-next.md)的N4和[完整修复spec](../../specs/android-fex-round2-g3-repair-h3.md)
的R2/R3/S0–S3继续有效。

## 本次核验

- G30–G32 R2-only FAIL+exit0已改为runner非零，原P1关闭；现有Python18/18、accounting19/19通过。
- 新P1：B02/B05/B06直接checker分别FAIL时，summary.failed=1但overall.has_failures=false、runner返回0；
  新overall只OR sub/suite，漏掉直接父项结果。损坏可执行文件还被ENOEXEC误判wrong architecture/never_started。
- N2等待点在running/lease已登记、相关锁作用域结束、JIT之前，替代任意PC冻结的方向成立。
  但实际gate反例证实旧代未Exited即可Arm新代、一次owner退出确认两次Release；timeout恢复也不完整。
  这些是test-only协议问题，不能描述为本次发现生产Pause故障。需票据/代次、初始化发布和timeout收尾。
- 独立新目录NDK重编，AYN Thor/API33/ARM64/4KiB完整guest104IDs/202checks全过，device contract43/43、
  host42/43+1SKIP；host ABI14/page22、device bionic smoke12通过。canonical为V0 10 PASS/0 FAIL/47 NOT_RUN、
  3延期，R2正式24项NOT_RUN。详见[证据](g3-n12-review-2026-09-10/README.md)。
- tests=OFF构建中gate相关符号为0；tests=ON为12。仅设CMAKE_BUILD_TYPE=Release不会排除test hook。
- 历史11轮文本全过，canonical有部署hash但标fd1c8a86+dirty、无完整source patch，不能倒填为当前HEAD。

## 下一步

1. A：直接checker失败/启动失败进入整体退出，补真实进程与结果一致性负例。
2. B：修补gate有票据的Arm/Release/Exited/timeout状态；补同owner先执行再hold、每轮epoch/identity/耗时记录。
3. C0：N3安装只转发的ARM64 ABI wrapper，保存/核验frame与原obj/func；先做无行为变化实验。
4. C1/C2：C++清理正常退回wrapper后，再实验非spill stop出口；unknown/rejected/合法HLE/错误后loop各100次，
   记录PC/sentinel/pin/host栈/FP/lease。固定dispatcher出口不自动恢复SP，不复制G1的stop_spill路径。
5. D：完整交付源码/探针/构建/日志/JSON/上下文。N3独立实验通过后，仍需N4生产整合与错误归属矩阵。

H0/H1/H2仍为partial：frame→thread fault flag、真实6/8整数和4double gate、bounded pin/sum60。
unknown/rejected后继store、native FP环境与exception故障尚未改；N3本轮只有设计。之后N4→R2（FP/异常/errno）
→R3（完整ABI/buffer/typed registry/TLS）→S0–S3（callback/scope/两层嵌套/WaitingHle）。主仓尚无H3实现，
普通HLT gate不等同FEX callback返回协议。

G2 Q1–Q3保持NOT_RUN；保留publication/decoder失效、poison、真实guest store、persistent owner/state、
failed-drain retry、内部Pause退休和sink/pin排他。Swan Android16/4KiB普通APK/JNI/ART/Foundation生命周期、
package ABI/closure/ZIP/只读guest snapshot仍在G4。V0_IN_PROGRESS；16KiB/finite Step/Vulkan/游戏/VR后置。
