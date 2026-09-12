# 执行 spec：R0 收尾与 R1 syscall 错误出口

> 当前入口：在dfb759a0完成[N1/N2复核](../validation/round2/g3-n12-review-2026-09-10.md)后，先执行[修补N1/N2与N3分步实验](android-fex-round2-g3-n12-fix-n3-probe.md)。本文N4及后续规范继续有效；不要以旧进度的N1/N2“已完成”跳过新反例。

日期：2026-09-10。依据：[当前复核](../validation/round2/g3-r0-review-2026-09-10.md)、[独立证据](../validation/round2/g3-r0-review-2026-09-10/README.md)。规范继承 [Round 2](android-fex-round2.md)、[V0](android-fex-v0.md)、[修复→H3 spec](android-fex-round2-g3-repair-h3.md)。本文件细化R0/R1并纠正“R0已完成”的进度描述；不删除原spec的R2/R3/S0–S3及G2 Q1–Q3。

## 0. 本轮目标与接手核对

**交付一个能可靠报告失败、具有确定G24同步条件、错误syscall不再执行后继guest指令的CLI基点。先不要实现InvokeGuest、两层callback或WaitingHle。**

起点 `0484a0b9`，分支 `codex/android-fex-round2`，主仓remote指向 `tencentmalos/Bachata-S4`。复核时origin仅到 `0e10defc`，H2/R0只在本地。接手先核实HEAD、remote、dirty/untracked及gitlinks，不reset他人工作。FEX `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`、Foundation `1f7008848736b7c6c0779220480344fd5b5fc5e3`固定。

已复验：AYN Thor/API33/ARM64/4KiB，guest104IDs/202checks、device contract43/43、host42/43+1SKIP；runner原自测15/15与9/9通过，但七个G30–G32 FAIL+exit0仍令main返回0。unknown/rejected后继store、FP/exception为未修复生产问题。60份历史PASS日志身份不完整，不替代fresh build。

正式目标是Swan/Android16/4KiB普通APK；本轮先交付CLI auxiliary，APK项继续NOT_RUN。使用主仓 `cmake/fex`，复用Foundation已启用能力。不要引入新框架、改FEX pin或混入16KiB/Vulkan/游戏工作。

执行顺序：**N1 runner → N2 G24 → N3 出口原语 → N4 错误契约/验收 → N5 归档**。每步独立可审阅；若有确定失败，保留并修复，不以多跑直到绿替代。

## N1：所有实际失败决定 runner 的整体退出

主要文件：`scripts/android/run-v0-tests`、`scripts/android/test-runner-accounting`、`tests/guest_cpu/test_v0_runner.py`。

1. 先计算V0和R2报告，再统一计算本次实际失败。保留V0范围的`summary.failed`时，新增明确整体字段，例如overall.failed_subchecks/failed_suites/has_failures及R2失败项数；具体命名自行决定，语义须可审查。
2. 任何已执行的V0/R2/未映射子项FAIL或suite失败均使runner非零退出；同一sub被多个父项引用时，不声称它是多个独立失败。可以分别报告父项失败数与唯一失败sub数。
3. missing/未启动/全SKIP及partial辅助PASS继续NOT_RUN；它们不单独造成测试失败，也不得成为正式R2-Hxx PASS。任何实际失败都不能被deferred或其他missing覆盖。
4. 保存runner进程的returncode、stdout/stderr和JSON，自动测试三者一致。当前自测helper丢掉returncode，必须修正；不能只用helper推导返回码或只断言R2 JSON。
5. 给suite自身的crash/timeout保留独立错误记录，避免未来无已知ownership的suite零输出崩溃再次消失。已有DEPLOY SHA、EPOCH/STORE严格解析、重复最坏结果规则保持。

最低测试矩阵：

