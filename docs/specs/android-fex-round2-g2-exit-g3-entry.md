# 下一阶段执行 spec：G2 出口修正 → G3 真实 HLE

> 2026-09-09 修复更新：E1/E2 的关键缺陷和 E3/E4 的核心回归已修复，完整结果见 [修复记录](../validation/round2/g2-exit-repair-2026-09-09.md)。下一执行入口为 [G2 尾项→G3 交接](android-fex-round2-g3-handoff.md)。下文保留原始要求；已验证/未完成以交接状态为准，不重复移植已完成修复。

日期 2026-09-09；被审起点 `e24aad69e8b174fa9cbebe713918b42838c8ec3f`，分支 `codex/android-fex-round2`。先读 [收口复核](../validation/round2/g2-close-review-2026-09-09.md)、[独立失败证据](../validation/round2/g2-close-review-2026-09-09/README.md)、[Round 2 主 spec](android-fex-round2.md)。本文件取代旧 g2-next 的执行入口，保留其未完成要求。

**目标：先关闭已经复现的 G2 CLI 缺陷，再交付真实 guest→host HLE 与 callback/等待。** 现有 guest 95/95、contract 43/43 可复现，但不足以宣布 G2 出口通过。FEX/Foundation 不改 pin；16 KiB、finite Step、Vulkan、游戏/VR 不加入任务。目标设备仍为 Swan / Android 16 / ARM64 / 4 KiB；Pocket DS API 33 的 CLI 结果继续标辅助。

## E0：固定交付身份和范围

- 核对 git status、origin ref、主仓/子仓改动。793072f2 已包含上次源码修复，不要重复回退/移植。当前旧 spec、修复报告和证据有未跟踪文件，progress/AGENTS/CLAUDE 有工作区改动，必须明确纳入后续提交。
- 保留历史 close report 及其原始文件，新增 corrected milestone；不得补写缺失的历史日志来伪装当时已有证据。
- 优先在主仓 API/adapter 修复。若确需改子仓，先按 ownership 和该子仓规则、通过 gh 核实 tencentmalos owned repo/ref；目前无须修改 FEX pin。

## E1：权限/映射变更必须使旧译码不可达（首个实现任务）

对应 R2-M02/M03/M05，失败复现见 NX/rewrite 探针。

1. 在 token 下撤销 Execute 后，即使原 native thread 保留热 lookup/block link，也不能再执行该 guest range。覆盖 Run 起始 RIP 和执行中的跨 block 跳转，不能只补 CreateThread 或单点 RIP 判断。
2. 选择并记录统一事务语义：权限/映射改变时立即退休相关共享/本地/decoder 缓存，或保持明确的 pending-invalidation 门禁直至失效成功。说明最终 RX/commit 的前置条件，以及 failure poison 与待提交状态的区别；不得用 token 析构清空未完成义务。
3. 检查普通 Protect/Unmap 与 token Reprotect/Remap 是否保持同一约束，避免另一个公开入口仍绕过。涉及 guest reservation 和 return gate 的身份区别不能混淆。
4. 保持 context→space、space unlocked sink、context→FEX invalidation 的锁序；不得恢复 callback 持 space 锁或漏清 live-thread cache。
5. 必需回归：warm A→仅 RW→原线程 Run 拒绝/正确执行 fault；warm A→RW 改 B→RX 无有效失效时拒绝或按明确定义完成失效后只执行 B；正常 Publish/Clear 成功后执行 B；多映射、跨块跳入、被移除 range 的 FEX Query、decoder cache 权限变化；失败后释放 token/Resume/新 Run 均不能绕过。

验收不能只断言 mapping.permission/code_generation。保存实际 guest result/sentinel、失败状态和受控恢复结果。E1 不依赖 HLE，必须在 G3 执行任务前完成。

## E2：纠正 guest store 与持久 owner 验证

### G27 / R2-M04 host-coordinator 子路径

