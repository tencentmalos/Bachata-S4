# 执行 spec：修补 N1/N2，完成 N3 可执行出口实验

日期：2026-09-10。基点`dfb759a0`，分支`codex/android-fex-round2`。先读 [本轮复核](../validation/round2/g3-n12-review-2026-09-10.md)与[证据](../validation/round2/g3-n12-review-2026-09-10/README.md)。本文件细化并接替 [exit-next](android-fex-round2-g3-r0-exit-next.md) 的当前执行入口；N4以及[完整修复spec](android-fex-round2-g3-repair-h3.md)的R2/R3/S0–S3、G2 Q1–Q3保持有效。

## 0. 可验收的本轮目标

**先修已复现的runner/gate反例，再交付能实际运行的N3出口探针和结果。只提交另一份设计说明不算N3完成。** 本轮不扩展HleScope/InvokeGuest/WaitingHle、完整FP/ABI、APK、16KiB或图形。独立实验通过不等于生产R1已接入，必须明确二者区别。

现状：guest104IDs/202checks、device contract43/43、host42/43+1SKIP；runner现有18/19条自测全过。R2-only失败已修，但B02/B05/B06直接FAIL仍exit0；损坏可执行文件被ENOEXEC跳过；gate旧代未退出即可re-arm且一次exit可确认两次Release。N3仅设计，未写probe。

origin复核时仍为0e10defc；关联文档大量未跟踪。先确认实际HEAD、remote、working tree和子仓，不reset或丢弃他人材料。FEX `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`、Foundation `1f7008848736b7c6c0779220480344fd5b5fc5e3`固定；优先只改主仓。正式目标Swan/Android16/4KiB，本轮AYN Thor/API33/4KiB只能记CLI辅助。

执行顺序：**A runner → B gate → C0无行为改变wrapper → C1单次错误出口 → C2完整最小矩阵 → D交付**。每步完成后才扩大范围；不要在没有可执行探针时反复以“工作量大/风险高”替代能力结论。若遇到真实工具/硬件/接口阻碍，提供具体失败和下一操作。

## A. 修复所有结果来源的整体失败

文件：`scripts/android/run-v0-tests`及两套runner测试。

1. `overall.has_failures`统一覆盖最终V0/R2 FAIL、唯一失败sub、未映射sub、失败suite与直接checker。不要依赖某一种结果来源恰好也在另一集合中。直接checker的失败不伪装成guest子项。
2. 增加不变量回归：任一最终父case为FAIL时，overall必为失败、退出非零。至少分别注入B02/B05/B06，无其他suite输出。再跑已有G30–G32、冲突、missing/SKIP、crash/timeout、去重矩阵。清理未使用的total_failed，避免再次出现两套不一致判断。
3. 捕获启动OSError并保留JSON是必要行为；**默认将指定存在文件的启动错误记为失败**。missing仍NOT_RUN；正常host/device目录应分别传入。若保留跨平台自动跳过，必须独立证明文件格式与平台，并通过明确策略启用，不能用ENOEXEC直接推断。
4. 增加实际进程测试：损坏文件、有执行格式错误、权限拒绝、missing、正常host程序。即使未开始执行子程序，runner也应识别“指定产物不能启动”这一配置/产物失败；与“没有构建”区分。
5. 测试存储returncode+JSON+stdout/stderr；至少一个真实checker失败跑runner子进程。可替换外部compiler/verifier返回失败，但保持runner原聚合/退出代码。不能因缺报告的异常退出碰巧等于1而通过。

验收：本轮 [runner反例](../validation/round2/g3-n12-review-2026-09-10/runner/summary.json)全部获得正确失败与非零退出，旧R2-only回归继续正确。无故障的partial/未构建仍NOT_RUN且exit0。无需新增通用二进制扫描框架。

## B. 修补 test-only gate 的身份、退场与记录

保持现有Run-entry位置，不回退SIGUSR1任意PC冻结或SleepThread parking。gate只服务测试，不发展成生产调度API。

### B1. 有票据的状态转换

使用明确的Idle→Armed→Arrived→Releasing→Exited/TimedOut状态或等价不可混淆的协议。Arm返回带arm generation、context、ThreadHandle(id+generation)、目标/实际Run invocation的票据；实际命名自行决定。若Arm指定“下一次Run”，到达时绑定invocation，后续Release/等待必须引用同一票据。

