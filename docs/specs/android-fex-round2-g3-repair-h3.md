# 执行 spec：先修 H0–H2，再验证 H3（2026-09-10）

> 后续入口：`0484a0b9` 的 [R0复核](../validation/round2/g3-r0-review-2026-09-10.md)确认完整CLI一次通过，但runner仍漏R2失败退出码、G24无锁推论未成立。当前先执行 [R0收尾与R1出口](android-fex-round2-g3-r0-exit-next.md)；本文保留完整R2/R3/S0–S3及历史基点要求。

规范：[Round 2](android-fex-round2.md) / [V0](android-fex-v0.md)。事实起点：[H2 复核](../validation/round2/g3-h2-review-2026-09-10.md)和[独立证据](../validation/round2/g3-h2-review-2026-09-10/README.md)。本文件接替 [上一交接](android-fex-round2-g3-handoff.md)，保留其中尚未完成的 G2 Q1–Q3 和 G4 要求。

## 0. 任务边界与当前状态

基点 `56cbc7b1`（H2），前序 `470109f6`（G2修复）、`1f8c7a21`（H0）、`0e10defc`（H1）。复核时 origin 为 `0e10defc`，H2 仅本地提交；接手先核实实际 ref。不要 reset 工作区，不把未跟踪的 spec/报告/历史证据遗失。

| 部分 | 当前证据 | 尚不能宣称 |
|---|---|---|
| G2 | 修复源码已提交；原权限/guest-store 路径有证据 | Q1–Q3 和全部生命周期矩阵完成 |
| H0 | frame→thread fault flag；bare HLT 双 owner 测试 | 未注册 syscall 立即停止、H05 完成 |
| H1 | 6/8 整数、4 double 的真实 syscall gate | 全 ABI、每次 crossing FP、H06 完成 |
| H2 | count 有界 pin、sum=60、三种坏参数不进 native | 完整 buffer/errno/TLS/并发/异常验收 |
| H3 | FEX 有底层 HandleCallback | 主仓已有 HleScope/InvokeGuest/WaitingHle |

当前 AYN Thor/API33/4KiB 两次完整 suite 都是 202 checks 中 1 FAIL（G24a/b），contract43/43。独立 unknown/rejected sentinel 和 FP 探针 exit2，native exception exit134。这些失败必须先关闭。Pocket DS 或另一轮 202全过不能覆盖它们。

正式目标仍 Swan/Android16/ARM64/4KiB 普通 APK；本阶段 CLI 按 auxiliary 记账。16KiB、finite Step、Vulkan、游戏/PSVR 后置。复用现有 Foundation 的已启用能力，勿另造网络/反射框架。

**执行顺序：R0 → R1 → R2 → R3 → S0 → S1 → S2 → S3。R1–R3 未过，不实现两层 callback 或阻塞 HLE。** 每步小提交，保存失败日志。优先修改主仓 API/adapter；FEX/Foundation 不改 pin。确需子仓变更时先遵守 ownership/子仓贡献规则，不默认修改 reference。

## R0：恢复可信基点、runner 与失败诊断

1. 核对源码 hash、HEAD/origin、FEX 静态库/fixture/binary 身份；把前序源码已提交这一事实同步到 progress/AGENTS/CLAUDE。列出尚未跟踪的关联 spec/证据，形成明确交付集。
2. 新 G30–G32 接入 SUITE_MAP/ownership/ROUND2_MAP。**G30 仍是 bare-HLT 子项，不能映射成 unknown-syscall H05 正例。** G31/G32 partial 放 auxiliary，正式 H 项仍 NOT_RUN。
3. 每个新 ID 单独 FAIL+exit0、missing、SKIP、重复冲突、suite crash/timeout 均应被正确处理。保留已有 epoch/STORE 和 deployed SHA 规则。新增未映射 FAIL 必须可见，不再依赖维护者手工记住每个新前缀。
4. G24 拆开 `closed/recovered/finished` 的每一个子断言，记录 iteration、外部 reason/次序、coordinator 开始/结束、request/receipt/stop/token epoch、future readiness、准入返回 category、恢复前后 progress。先在完整套件次序复现，再做独立/前缀定位；不得以删除用例、降低门槛、settle sleep 或无限重跑解决。
5. 继续追踪上一 spec 的 G2 Q1–Q3（Query/decoder 邻接/边界/remap、跨 mapping/销毁 token、普通 mutator sink、按组合计数、每轮 generation/耗时）。未执行的项明确 NOT_RUN；不得因为转入 G3 将它们划掉。