- 两个独立代码区：target 被持久 owner 热执行为 A；writer 保存在另一区域，不通过 LoadFixture 覆盖/失效 target。
- guest writer 实际 store 直接改 target 的指令，host 只负责事务权限和 ExplicitPublication/Invalidate，不用固定 host fixture 覆盖 guest-authored 字节。
- 版本每轮不同，至少 10 次；保存写前/写后实际字节、原线程 handle/TID/generation 和执行返回。禁用 store、改写错误地址或错误 immediate 必须使对应测试失败，避免“第一次留下固定 B 字节”使后续迭代空跑。
- 对 target 的 writable/X 权限转换、失效与最终 RX 在协调事务内完成。保留原 native thread/cache 身份；写入期间如何执行 writer、如何保持 target owners 停止需先定义合法状态，不能允许普通 Run 绕过 active token。
- 如当前 API 不支持“保持 target 停止同时运行 writer”，先增加受约束的执行/发布流程；不要把 host 重建 B image 当替代。真实 guest→HLE 发布 gate 在 G3 补证，完整 R2-M04 继续待完成。

### G25/G26 / R2-M01/M03

保留循环外创建两 owners 的现有进展，补保存/消费每次 Run future，逐轮核验 StopReason、ack、StopEpoch、handle/generation/TID、旧/新版本与 counters。publish/remap 各 100 epochs，写 JSONL 原始记录。完整 GPR/XMM/MXCSR/FS/GS/内存保持单独断言，排除有意修改的 RIP/版本输出区；继续保留 G23 的独立 Clear 验证，避免 Remap/Publish 已失效导致 Clear 空测。

## E3：补真实负例与生命周期矩阵

- M27 用 publisher 持有的同一有效 token，在 barrier sink 内依次测试 Reprotect/Remap/Publish/Invalidate/read pin/write pin；分别以 Publish、Remap 发起回调，再覆盖 unregister/draining。不能靠第二个 Quiesce 被 Busy 拒绝就跳过 token 分支。
- M28 fault seam 明确是 syscall 前注入，不宣称 kernel rollback 已证明。增加失败前后 backing sentinel、permission/mapping/code generation、errno/category、token 释放后 execution lease/实际 guest Run 封锁和成功恢复。测试控制仅在测试构建可用，用 RAII 清理 hook。
- M29 用 ReservationSize 构造真正末端外地址；增加 UINT64_MAX overflow、zero、misalignment、跨 mapping、out-of-reservation，覆盖 Reprotect 和 Remap。move-from 原对象、清空 token、foreign token、旧 token 释放不能开启更新 epoch 分别验证。
- 将协调请求矩阵补到原 N2 要求：100 轮 late ack、外部 Pause/Cancel/Shutdown、部分停止、fault 优先、并发 coordinator、入口已取得 lease 的 Run、Create/Destroy/generation、failed drain retry。WaitingHle/InvokeGuest 实际交错按主 spec 留给 G3，不制造假状态来凑通过项。

## E4：可信结果和 G2 CLI 出口

- 为 G25–G27、M27–M29 及 E1/E3 新 case 加 SUITE_MAP/ownership 与正确 V0/R2 归属。新增 case 单独 FAIL+exit0、整进程非零/timeout、完全缺失、重复 ID、部分 epoch、SKIP 均有自动负例。
- 实际运行并输出 24 项 R2 与完整 V0 两种结果；部分机制 PASS 放 auxiliary，不能把 G25 总 PASS 当成所有 R2-M 项通过。
- fresh embedder build、准确 FEX 静态库身份、source/fixtures/runner hash、binary Build ID、设备部署 SHA、完整 stdout/stderr/exit/timeout/逐 epoch 日志一起归档。设备不可用时明确阻塞，不用本地 SHA 或 tail 替代运行证据。
- 主仓相关源码、测试、spec、报告、progress/AGENTS/CLAUDE/docs index 形成可 clone 的提交集；不混入 Windows/Vortek 调研、Bachata-S4 本地改动和 externals 临时目录。child 改动必须先 push 再更新 parent gitlink。