- Releasing但旧owner未退出时不能Arm新代；迟到/重复/外context/错thread generation的Release不能影响当前代。
- 退出确认比较具体票据，而非任意`exited_generation > before`。一次owner退出最多完成所属代的确认。
- Release先于Arrived、重复Release、owner超时、controller超时、旧票据在新Run中复用均有明确结果。超时后旧owner仍存活则保持封闭，不得伪造退出；旧owner确实退出后有定义的恢复路径。
- 初始化gate必须在允许并发Run前完成，或增加正确的同步发布；不要保留懒写unique_ptr与Run读之间的数据竞争。测试接口可以约束初始化时机，不需要动态注册框架。
- Gate owner等待超时后，Run返回、running标志、已保护的interrupt页、pending receipt/WaitStopped及lease释放必须一致；控制端能得到明确错误，不永久等一个永远不会发布的receipt。

### B2. 确定性负例与G24验证

把本轮[实际gate反例](../validation/round2/g3-n12-review-2026-09-10/gate-probe.cpp)迁入正式测试，使用协议barrier构造顺序，减少依赖调度碰巧发生：

| 场景 | 必须结果 |
|---|---|
| 旧代已Release但尚未Exited时Arm | 拒绝/Busy；新代不能接管旧owner |
| 两次Release争同一owner退出 | 重复调用按明确契约拒绝或幂等返回同一票据；不得确认另一个新arm |
| owner超时后重新Arm/Release | 终态与恢复一致，无两个额外超时才清状态 |
| 错context/thread generation/invocation/stale arm | 不影响当前等待者、epoch或guest状态 |
| Release前到达与Release前未到达 | 各自行为确定，均不伪造Exited |

保留100轮G24六种reason/ordering组合，并补同owner已执行guest、暂停/恢复后在下一Run hold的状态延续测试。每轮输出结构化`G24`事件，至少包含两owner身份、gate代次、request/receipt/token epoch、外部reason/ordering、阶段耗时及progress；runner核验次数和身份，不只是累加PASS。

确认第二owner仍通过真实JIT独立停止；held owner未release期间必须没有guest推进；timeout期间admission/lease拒绝；release+retry只消费内部Pause；外部请求仍需明确Resume。保留G1对活动JIT中断的全部测试。gate测试退出/超时需有界清理，不能靠整个进程立即退出代替清理证据。

验证NDK两个配置：tests=ON可用，tests=OFF无gate符号及等待分支。注意`CMAKE_BUILD_TYPE=Release`不等于tests=OFF。完整suite只做1–3次有编号复跑；任何失败保留并定位。

## C. N3 真实 FEX 出口实验

### C0. 先接一个完全保留行为的ABI wrapper

以下是源码支持的**候选实验路线**，尚未做运行验证。可以选择其他有证据的路线，但不能再次只列候选而无可执行结果。