交付：新的完整 runner JSON 和 G24 定位记录。若发现生产错误先修；若是测试竞态，用观测/协议条件修复并解释。最多先做三次有编号的稳定性复跑，任何失败保留并继续定位，而不是择优归档。

## R1：错误后立即离开 guest，保留发生点与归属

修改主要在 fex_context.cpp，公开错误结构按需要扩充。目标是 R2-H05，而非把 bool 改成容器。

### R1-a：先验证退出原语

- 复现 unknown operation、注册成功但 buffer 拒绝两条 `syscall; sentinel store`，两者 sentinel 必须保持 0；再加错误后自跳 loop，证明无需外部 Cancel 也能有界返回 owner。
- 固定 FEX 非 Windows syscall **不结束 block**。不能只改 CPUState.rip、等下个 block interrupt、把测试 fixture 改成 `syscall; jmp`，或设置全局 `_WIN32` 来绕过。
- 在实际 pin 上选择一个可证明的 backend 出口：说明寄存器 spill、当前 host/JIT 栈、回到 owner 的位置、C++ 自动对象析构责任。可以先做 out-of-tree 最小实验，但最终要进主仓测试。全局 MAXINST=1/逐指令模式只能作诊断，不能成为未说明的生产解决方案。
- 若需要自己的 ABI trampoline，C++ 边界必须先正常完成清理，再通过核对过的汇编出口返回；禁止从任意 native C++/持锁帧长跳，禁止恢复 SleepThread parking、SIGILL PC 猜测或跨 JIT exception。

### R1-b：完整错误记录

建立按 context/thread generation/invocation 归属的事件，至少包含 operation、fault guest PC、category/system_error、事件种类。对参数拒绝保存具体参数错误，不能只存进无人读取的 context `last_hle_error_`。

unknown/null frame 不得让下一任意 owner 消费全局 bool；明确定义为 backend 异常或可归属错误，保守处理并留下诊断。记录和清理须覆盖重复调用、thread 重建、旧 frame 与未来 nested invocation。

### R1-c：验收

- unknown/rejected 路径各100次，后继 sentinel始终不变、fault PC正确，记录发生点而非最终 return gate。
- 错误 owner 与合法 HLE owner 同时执行，至少100次；正确 owner 的返回/计数不被污染。
- 与 Pause/Cancel/Shutdown 交错，fault 优先；新 Run/Resume 不绕过 fault；重建后不继承旧错误。
- 正常 HLE 后的 sentinel 必须执行，避免测试通过原因是所有 syscall 都被无条件停掉。

## R2：真实 host/guest FP 与异常边界

1. 保存 owner 的真正 host fenv（进入 guest 前），HLE native 调用前装入它；保存当前 guest FPCR/FPSR/有效 MXCSR 状态，native 返回后恢复 guest。两份状态及其 lifetime 明确区分，不把 syscall 入口的 fegetenv 命名为 host 环境就当完成。
2. 用 RAII 覆盖参数解码、pin、分配、native 调用、返回编码；native exception 在 C++ HLE 边界内捕获并转换成定义错误，然后走 R1 出口。不能跨 JIT unwind，不能跳过 fenv/binding/状态清理。
3. 定义 native HLE 的 host errno 影响与 guest errno 的映射/保存，不让一个 owner/invocation 的 errno 混入另一个。若本步只支持一小类 Orbis 错误，显式记录不支持部分。
4. 测试 host/guest 不同舍入的全部组合、异常标志、native 主动修改模式后返回；native 内观测 host 模式，guest 后续真正执行舍入敏感指令，Run 结束再观测 host。每类至少10次。
5. native throw（含持 pin 后 throw）、解码失败、返回编码失败各验证：无进程 abort、无后继 sentinel、live pins 恢复到基线、owner 能正常销毁并重建。先完成单层，再允许 S 阶段嵌套复用。