**G2 CLI 出口条件**：本次 NX/rewrite 两条缺陷关闭；G27 真实来源链路成立；持久双 owner/状态/范围、所支持失败矩阵与 runner 通过；完整证据可追溯。Swan 普通 APK 和真实 HLE 交叉项明确待 G4/G3，不以其缺失否定已经合格的 CLI 子项，也不因此豁免 E1–E4。

## H0：G3 的第一个任务是错误归属与立即停止（R2-H05）

当前 FexSyscallHandler 的 unexpected_syscall 是 context 级标记，HandleSyscall 仅设置标记。先以汇编 fixture 验证当前 sentinel 行为，再做每线程/每 invocation 的错误记录及确定的执行退出路径。

- 未注册 syscall/gate 后紧跟 sentinel store；必须在该 store 前停止。另一 owner 同时做正常操作，不得被错误串值污染。
- 核实边界寄存器是否已 spill，写清 guest RIP、syscall clobber、pending requests、StopReason 的构造依据。禁止用异步 host 寄存器当完整 guest state。
- 保留 fault 高于 Pause/Cancel 的优先级；错误路径不跨 JIT 栈抛 C++ 异常。先提供 no-HLE sentinel 双 owner 验证，再随合法 gate 接通补齐 H05 的正式正例。

## H1：最小 typed gate（R2-H01）

- 先定义注册/operation ID/签名、gate provenance、Run→HLE→guest 的状态与锁序；验证 CPUState 的实际边界和返回地址，再复用现有 HleCallFrame/CallCursor/typed adapter。现有 14 个 host 手工 frame 测试仅作回归。
- guest 实际传 8 整数、9 double、混合参数/返回，各 10 次。断言 RCX/R10 区别、spill 参数、栈对齐、callee-saved/red zone；host 函数调用日志绑定 thread+invocation。
- 每次 crossing 保存/恢复 host 与 guest FP/异常标志；不能只依赖最外层 Run 的 fenv 恢复。

## H2：长度/方向 pin 与错误返回（R2-H02）

显式 buffer 描述覆盖长度、方向、nullable、零长和 overflow；native call 全程持 pin。现有 DecodePointer 的 ValidateRange/sizeof(T) 不能替代完整 buffer lease。每类负例至少 10 次，与另一线程 remap/protect 竞争；错误签名/过期 range 时 host 函数调用次数为零，guest sentinel 不变。返回值、errno/guest TLS 属于当前 owner/invocation，不使用 context 级临时变量。

## H3：受控回调与等待（R2-H03/H04/H06）

- 公共普通 Run 继续拒绝重入；新增受 owner、context/thread generation、HleScope lifetime、depth 约束的 InvokeGuest。记录 invocation 栈、outer/inner snapshot、guest stack/return gate、资源清理责任，避免将嵌套状态写进一个共享单槽。
- 两 owner 独立 FS/GS/TLS，各 1000 次 HLE，至少 100 次两层 guest→HLE→guest→HLE→guest；校验 outer/inner 及 host FP 状态。
- 非 owner InvokeGuest、普通 Run 重入、过期 HleScope、超 depth 明确拒绝，栈/计数/guest 状态不变。
- WaitingHle 使用可取消等待，100 次 native exception/Stop/inner Cancel/rebuild 混合循环；Stop≤1秒，无跨 JIT unwind、残留 pin、旧 scope 或错误串值。此处与 G2 drain 合并补测，避免持 context 锁等待 HLE/owner 形成死锁。

各小阶段形成独立可审提交并保持之前回归。G4 再承载同 backend 的普通 APK/JNI/ART、Foundation lifecycle、只读 LLDB guest snapshot，补 ABI/closure/ZIP package verifier；正式目标是 Swan / Android 16 / 4 KiB。