| 场景 | 必须结果 |
|---|---|
| G30a/b、G31a/b/c、G32a/b逐项FAIL + suite exit0 | 正确R2项FAIL；整体失败；runner非零 |
| 上述ID FAIL→PASS重复冲突 | 保留最坏FAIL与重复次数；runner非零 |
| 每个ID缺失或SKIP，其他已提供子项PASS | partial/NOT_RUN，不能完整PASS；无其他故障则exit0 |
| guest suite零输出exit7、timeout | G30–G32 ownership被taint；suite故障可见；runner非零 |
| Z99新ID FAIL/FAIL后PASS | accounting failure可见；runner非零 |
| 新ID仅PASS、正常partial、suite未启动 | 无虚构失败，无正式验收升级 |

迁移本次 [runner-probe.py](../validation/round2/g3-r0-review-2026-09-10/runner-probe.py)中的反例到正式测试，至少增加一个真实runner子进程返回码检查。自测不得受连接真机影响；mock仅替换外部执行/设备发现，parser/ownership/聚合/退出路径保持实际实现。

验收：新增矩阵与两套旧测试全过，并归档修复前后同输入的JSON、stdout、exit。不要把改文档或把R2失败重新叫orphan作为修复。

## N2：G24 使用确定的延迟应答协议

保留每轮拆断言、200ms drain timeout、第二owner独立停止、两种外部request次序、Pause/Cancel/Shutdown、retry恢复、禁止越过外部epoch和最后真正guest推进。

先修正定位说明：历史证据只定位到“second在futex等待且没有guest推进”；CodeInvalidationMutex读锁+排队writer尚未证实。将原报告显著标注为被本复核修正的假设，保留历史观测原文。不得再以second progress变化一次命名“lock-free safe point”。

推荐实现一个仅测试构建启用的延迟owner应答点：

- 位于主仓Run已设置running/持有execution lease、释放context及address-space等锁之后、进入FEX ExecuteThread之前。钩子自身不持任何coordinator将需要的锁，不在signal handler里阻塞。
- owner/context/thread generation/run invocation决定hook身份；到达、release、exited都有明确代次。控制线程必须确认上代退出，才允许arm下一代。任何超时单独FAIL并执行有界清理，不能直接继续重试。
- 如需证明state延续，先让同一owner真实运行/停下/恢复，再在下一次Run入口门禁延迟；不要伪造receipt、修改生产pending位或提前释放execution lease。
- 第二owner在真实JIT运行，coordinator仍走实际QuiesceContext；仅延迟第一owner进入可响应点，测试超时后准入仍关、先发全体request、释放后retry与外部epoch保存。
- hook仅通过test编译开关生效，release产物没有此等待机制。不恢复SleepThread parking，不用这个host阶段测试替代G1对正在执行JIT的Pause/Cancel/Shutdown验证。

如果选择其他方式，先证明同步点和锁生命周期再编码。保留任意PC SIGUSR1冻结作为诊断时，应记录signal所落host PC、frame/invocation、handler进入/退出、失败线程等待点；精确锁归因需要锁地址/调用栈及writer/reader状态，wchan字符串不足。不强制为追溯旧偶发故障无限复跑；可以标记历史锁身份未决，同时用可证明协议消除该测试手段的不确定性。

每轮至少记录iteration、两owner handle/generation/invocation、外部reason/ordering、到达/释放/退出代次、request/receipt/stop/token epoch、各阶段耗时/分类和progress。失败应能看出是哪一个条件而非仅stale boolean。

验收：100轮矩阵各组合计数可核验；完整suite做最多三次有编号运行，任何失败保留并定位。确认尚未release时second独立停止、首轮Timeout且lease拒绝、释放后retry成功、只退休coordinator内部Pause、外部request仍需显式Resume。日志和runner重复次数一致。

## N3：先证明一个安全的 syscall 错误退出原语

先交付简短设计与最小可执行探针，通过再接全量错误类型。这里尚无已验证的现成汇编实现，不把“需要trampoline”直接当作完成设计。

固定FEX路径必须核对：