## R3：补齐 H1/H2 契约与真实测试

### R3-a：ABI 和注册

- guest 汇编真正设置8整数、9double、混合整数/FP及stack spill，走定义明确的 SysV call/Orbis syscall veneer。不能只用 host seed/dummy frame 代替实际调用栈。
- 明确 RCX/R10 的**参数视图**与 syscall 后 RCX/R11 的架构状态；当前把正规化后的整个 RegisterFile 写回会混淆两者。返回只修改约定字段，测试 callee-saved、RSP对齐、red zone、XMM/GPR返回。
- HLE日志包含 context/thread/generation/invocation/operation/signature；每类10次。保留原基础用例，但按实际覆盖改说明。
- 注册接口改为有类型/生命周期约束的入口，拒绝错误 backend/context，避免公开 void* 加 unchecked static_cast；公开 guest CPU 接口保持不依赖 FEX 类型。
- 未支持 aggregate/varargs/host pointer return 在注册时拒绝；若支持 guest pointer 返回，必须验证其属于 guest address space，定义 nullable/范围语义。

### R3-b：buffer policy

定义显式 In/Out/InOut、nullable、element size/alignment、最大元素/字节数、零长规则。建议 nullable+零长允许空 span，无 pointer 解引用；非 null 零长也按一致规则处理，不能悄悄 pin1byte 当作一个元素。Out 是否需要旧内容可读应明确，不再只靠 const 猜全部语义。

解码先完成所有参数的检查和 pin，再进 native。pins 归当前 invocation/crossing，不因复用 HleCallFrame 或嵌套调用累积。失败、异常、取消和正常返回均有明确释放点。

### R3-c：验收矩阵

每类至少10次，并记录每个 native 调用与返回：

- valid读/写/读写、nullable/nonnullable、0长度、最大长度；结果/guard区正确。
- 只读输出、未映射/跨未映射边界、过期range、错签名、未对齐typed pointer策略。
- **真正乘法溢出**：`count > UINT64_MAX / sizeof(T)`；另测乘法不溢出但 pointer+bytes 溢出、以及不溢出但超过reservation。不要以当前0x00ffffffffffffff的uint64元素计数同时声称三类已验证。
- native 用 barrier 保持活动，另一 owner 尝试 protect/remap/quiesce；核验 pin数量、Busy/Timeout、字节未提前变化、结束后可恢复。既有 execution lease 会挡映射，必须同时观测实际 pin，避免“删掉pin仍通过”。
- 每个拒绝点独立断言 native call count不增、后继guest sentinel不变、无残留pin；另验证一参数先pin、后一参数失败的清理。
- 两 owners 独立buffer与FS/GS/guest errno，交错调用不串值。完整 H3 TLS压力留到S阶段。

R1–R3通过后归档“H0/H1/H2已验证子项”，完整目标环境项仍按规范标注。此时才进入H3。

## S0：选择 callback 执行方式，先做单层独立证明

先提交一页设计和最小实验，比较两种方式：

| 方式 | 必须证明的条件 |
|---|---|
| owner 普通栈调度 HLE，guest 在显式边界退出/恢复 | 合法 gate 能在后继指令前退出；pending call/continuation 状态不丢；外层 guest 已退出 JIT 后才运行可等待的 HLE |
| FEX HandleCallback | 专用 callback return gate、host/guest 栈、SignalHandlerRefCounter、callret stack、异常/取消的配对清理与返回目标均已验证 |

优先评估第一种，减少在活动 JIT/C++ 栈内的清理复杂度；这只是实现建议，不是已有能力结论。采用第二种也可以，但以下核验不能跳过：

