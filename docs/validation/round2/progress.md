# 当前 Git 交付（2026-09-12 / 7c675280）

图形修复已由091334d3保存，入口/spec由1d411955保存；新增COMMON/bionic适配06bd43bd、进度7c675280。以下“本轮未提交”和旧origin位置是各次历史观察，不是当前代码提交状态。本次按用户要求补交遗漏的研究/spec/原始复核证据，再推送当前主分支；不改历史测试的源码身份。

---

# 当前补充：Vulkan 编译缺口已修，整版默认 Turnip（2026-09-12）

最新：[复核与直接修复](../android-native-host/vulkan-review-2026-09-12.md)，继续 [PKG v2 整版任务书](../../specs/android-native-host-pkg-v2.md)。被审 HEAD `fd3587fd`＋本轮未提交工作区；没有改动/推进子仓。

- 独立完整图形 syntax census 修前97/104；修正真实bionic fallback、平台/头路径和生成依赖后，最终37+67全部生成AArch64对象104/104。
- 两个旧sweep读/tmp/吞失败已修：统一仓库工具，缺文件负例exit1。Acquire一次有限等待，timeout/cancel不误触recreate，Suboptimal先消费图像。新增合同19/0；SessionCore新复跑807/0。
- 用户指定默认Android/bionic Turnip。实际native loader/dispatcher接线未做，当前默认环境配置与旧glibc包不能充当实现；系统vkjson不代替Turnip能力。
- 正式host全链接、FEX/Orbis/VM/信号/回调、Surface/Stop与完整音视频输入、真实PKG真机验收继续按一个整版推进。没有新的APK/游戏呈现验收，不倒改旧V0/Round2结果。

---

# 当前整版复核（2026-09-12 / 0f4fd74b）

最新 [复核](../android-native-host/full-pkg-review-2026-09-12.md) / [PKG v2 spec](../../specs/android-native-host-pkg-v2.md) / [证据](../android-native-host/2026-09-12-review/README.md)。origin实查0e10defc，本地领先43提交。用户将下一版终点提升为真实PKG的完整生产链路和设备可交互验证；内部HN步骤可分批实施，但不再逐个停下来另作规划。

- 主干HN0重构、RasterizerHooks、NDK time/UUID、AAudio/Android Surface源码已存在；完整host link/loader/Orbis/renderer仍未进APK，当前仍为CPU循环。
- 本次真实重跑：host生命周期767/0，runner23/23，JVM runtime92+session11+library6=109/0。五个新增边界probe揭示早取消Destroy重叠、Prepare/Run throw abort、控制异常泄漏lease、迟到drain永久未回收。不是把旧裸JNI实现原样列作未修。
- Service十秒整场运行超时、WSI timed acquire中的无限retry/取消未接通、实际rpmalloc与allocator审计不符均纳入整版修复。Mac已实证Darwin NDK全链接小型Android so；完整host闭包不能只交syntax-only。
- PKG只读核实：指定TMNT1.08为gp更新，本地有匹配gd/1.00本体；另一份1.08全文件hash相同。未解包/新装包/新跑设备，AYN仅枚举在线。历史HN-U01不能用于证明allocator失败retry或完整设备矩阵。
- 继续保留G2/G3/H3和Swan目标；PKG v2不倒改旧Round2/V0验收，也不把AYN成绩替代Swan。

---

# 当前补充：Android native host 评估（2026-09-11 / 9ac6c300）

最新：[评估](../../android-native-host-assessment-2026-09-11.md) / [host-native v1 spec](../../specs/android-native-host-v1.md) / [独立证据](../android-native-host/2026-09-11/README.md)。origin 实查 `0e10defc`，本地领先36提交；以下旧记录按其各自源码/产物身份保留。

- **已有增量**：E0/E1-a/E2-a 基础修复、A0 验证 APK，以及 `android/shadps4-app` Kotlin UI、zlib PKG native 和进程内 FEX 固定循环。旧文档的“无主仓 JNI/APK”已过时；完整 loader/Orbis HLE/renderer 尚未接入。
- **本次独立检查**：runner23/23；Android 三模块 JVM test 命令在 core:runtime 测试 Kotlin 编译期失败，未执行完成。检查本地现存 APK 的全部5个 ELF，未新装包/跑设备会话。JNI CMake 实际 API33，FEX prebuilt API35，须统一 profile。
- **证据边界**：手工 Room 记录、临时 exported service 的 adb 固定循环不证明真实 PKG 导入或最终非导出 service 的 UI 闭环。不要把点击失败归因于 tagging/手柄方式而不验证。
- **HN0 优先修复**：Stop 裸 context 生命周期、early Stop/重复 join、generation/readiness/GuestFault 终态；ART allocator 的占位/sleep/释放没有检测静默或保证 SetupHooks ownership；JVM tests/API 依赖闭包。
- **下一批 HN1–HN2**：提取可重启 HostRuntime；统一旧 VMM 与 guest_cpu（64GiB 边界冲突）；真 ELF 经主仓 loader 调用一个真实 Orbis HLE 后返回。随后 HN3 callback/kernel、HN4 WSI、HN5 guest renderer/input/audio、HN6 Swan 普通 APK。新增 spec 不使 G3/G4 或 V0 自动收口。