| 边界 | 已有事实 | 实现需要回答 |
|---|---|---|
| X86 `SyscallOp` | 非Windows默认不带BLOCK_END；保存syscall RIP，并生成RCX=下一PC/R11=flags | 正常continuation和fault发生PC如何区分 |
| ARM64 `BranchOps::Syscall` | PushDynamicRegs、SpillStaticRegs、设置InSyscallInfo、调用handler；正常返回才Fill/clear/Pop | 绕过正常尾声后谁处理这些状态 |
| Dispatcher stop | 有SpillSRA与非spill入口；PopCalleeSavedRegisters按当前SP读保存区 | SP、x28、LR以及架构快照从哪来 |
| 主仓G1中断handler | 从已验证JIT入口probe重建状态，设置ReturningStackLocation并跳stop_spill | 这是另一种停点，不能直接复制到C++ syscall入口 |

源码入口：[SyscallOp](../../references/FEX/FEXCore/Source/Interface/Core/OpcodeDispatcher.cpp#L38)、[JIT syscall](../../references/FEX/FEXCore/Source/Interface/Core/JIT/BranchOps.cpp#L279)、[stop入口](../../references/FEX/FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp#L240)、[PopCalleeSavedRegisters](../../references/FEX/FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.cpp#L613)。以实际pin及构建宏为准，不能依赖行号猜布局。

方案选择：

1. 优先评估主仓拥有的ABI边界wrapper：C++ dispatch正常完成RAII清理并返回“继续/错误退出”结果，wrapper只在确认没有存活C++清理责任后转至已证明的backend出口。记录正常/错误两路完整host栈图和状态来源。
2. 若评估owner普通栈调度HLE，也必须先证明**同一block**中后继指令前退出与continuation保存，不能依赖下一block中断来实现“立即”。H3是否最终采用此方式留到S0。
3. 不默认修改只读reference或FEX pin。确有固定backend能力缺口时提交最小失败探针、已核查出口、候选依赖改动与贡献约束；不能为赶进度改全局_WIN32、MAXINST=1或逐指令模式充作生产解法。

关键限制：

- 不能从virtual HandleSyscall/native/allocator/持锁scope中直接longjmp到owner，不能让异常跨JIT展开。ABI出口只能发生在相关C++自动对象已正常析构之后。
- syscall调用点的guest状态已经spill；C++返回后的host寄存器不是G1入口probe的guest寄存器视图。未经证明不得调用stop_spill覆盖正确快照，亦不能用“naked函数”跳过编译器函数尾声。
- 显式处理InSyscallInfo、ReturningStackLocation、callret栈、x28/frame、callee-saved/host栈对齐、frame注册/binding和execution lease。逐个说明保持、恢复或废弃的依据；不能memcpy整个CPUState覆盖backend指针。
- `Frame->State.rip`改为某地址或置fault flag不能替代退出。禁止将失败fixture改成`syscall;jmp`、删除同block后继store，或通过额外外部Cancel让探针“结束”。

N3最小验收（三条均应走真实FEX与同一生产路径）：

- unknown operation：`syscall; store sentinel=42; ...`，结果GuestFault、sentinel保持0、发生PC是syscall。
- registered但buffer拒绝：同上，native call count不增、所有pin回到基线。
- 正常registered调用：同一后继store必须执行且返回值正确，证明不是无条件停止所有syscall。

另加错误后自跳loop，单次无需外部中断须在1秒内返回。先各100次单owner，再进入N4。错误路径返回后owner host栈/FP恢复、线程可正常销毁；完整crossing FP矩阵仍属后续R2。

## N4：按 invocation 保存错误，接入正式 R2-H05 辅助验证

事件记录至少有context/thread/generation/invocation/operation/fault_guest_pc/category/system_error及事件种类；保留参数拒绝的具体信息。fault发生PC与正常继续PC分开，RCX/R10仅用于参数视图转换，不能把参数视图整体覆盖syscall之后的RCX/R11架构状态。

去掉“任意下一owner消费unknown_thread全局bool”的行为；null/未登记/过期frame按可审计的backend错误处理。若不能可靠归属，明确上下文失效策略，不能把错误悄悄塞给无关owner。错误事件的分配/记录失败也必须有定义，不能从C++跨JIT抛出；边界内部异常须收敛为错误。完整native异常及FP/errno/pin矩阵仍须R2验收。

正式测试建议使用尚未占用的G33及以后ID，接手先核查。每个ID同提交接入ownership、ROUND2_MAP和N1退出码负例；G30维持bare-HLT语义。R2-H05保持CLI auxiliary与正式目标环境状态区分。

| 矩阵 | 最低次数与断言 |
|---|---|
| unknown、registered拒绝 | 各100次，native count/sentinel/PC/category/identity正确 |
| 合法HLE正例 | 100次，合法owner返回和后继store正常 |
| 错误owner与合法HLE owner并行 | 100轮，不同operation/buffer/TLS sentinel，无错误串owner |
| 错误后guest自跳loop | 100次，无外部Cancel，返回≤1秒；每轮耗时可见 |
| Fault与Pause/Cancel/Shutdown交错 | 每种至少10次，通过barrier确认实际交错；fault优先于其后的中断，先前中断未执行syscall时不伪造fault |
| fault后Run/Resume、销毁重建、旧frame/handle | 各10次，拒绝错误恢复，重建不继承旧事件，旧身份不能污染新线程 |

每个拒绝点核验清理：pin/lease/frame绑定/后台状态回到定义基线、无后继副作用、无process abort；错误线程仍按当前契约要求销毁重建。trace记录invocation归属，不把错误存入无人读取的context级last_hle_error_。

验收：N3/N4全部实际探针进入正式suite，runner能够对每一个新增FAIL返回非零；保留失败前后对照及完整canonical runner结果。此时才能标记“R1 CLI已验证”，不能顺带关闭FP/H2全矩阵或H3。

## N5：证据、状态与提交交接

每个批次固定源码/fixture/runner/FEX静态库SHA、实际编译配置、binary Build ID和部署SHA；构建或部署变化就开启新批次。运行前核验device SHA，记录每次exit、timeout、总检查数、唯一ID数与组合次数，所有失败轮次保留。不要续用只检查`[g24] FAIL`的verify.sh作为门禁。

优先用canonical runner，完整运行1–3次；真实目标设备不可用时，按AYN Thor/API33/CLI写明，Swan/API36/APK继续NOT_RUN。不要补写历史身份或把60份旧日志宣称成新构建验收。

交付至少包含：

- N1/N2/N3/N4源码与正式回归，R1栈/状态出口设计，实际测试记录及runner JSON。
- progress/AGENTS/CLAUDE/docs索引同步到**实际**HEAD；旧G24报告加勘误入口，旧失败证据保持。
- 本次及前序关联spec/审核/证据的明确Git交付清单。执行提交时显式stage相关路径，检查parent与submodule状态；不要`git add .`混入Windows/Vortek研究、Bachata子仓本地变化、externals临时目录或build产物。
- H2/R0以及本轮提交的远端状态。发生push时核对remote ref与所交付提交；若只完成本地提交，就明确标注不能在另一机器完整检出。

## 本轮完成后的顺序

1. **R2：真实host/guest FP、异常和errno边界。** 延续原spec全舍入组合、native修改FP、持pin抛异常、清理与native观察host环境测试。
2. **R3：完整ABI、buffer policy、TLS及typed registry。** guest真正构造8整数/9double/混合栈，真实乘法/地址/范围三种溢出分开；In/Out/InOut/nullable/zero/max/alignment/并发pin矩阵。
3. **S0→S3：单层callback原语证明，再HleScope/InvokeGuest、两层嵌套、可取消WaitingHle。** 普通HLT return gate不是FEX callback返回协议。R1/R2/R3过关前不开始这些实现。
4. **G2 Q1–Q3与G4显式保留。** Query/decoder边界和生命周期矩阵不因HLE推进划掉；Swan Android16普通APK/JNI/ART/Foundation生命周期与package闭包另行验收。

本轮完成状态应为“R0收尾 + R1 CLI通过，R2/R3/H3/G4待办”，仍是V0_IN_PROGRESS。