- 当前 `GetThunkCallbackRET()` 指向普通 HLT return gate，不能直接复用。固定 FEX 的 CallbackReturn 是特殊入口，负责不同的栈调整和引用计数递减。
- 核对 ExecuteThread 的 ReturningStackLocation、CurrentFrame、callret_sp、SignalHandlerRefCounter、InSyscallInfo、t_binding，以及 G1 interrupt stop 到底返回哪层。禁止 memcpy 整个 CPUState 来覆盖其中 backend-owned 指针。
- 专用 guest callback stack 需边界/guard/对齐/参数区/red zone，native owner保持不变；普通return、callbackreturn、fault必须按provenance区分。
- 第一个实验只做“native调用一个guest函数→正常返回native”，有重复100次、guest副作用/返回、outer状态、资源计数证据。第二个实验才注入inner fault/cancel，证明能回到正确层清理。

若现有pin/贡献约束下没有安全出口，提交明确的可复現能力缺口和备选设计，不冒充H3完成，也不绕过子仓规则。

## S1：HleScope / Invocation 栈与受控 InvokeGuest

在主仓API定义有界HleScope和GuestCallFrame/CallResult（命名可沿用规范）。Scope包含owner/context/thread generation/invocation/parent/depth/liveness；不可伪造、过期不可复用。普通Run继续拒绝重入，不能通过清running标志或临时开放global lease实现callback。

每层独立保存：guest有效架构状态、FS/GS/TLS/errno、stack和continuation、host/guest FP、pending错误/取消、owned pins、backend callback bookkeeping及binding恢复责任。只有一个可变快照槽或一个Frame→bool不够。

验收：

- 单层callback正例100次，outer状态和native返回正确。
- WrongThread、错误context、stale generation、过期scope、普通Run重入、超depth逐项拒绝，栈/计数/寄存器/pins不变。
- 注册返回值/内层状态不覆盖外层；每次日志能还原invocation树。

## S2：两层嵌套与独立 TLS

两owner各1000次真实HLE，至少100次guest→HLE→guest→HLE→guest两层嵌套。每owner独立FS/GS/TLS和errno，使用不同sentinel；测试guest主动读取这些值。

每个crossing验证host/guest FP、outer/inner GPR/XMM/MXCSR和有效位；明确允许改变的返回字段。还要覆盖内层fault、native exception及外部Cancel的归属，不能全部折成最外层GuestFault而丢发生点。

## S3：可取消 WaitingHle 与 G2 交叉

新增明确InHle/InCallback/WaitingHle状态或等价可观测状态机。native等待必须使用cancel-aware wait，由请求唤醒，在正常owner栈清理；任意阻塞用户函数无法自动变可取消，注册契约须约束这种能力。

- 100次native exception / WaitingHle Stop / inner Cancel / session rebuild混合，Stop≤1秒；timeout不得伪造stopped或销毁活动frame。
- 逐层清理invocation/pins/binding/FP/backend引用；旧scope/ticket不能影响新session。
- 与BeginDrain/QuiesceContext的准入和lease契约配合：不能持context锁等owner，也不能让owner在保留自己execution lease或pin时同步等待“包括自己”的全context drain。
- 明确定义guest writer→HLE要求ExplicitPublication时的让出/续执行流程，再接真实R2-M04；无需同时释放所有保护让任意Run进入。
- 补WaitingHle/InvokeGuest与R2-M02/M05：迟到ack、注销sink、部分停止、poison后Run/Resume/InvokeGuest均不能绕过；成功修复可恢复。

## 交付和最终门槛

每一步：实现、实际测试、runner归属、完整日志和scope更新一起交付。记录源码/fixtures/runner/固定FEX静态库SHA、Build ID、device SHA、设备/API/进程页大小、exit/timeout；多个失败轮次全部保留。不要把检查次数当独立ID数量。

提交时明确stage源码、测试、spec、报告、progress/AGENTS/CLAUDE/index及关联历史证据；无关Windows/Vortek、Bachata-S4本地修改和externals临时目录不混入。推送后核对实际remote ref，H2这种“本地commit但未push”必须说清楚。

H3 CLI完成后仍需G4：Swan Android16 4KiB普通APK/JNI/ART、Foundation生命周期、包ABI/closure/ZIP、只读guest快照。没有该环境证据则最终项NOT_RUN。有限Step/Vulkan/游戏/VR不扩入本spec。