---

# 二周目进度（2026-09-11 E0/E1-a/E2-a 基础反例已修）

> **N4 复核（[g3-n4-review](g3-n4-review-2026-09-11.md)）的五个确定基础缺陷已全部修复并真机验证**
> （提交 `22891924` E0、`c7b3f46f` E1-a、`e40a70be` E2-a），完整 guest suite **215 checks ALL PASS**、
> canonical guest **117 sub 0 失败**、python 23/23、accounting 21/21、tests=OFF 构建含生产 wrapper：
> - **runner**：G34 补 ownership；G32c 后失败 sub 计数按集合大小（8）；加 mapping↔ownership 完整性校验
>   （referenced 必有 owner）。复核的 22 项 2 失败清零（现 23/23）。
> - **gate 三态协议**：Release/Abort/TimedOut 用 token 键的不可变 disposition 并归档；early Release 不再
>   被 owner 当 clean、重复 Release 跨新 Arm 不翻转（host 11 场景）。WaitAtEntry 失败**不再进 guest**：
>   恢复中断页/清 running，有 pending 走 interrupted finish（PauseRequested/receipt），无 pending 有界
>   BackendFailure 且 guest 零推进（复核 no-pending 反例 progress 7e8→0、Run 自行返回）。
> - **RCX**：R10→RCX 仅解码视图，回写前恢复 guest RCX；G33c（单参 addone + 全 GPR seed）守护
>   rax=41/rcx=successor/r10 保留；复核 valid probe GPR mismatch 0。
> - **FP crossing**：Run 把 owner 真实 host fenv 发布到 binding，HandleSyscall native 前装 host、返回后
>   恢复 guest FPCR/FPSR；fp probe native 现读到 host rounding(1)。
> - **native 异常**：Invoke 包 try/catch，std::exception/...→归属 BackendFailure，走同一立即出口
>   （GuestFault/sentinel=0），不跨 JIT unwind（exit134 消除）；G37a 无 abort、G37b throw 后 backend 仍可用。
>
> **仍待**：host errno 映射；FP/异常/flag 的完整 10× 矩阵与部分 pin 失败；E2-b 完整 ABI/buffer/typed
> registry；两 owner 真实 HLE 重叠 100 轮、fault×Pause/Cancel/Shutdown 交错；S0→S3 callback；A0 首个普通 APK。
> origin 仍 `0e10defc`，这些提交仅本地未 push。

---

# 二周目当前进度（2026-09-11 独立复核）

事实基点`e0693faa`，origin实查`0e10defc`，本地领先24提交。状态仍为**ROUND2_IN_PROGRESS / V0_IN_PROGRESS**。
最新入口：[N4复核](g3-n4-review-2026-09-11.md) / [独立证据](g3-n4-review-2026-09-11/README.md) / [修复至APK执行spec](../../specs/android-fex-round2-n4-to-apk.md)。

- **已确认**：生产常驻non-spill syscall出口；unknown状态probe的16 GPR/16 XMM无误，原10-GPR缺陷关闭。
  正式guest114唯一ID/212checks、device contract43/43、host42/43+1SKIP通过；tests=OFF保留生产wrapper且gate/trace符号0。
- **仍需修复**：合法HLE把R10参数写入架构RCX；native运行在guest舍入模式，native throw仍使隔离测试进程exit134。
  gate在无pending请求时超时放行guest，早Release终态归档后false变true；这是test-helper缺陷，不能外推为生产Pause失败。
- **验收纠正**：当前runner自测22项中2失败（G32c计数期望陈旧、G34漏ownership），accounting21/21。
  G36健康owner跑无syscall循环，未验证两个HLE同时活动；100次/中断交错/部分pin/旧身份矩阵及不可归属frame策略仍缺，N4/H05不能收口。