连接点：[JIT构造](../../references/FEX/FEXCore/Source/Interface/Core/JIT/JIT.cpp#L658)初始化`CurrentFrame->Pointers.SyscallHandlerObj/Func`，[BranchOps](../../references/FEX/FEXCore/Source/Interface/Core/JIT/BranchOps.cpp#L299)从frame加载函数地址调用。考虑在主仓thread/backend初始化完成、首次Run之前，为实验frame安装单独的ARM64汇编wrapper，保留原obj/func用于正常转发。

1. 用独立实验目录/目标、真实FEX静态库、实际主仓harness/guest fixtures。若必须修改adapter连接点，使用可归档的主仓adapter副本或受控实验开关；不编辑FEX reference，不在活动JIT上patch代码。
2. 指明obj/func安装位置、保存位置、首次Run读回，以及backend重建/缓存重建会不会重置指针。Frame数据只由拥有其生命周期的adapter配置；FEX类型不泄漏到公共CPU API。
3. wrapper保存必须跨C++调用保存的host ABI内容，然后调用C++ shim。shim普通调用原handler并正常return；wrapper再普通返回原JIT continuation。禁止从virtual C++函数中途切SP。
4. 核验实际构建的间接调用路径（包括DisableVixlIndirectCalls/simulator相关配置）、ARM64 ABI与SP对齐。记录入口obj/frame/SP/LR/x28、ReturningStackLocation、InSyscallInfo、guest RIP/RCX/R11、callret_sp。尽量写固定trace记录，返回owner后再输出，避免让日志函数污染待观测寄存器。
5. 正常HLE先10次，返回值与后继sentinel正确，owner host callee-saved/FP恢复；unknown路径此步仍应复现旧sentinel=42。说明实验没有靠停掉全部syscall获得假阳性。

交付：wrapper/实验连接代码、构建命令、反汇编、安装读回及上述trace。**通过C0再写错误跳出分支。**

### C1. 单次 unknown 错误退出

固定源码事实：

- SyscallOp已经保存syscall发生PC与RCX/R11；同block后继指令不因修改State.rip自动停止。
- Syscall调用C++前已spill；返回后的host寄存器不能再次当完整guest寄存器写回。
- Dispatcher的`ThreadStopHandlerAddressSpillSRA`先spill，`ThreadStopHandlerAddress`跳过spill。**两个入口都不自动恢复SP。** [G1 handler](../../src/core/guest_cpu/fex/fex_context.cpp#L216)是在跳转前显式设SP/x28。
- [保存区](../../references/FEX/FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.cpp#L578)为D8–D15低64位64字节，加X19–X30的96字节。ReturningStackLocation指向其起点，Pop恢复包含真正返回LR。此布局仅对当前pin成立。

候选错误路径：C++ shim以固定POD结果返回Continue/ExitFault，所有局部pin/shared_ptr/锁/临时对象已析构；汇编wrapper依据结果分流。错误分支保留syscall内存快照，清理InSyscallInfo，按核验过的frame与保存区设置SP/STATE，转到**非spill**stop入口，让ExecuteThread返回Run收尾。必须先证明没有需要析构的C++调用帧被跳过；不能在handler内部直接调用这段跳转。

需要逐项说明责任：

| 状态/资源 | 本轮必须明确 |
|---|---|
| wrapper自有host栈、LR与callee-saved | 正常返回路径与错误路径分别怎样恢复/退出 |
| 动态JIT保存区/ReturningStackLocation | 为何可退休，当前没有C++清理责任；禁止手工硬编码未知栈偏移 |
| guest GPR/XMM/flags/MXCSR/rip | 哪些已spill且有效，哪些需重建或不宣称有效；不得二次spill错误host值 |
| InSyscallInfo/callret_sp | 明确退出值，先观察再决定是否复位；不照搬callback协议 |
| frame注册、t_binding、host fenv、execution lease | 仍由实际Run收尾，不在handler尚执行时撤销 |
| 错误记录 | 至少绑定当前frame/thread/invocation、operation、syscall PC与类别；完整N4生命周期留后续 |

先只做unknown一条：`syscall; mov [sentinel],42; ...`，必须GuestFault、sentinel=0、fault PC=syscall PC，且正常回到owner C++收尾。保存失败结果，出现abort/stack corruption时不得把guard跳过或改fixture隐藏。

禁止：长跳越过C++ scope、跨JIT抛异常、全局_WIN32/MAXINST=1、依靠下个block或额外Cancel、把fixture改成syscall后先jmp、用普通HLT gate冒充callback返回。实验中的wrapper跳转只能在明确的汇编ABI边界发生。

### C2. 最小矩阵与独立证据

单次出口通过后，unknown/registered拒绝/合法HLE各100次，错误后自跳loop100次：

- unknown：GuestFault、sentinel不变、正确PC与operation，无原错误状态串到重建线程。
- registered buffer拒绝：native计数不增、pin回基线、sentinel不变；包括一个参数已pin而后续参数失败。
- 合法HLE：返回值、后继store都正确；保持原成功continuation。
- 错误后自跳loop：每次无需外部Cancel，≤1秒返回，记录每轮耗时。外层watchdog仅用于收集失败，不当作成功的停止机制。
- 每次核验owner host栈/寄存器、host fenv、binding/frame注册、lease和线程销毁；不得把“没有崩溃”当完整清理。

probe子项必须有机器可读ID/次数/结果，出错程序非零退出；配套runner反例覆盖FAIL+exit0。独立实验测试身份与正式guest suite区分，不能直接把实验copy通过写成production R2-H05完成。

若候选连接点/出口失败，交付**实际最小失败ELF的源码与构建、精确失败位置和状态**；据此选择第二路线或提出最小依赖需求。不能仅因为尚未做实验就判断固定FEX能力不足。需要依赖改动时先核对ownership/贡献规则，不擅自切pin。

## D. 完整交付与后续门槛

交付A/B修复与正式负例、N3实验源码/构建/反汇编/trace/矩阵结果、canonical回归、tests=OFF排除证据。记录源码/fixture/runner/FEX库SHA、binary Build ID、实际device SHA、设备/API/进程页大小、exit/timeout。历史11轮及dirty canonical保留，不倒填构建身份。

更新progress/AGENTS/CLAUDE/docs入口及旧N3设计勘误。关联spec/报告/证据一起形成明确Git交付集；显式stage相关路径，不混入Windows/Vortek、Bachata子仓本地变化、externals临时目录或build二进制。核实远端是否包含交付提交；仅本地commit就明确说尚不能远端检出。

本轮成功状态：**A/B反例关闭，N3出口原语在独立真实FEX实验通过；N4生产整合/生命周期矩阵仍待验收。** 若完成C2后继续落地主仓，应执行原N4正式要求并重新标定证据，不能只复制实验结果。

下一阶段依次为N4（生产错误归属/双owner/interrupt交错）→R2（FP/异常/errno）→R3（完整ABI/buffer/TLS/typed registry）→S0–S3 H3。G2 Q1–Q3与Swan Android16普通APK G4不自动关闭；整体仍V0_IN_PROGRESS。