- **下一步**：E0 runner/gate→E1返回状态与N4矩阵→E2 FP/异常/ABI→E3单层callback、scope栈、两层与WaitingHle；G2 Q1–Q3继续保留。
  A0 APK编译接线尽早做，单次crossing修好即可先验输入/guest/HLE/Stop；A1 JNI/ART/Foundation与E3汇合后做Swan完整G4。
- **环境边界**：本次AYN Thor/API33/4KiB是CLI辅助，native API35；尚无主仓JNI/APK，也无HleScope/InvokeGuest/WaitingHle。
  canonical V0为10 PASS/0 FAIL/47 NOT_RUN+3deferred，24个正式R2项全NOT_RUN；16KiB/Step/Vulkan/游戏/VR继续后置。

本次只新增审核证据、规划和更新入口，未修生产代码、commit或push。下列段落保留历史执行者记录；其中“22/22全过”“H05核心完整验收”“timeout完整关闭”等结论以本次复核纠正为准。

## 历史执行记录：2026-09-10 R2-H05核心完成的原声明

> **R2-H05 的核心场景已全部由正式 guest suite 验收（提交至 `4d8d8003`），212 checks ALL PASS（4 次稳定）**：
> - **G33a** unknown syscall：GuestFault、sentinel=0、fault RIP=syscall PC；**G33b** valid：sentinel=42（出口仅错误路径）；
> - **G34** unknown 自跳 loop：无外部 Cancel、首次 fault 即返回（0ms，预算 1s）；
> - **G32c** registered buffer 拒绝：GuestFault、sentinel=0、native count 不增；
> - **G35a/b/c** 结构化归属：fault 携带 op/context/thread gen/invocation；fresh owner 合法 Run 干净；
>   销毁重建后新 Run 不继承旧事件；
> - **G36a/b/c** **并发双 owner**：一 owner unknown syscall fault 时另一 healthy owner 持续运行，fault 仅归
>   faulting owner；healthy owner cancel 后干净 Cancelled（非 GuestFault）、receipt 归属正确。
> - 生产常驻 wrapper（无 arm 开关，per-thread binding/thread_local），tests=OFF release 含 wrapper 符号、
>   无 gate/trace 符号；canonical guest 114 sub 0 失败、R2-H02/H05 aux PASS；python 22/22、accounting 21/21。
>
> **仍待 R2（FP/异常/errno）与 H05 边角**：fault 与 Pause/Shutdown 显式交错的专项、部分 pin 失败的 pin
> 残留基线、100× 重复计数（当前各关键路径均单次断言 + G36 4 次）。之后 R2 → R3（完整 ABI/buffer/typed
> registry）→ S0–S3（HleScope/InvokeGuest/WaitingHle）。正式 R2-H05 验收仍需 S0 callback 与上述边角。
> origin 仍 `0e10defc`，这些提交仅本地未 push。

# 二周目进度（2026-09-10 syscall立即退出生产化 + N4结构化归属）

> **N4 结构化错误归属与拒绝路径已落地（提交 `18628f61`、`5b0dd002`），正式 suite 209 checks**：
> - GuestFaultInfo 新增 syscall 归属字段（operation/context/thread generation/invocation/category/errno）；
>   owner binding 携带 SyscallFaultEvent，Run 播种 identity（每次 Run 清空）、HandleSyscall 填 fault 字段、
>   BuildRunResultLocked 发布，错误绑定到具体 crossing，不被另一 owner 或后续 Run/重建线程消费。
> - **G35a** fault 携带 op/category/context/thread gen/invocation；**G35b** fresh owner 合法 Run 干净
>   Returned（不继承他 owner 错误）；**G35c** 故障 owner 销毁重建后合法 Run 干净（新 invocation 不继承旧事件）。
> - **G32c** registered buffer 拒绝：GuestFault、后继 sentinel=0、native count 不增（R2-H02"拒绝不执行后继"）。
> - 真机完整 suite **209 checks ALL PASS**；canonical guest 110 sub 0 失败；python 22/22、accounting 21/21；
>   R2-H02/H05 正式 NOT_RUN + aux PASS；tests=OFF 构建通过。
>
> **H05 仍待**：①并发双 owner（unknown 与合法 HLE **同时** Run，错误不串、TLS/计数干净）；②fault 与
> Pause/Cancel/Shutdown 交错；③部分 pin 后失败的 pin 基线/无残留。之后 R2（FP/异常/errno）→ R3（完整 ABI/
> buffer/typed registry）→ S0–S3。正式 R2-H05 仍未验收（当前为 CLI auxiliary）。origin 仍 `0e10defc`，仅本地。

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
