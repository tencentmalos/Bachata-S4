# 执行 spec：N4 修复到 Swan 普通 APK 验证

日期：2026-09-11。状态：**SPEC_READY；实现仍为 ROUND2_IN_PROGRESS / V0_IN_PROGRESS**。

先读 [本次复核与反例](../validation/round2/g3-n4-review-2026-09-11.md)、[证据入口](../validation/round2/g3-n4-review-2026-09-11/README.md)。
本文件确定接下来各批的顺序与交付，保留 [Round 2 的24项验收](android-fex-round2.md#5-二周目验收清单)、
[R2/R3/S0–S3完整要求](android-fex-round2-g3-repair-h3.md)及 [G2 Q1–Q3](android-fex-round2-g3-handoff.md#1-首个交付补齐-g2-剩余精确矩阵q1q3)。
旧文档的历史状态不再作为当前事实；不得把其中尚未完成的矩阵改成可选。

## 0. 接手基点与目标

| 项目 | 已知事实 / 本轮要求 |
|---|---|
| 被审源码 | `e0693faad1ac0cbeb97ae3da39655207abb7aef2`，`codex/android-fex-round2` |
| 远端观测 | 2026-09-11 origin仍为`0e10defc04457e019c07fd2a78d4f58d506133b2`，本地领先24提交；接手重新核实 |
| FEX / Foundation | 分别固定`385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842` / `1f7008848736b7c6c0779220480344fd5b5fc5e3` |
| 已有执行 | 当前正式adapter的AYN Thor/API33/4KiB CLI：114唯一guest ID / 212 checks全过，device contract43/43 |
| 不能忽略的反例 | runner22项中2失败；合法HLE改坏RCX；native看到guest舍入；native throw使进程exit134；gate无pending超时放行及早Release终态翻转 |
| 已关闭的旧缺陷 | 正式non-spill出口的unknown状态probe，16 GPR与16 XMM无误；旧Release污染新Arm反例100次未复现 |
| 尚无实现 | HleScope/InvokeGuest/WaitingHle、主仓JNI shared target和普通APK；`android/validation`空目录不构成app |
| 产品架构 | native ARM64 shadPS4 host；FEXCore只运行x86-64 guest；本轮使用自有fixture |
| 目标环境 | Swan / Android16 API36 / ARM64 / **4KiB**普通app进程 |

先确认parent/child状态，保留未提交研究和子仓工作。读 [事务加固](../validation/v0/transaction-hardening-2026-09-08.md)、
[失败保护](../validation/v0/followup-poison-2026-09-08.md)、[子仓归属](../subrepository-ownership.md)后再改实现。
优先修改主仓；不改FEX/Foundation pin，不默认向限制AI代码贡献的FEX reference写代码。

**两项交付分开：**

1. **首个验证APK**：普通Activity加载同一backend，进程内测页大小/身份，UI输入经guest与真实HLE返回，loop可停止。
   不等待两层callback完成；其余项明确未验收。
2. **完整G4 APK验收**：G3完成后，在Swan同一个普通验证APK内重跑规定CPU/HLE/VM项，加上生命周期、ART、Foundation、只读snapshot和soak。
   只有24项全部满足原语义、环境及次数，才可报告ROUND2_ACCEPTED；V0仍IN_PROGRESS。

本轮不加入16KiB runtime适配、finite Step、Vulkan/Surface、Orbis游戏loader、图形/音频、VR、Wine或Vortek。
首个APK是后续产品app可复用的native session基座，不能称为已能运行PS4游戏的Android版。

## 1. 执行顺序与依赖

| 批次 | 必须交付 | 开始条件 / 出口 |
|---|---|---|
| E0 | runner、gate反例修复；可交付Git基点 | 立即开始；自测零失败，timeout/terminal协议有确定证据 |
| E1 | 返回字段隔离、N4错误归属及完整矩阵 | E0；不能用G36的无syscall循环替代HLE并发 |
| E2 | 每crossing FP/errno/异常、完整ABI/buffer/typed registry | E1；分E2-a单次边界与E2-b完整矩阵提交 |
| A0 | 主仓APK工程、共享backend、基础输入/执行/Stop | 编译接线可立即做；运行验收需E1的RCX修复和E2-a通过，不等H3 |
| E3 | S0单层证明→scope栈→两层callback→WaitingHle，回补G2 Q1–Q3 | E1/E2通过后进入；不直接跳到两层框架 |
| A1 | JNI/session、ART/信号、Foundation、snapshot | A0后推进；需要callback的测试待E3汇合 |
| A2 | clean构建、Swan普通APK完整24项、10分钟soak、证据交付 | E0–E3、A0–A1行为完成；失败不降门槛 |

实现者可先完成A0编译接线来暴露平台问题，再继续CPU工作。这里的两条工作线是依赖划分，不要求创建新AI任务。
每批先保留反例，再修实现，最后归档指定回归；不要每修一条就把全套重复运行次数当成尚缺专项矩阵。

## 2. E0：恢复可信 runner 与 gate 协议

主要位置：`scripts/android/run-v0-tests`、`tests/guest_cpu/test_v0_runner.py`、`src/core/guest_cpu/fex/test_run_gate.h`及Run的gate收尾。

1. 把G34登记到正确suite ownership；G32c加入后8个失败子项应按实际输入计数，修仍写7的旧断言。
   增加映射表完整性检查，防止新case有ROUND2映射却无owner。继续验证单项FAIL+exit0、missing、SKIP、重复冲突、crash、timeout和未知FAIL。
   不能删除失败断言或改成仅检查总suite exit。当前实际的22项/21项两套自测均须零失败；新增项按新数量报告。
2. gate明确区分**合法Release、提前Release的abort、owner timeout**。同票据的当前态与归档态必须返回相同终态，错误不能被owner改写成clean。
   将本次`gate_early_probe.cpp`反例迁入正式协议测试，覆盖新Arm前后重复Release、迟到Release/WaitExited、旧票据不能改新代。
   不用sleep制造指定交错；采用barrier和实际generation/terminal观测，历史保留策略必须有界且过期语义明确。
3. WaitAtEntry失败不能默认授予进入guest的权限。定义正常停止/失败收尾，释放lease、恢复中断页并完成属于本次Run的pending receipts；
   有外部Pause/Cancel/Shutdown时保持优先级和请求epoch，没有外部请求也必须自行有界返回且guest零推进。
   若使用内部interrupt收尾，明确其归属及退休规则，不消费别人的请求，不再以“也许已经有pending位”为前提。
4. 正式测试覆盖有pending Pause、无pending、提前Release和timeout/新请求交错；测试helper的5秒期限不等于生产Stop的1秒预算。
   outer watchdog只负责失败清理。额外Cancel后才结束，必须判timeout分支失败。

出口：两套runner自测通过；host gate协议及真实adapter timeout正负例通过；tests=OFF无gate/trace，生产wrapper仍存在。
保留G1/G2控制回归，尤其不能恢复SleepThread parking、host SIGILL恢复或settle sleeps。

## 3. E1：N4收口，并修正常HLE状态

### E1-a：参数视图与架构回写

在`fex_context.cpp`/typed adapter明确一份只用于解码的参数视图及一份显式返回字段集合。
R10→RCX的第四整数参数正规化不得覆盖syscall之后的RCX；R11、RSP、callee-saved及未返回的XMM/GPR保持约定状态。
不能以删除RCX断言或所有syscall一律停掉修复。

把本次`hle_probe.cpp`的unknown/valid状态检查迁入正式fixture suite：先确认初始化真的生效，再比较16 GPR/16 XMM与有效位。
unknown停在fault PC、sentinel=0；valid返回41、sentinel=42、RCX=syscall successor、R11符合定义的syscall语义。
加强G33a StopReason、G33b返回值和G36有界错误处理；不再对1ms的WaitStopped结果直接.Value()。

### E1-b：错误事件生命周期

保留已落地的context/thread/generation/invocation/operation/guest PC/category/system_error，增加明确事件种类及参数拒绝详情。
事件必须有合法归属/生命周期，不能以`last_hle_error_`全局槽代替参数错误，也不能让unknown/null frame的全局bool由下一任意owner消费。
对无法归属的backend异常，定义context失败及后续准入策略，保留可读诊断；不要制造一个无来源的guest fault塞给健康owner。
异常记录自身应有低分配或预留策略，避免处理bad_alloc时再次分配并逃出边界。

frame登记/注销、Run开始/结束、fault后Run/Resume、thread重建及未来nested invocation都有独立规则。
null/未登记/已退休frame可经受控test-only seam检验，不伪造公共路径已触发的证据；tests=OFF排除该注入能力。

### E1-c：完整矩阵

| 场景 | 必须断言 | 最少次数 |
|---|---|---|
| unknown、registered拒绝、合法HLE、错误后self-loop | reason/PC/返回/后继sentinel；错误loop无需Cancel且≤1秒；有效状态正确 | 各100 |
| 两个真实HLE owner重叠 | 健康native用barrier保持活动，另一owner触发fault；不同native TID、参数/buffer/返回/计数/TLS各自正确 | 100轮 |
| fault与Pause/Cancel/Shutdown | 每种请求在fault边界前/后受控交错；fault优先，其他pending/receipt可追溯 | 每组合10；G2/G1原100次要求另保留 |
| 部分pin后拒绝 | 第一参数成功pin，后续参数失败；native未调用、sentinel=0、pins回基线 | 每失败位置10 |
| fault后Run/Resume/重建/旧身份 | 原fault不得绕过；旧handle/frame/事件不影响新generation；销毁资源回收 | 每类10 |

并发证据至少记录进入/退出native的phase、两个owner/invocation及barrier释放次序。
现有G36继续作为“健康JIT不被误停”回归，不能改名当成HLE/HLE重叠。保留G35串行生命周期证明并准确命名。
在E2产生的native异常也接同一事件/清理路径，不另立context全局异常槽。

## 4. E2：完整typed HLE基础，先修单次crossing

### E2-a：FP、errno、异常边界

按 [既有R2](android-fex-round2-g3-repair-h3.md#r2真实-hostguest-fp-与异常边界)实现：

- Run进入guest之前保存真正owner host fenv；每次native入口装入host环境，返回guest前恢复该invocation的guest状态。
  syscall中的fegetenv读到guest环境，不能当作已经保存host。区分FPCR/FPSR、有效MXCSR及异常标志的归属。
- RAII覆盖解码、pin、分配、native调用和返回编码。捕获native异常及分配失败，先完成C++清理，再经已验证的non-spill错误出口返回；不得跨JIT unwind。
- 明确host errno保留/修改政策、所支持的Orbis错误映射及guest TLS位置。按owner/invocation保存，不能共享一个context errno。
- 测host/guest所有不同舍入组合、异常标志、native主动改模式；native内观测host，guest后续实际执行舍入敏感指令，Run后host恢复。
  每类10次；native throw、持pin throw、解码/返回编码失败均须无abort、无后继store、pins回基线且可销毁重建。

出口：本次valid/FP/throw三个实际反例均成为正式通过的回归，错误信息仍归正确owner。
**这一步加E1-a后即可交付A0基础运行APK；不必等待下面完整ABI矩阵与H3。**

### E2-b：ABI、buffer policy、typed registry

完整遵守 [既有R3](android-fex-round2-g3-repair-h3.md#r3补齐-h1h2-契约与真实测试)：

1. 真实guest汇编设置8整数、9double、混合参数和stack spill；区分SysV call/Orbis syscall veneer，验证栈对齐/red zone、callee-saved、GPR/XMM返回，每类10次。
2. 以有类型、带生命周期检查的注册入口替换公开void*/unchecked static_cast；错误backend/context/stale registry明确拒绝。
   不支持的aggregate/varargs/host pointer return在注册时拒绝；guest pointer return有明确地址空间/nullable检查。
3. 显式In/Out/InOut、nullable、零长、element size/alignment、最大字节/元素数。先完成全部检查和pin才进native；pins属于本次crossing并在所有出口释放。
4. 每类10次：有效读写/零长/null/最大长度、只读Out、跨unmapped/过期range/错签名/未对齐；
   分开真正`count > UINT64_MAX / sizeof(T)`、pointer+bytes溢出和reservation越界，不能同一输入声称三项覆盖。
5. native持pin的barrier与另一owner的protect/remap/quiesce竞争：真实pins、Busy/Timeout、字节不提前变化、结束后可变更均要观测。
   仅execution lease挡住映射不证明pin有效；每个失败点检查call count、sentinel和实际pin基线。
6. 普通Run的重入拒绝须考虑同一native owner上的其他handle，不能因为entry不同就覆盖单槽`t_binding/t_syscall_original`。
   此处建立拒绝边界，受控嵌套只由E3的scope路径开放。

## 5. A0：尽早交付第一个普通验证APK

### A0-a：工程、依赖与工具链锁

新建`android/fex-validation/`和`src/android/validation/`，使用普通Activity及JNI `shadps4_fex_validation` shared target。
复用`cmake/fex`的`guest_cpu_fex`，不能复制FEX链接列表、把CLI ELF作为外部运行服务，或引入参考工程的glibc runtime安装器。
参考Android源码只迁移必要的UI/session组织，不复制整套工具链和运行时。

建立可由新checkout重建的lock与build入口，记录JDK/Gradle wrapper/AGP、SDK/build-tools/NDK实际metadata/Clang/CMake/Ninja、native API、STL和gitlinks。
选择并验证支持SDK36的官方工具组合；reference中的AGP9.1.1/Kotlin2.4.0/SDK37不是当前主仓已经验证过的组合。

默认Swan profile为minSdk/compileSdk/targetSdk=36。native工具链有两条明确路径：

- 如已验证真实API36 sysroot，则按该native API走统一externalNativeBuild。
- 本机当前NDK目录`29.0.14206865`的metadata实际max=35；允许采用Round2明文许可的**native API35**。
  若Gradle把app minSdk36传给不支持的NDK，改用锁定API35的规范CMake source build产出JNI，再通过有依赖/输入hash校验的Gradle任务打包其输出。
  不能只改目录名、伪造API36 sysroot，或手工复制一个不知来源的旧.so；此profile也不能标成V0的native API36构建已通过。

Android的compileSdk不决定NDK可用API，native最低版本与targetSdk行为需分别验证；在高于设备API构建的库即使偶然运行也不构成兼容保证。
参见[官方NDK版本说明](https://developer.android.com/ndk/guides/sdk-versions)。
如果只持有当前AYN/API33，可另建明确minSdk33的辅助profile，并将全部native依赖重建到匹配API；它不替代Swan最终验收，也不是A0默认要求。

shared库链接必须核验：FEX及所有静态依赖PIC；STL/exception/RTTI/allocator边界统一；建议JNI/Foundation多DSO统一一个`libc++_shared.so`。
不要直接继承CLI的每个DSO静态C++运行库策略。审计实际DT_NEEDED、符号版本、导出可见性及allocator分配/释放归属。
保留已有≥16KiB ELF对齐，分别检查ELF与ZIP打包规则；这只保留现有构建要求，不新增16KiB运行验收。
参见[Android官方打包与对齐说明](https://developer.android.com/guide/practices/page-sizes)。

### A0-b：最小进程内闭环

复用已通过的基础fixture：UI输入整数/有界数组→native复制入guest buffer→真实x86-64执行→typed HLE→guest读取结果→UI显示。
另有无HLE loop的Start/Pause/Resume/Stop和已发布snapshot摘要。Step能力为None，不显示可用按钮。
界面显示实际结果、状态和失败原因，不把内部构建步骤变成使用流程。

native session使用不透明handle+generation，控制端队列与两个持久owner分离；owner负责Create/Run/写寄存器/Destroy。
UI/JNI控制调用有界，Stop走已实现的异步interrupt，不在UI线程等待仍运行的guest join。
基础fixture和loop各10次，Stop热身后每次≤1秒；失败保留结果，不能靠重启后PASS覆盖。

app自己报告page=4096、UID/PID、device API/target SDK、实际加载库Build IDs、fixture SHA和backend capability。
输出实际输入与返回值，不能只验证Activity启动或UI计数变化。首次APK里未完成的矩阵标NOT_RUN。

将正式suite逐步提取为CLI/JNI共用的可调用测试函数与结果sink；CLI main只是前端。
不得把本次包含`guest_execution_tests.cpp`并改名main的review probe方式变成产品集成，也不得把`exit/_Exit/abort`式测试清理直接塞进app主测试进程。

交付：可重建APK及符号路径/SHA、安装/启动命令、source→package→loaded library身份、10次基础记录、明确的剩余项。
如没有Swan，构建成功可以交付APK文件，但运行结果仍NOT_RUN，不能据此关闭G4。

## 6. E3：H3分层实现与G2交叉收口

完整要求沿用 [S0–S3](android-fex-round2-g3-repair-h3.md#s0选择-callback-执行方式先做单层独立证明)，必须逐个出口验证。

### S0：先证明一次callback能正确往返

优先评估在guest合法gate显式退出后，由owner普通栈调度native HLE，再恢复guest的方案。
当前unknown/fault的终止出口不自动等于成功HLE continuation；需单独记录successor、返回字段、pending call和恢复点。
若使用FEX HandleCallback，则先证明专用callback return、ReturningStackLocation、callret_sp、SignalHandlerRefCounter、InSyscallInfo和binding的配对清理。
普通HLT return gate不能直接当CallbackReturn。

先提交一页所选路径/栈/清理设计，随后单层native→guest→native最小真实证明100次；检查guest返回/副作用、outer状态和资源计数。
再注入inner fault/cancel，证明回到正确层；两者通过后才写两层结构。保持固定FEX pin和贡献约束。

### S1/S2：scope与invocation栈

- 有界HleScope绑定owner/context/thread generation/invocation/parent/depth/liveness；拒绝WrongThread、错误context、stale/过期scope、普通Run重入、超depth，且无状态/计数/pin变化。
- 每层保存有效架构字段、FS/GS/TLS/errno、独立guest stack/guard/red zone、continuation、FP、pending error/cancel、pins及backend bookkeeping。
  明确single TLS binding槽如何入栈/恢复；不得memcpy含backend-owned指针的整个CPUState，或临时放开全局准入绕过重入约束。
- 两个owner各1000次真实HLE，至少100次guest→HLE→guest→HLE→guest两层嵌套。
  每层都有不同sentinel，guest主动读取TLS；日志可还原invocation树，内层fault/exception/Cancel保留发生点和归属。

### S3：WaitingHle与发布

状态机显式可观察InHle/InCallback/WaitingHle或等价状态。等待使用cancel-aware原语，注册契约声明可取消条件；不能承诺打断任意阻塞C++函数。
100次native exception / WaitingHle Stop / inner Cancel / rebuild混合，Stop≤1秒，逐层清理pins、bindings、FP、backend引用，旧scope不影响新session。
timeout不伪报stopped，也不销毁仍活动的frame。

R2-M04必须是guest真实store→HLE边界→controller ExplicitPublication→原target owners执行新字节，至少10次。
禁止owner持自己的execution lease/pin同步等待包含自己的全context drain；需要明确让出、关闭准入及继续执行协议。
保持G27真实store、shared/local/decoder在变更前退休、poison和内部Pause退休的既有语义。

回补 [Q1–Q3](android-fex-round2-g3-handoff.md#1-首个交付补齐-g2-剩余精确矩阵q1q3)：
实际FEX range/decoder的邻接/NX/unmap/remap；跨mapping/space销毁token；普通Protect/Unmap sink与pin/drain；
G24的Pause/Cancel/Shutdown×前后次序各100次，warm owner身份和request/receipt/stop/token epoch逐轮可追溯；
G25/G26 mapping/code generation、耗时、协调者ack及同一handle/TID状态证据。已有结果可复用明确覆盖项，不能把总100当每类100。
最后将WaitingHle/InvokeGuest接入R2-M02/M05：迟到ack、sink注销、部分停止、poison后的Run/Resume/InvokeGuest全部遵守封锁与恢复。

## 7. A1：普通app生命周期、ART与Foundation

### A1-a：JNI与session所有权

JavaVM可共享，JNIEnv只属当前native线程；需要调用Java的native worker正确Attach/Detach。
异步引用有明确GlobalRef所有者/释放点，不传递跨线程LocalRef；不持有GetPrimitiveArrayCritical跨JIT/等待。
输入优先采用有界复制，Java内存不冒充guest映射。[Android官方JNI建议](https://developer.android.com/ndk/guides/jni-tips)

销毁顺序必须写入设计及实现：关闭session新请求和诊断注册→Cancel/唤醒owner→排空in-flight/回调→销毁thread/context/address space→释放JNI refs。
排空不能持owner需要的锁。旧callback携带generation，不可写重建后的session。
验证APK可明确选择后台/Activity detach时停止并关闭session，回来创建新session；不要假装已实现游戏状态保持。
独立Pause/Resume控制仍按CPU契约验证。

100次create/run/stop/destroy；前后台、back、Activity recreate覆盖，其中前后台/recreate各20次；
owned threads/maps/FD/JNI refs回到热身基线，记录RSS趋势而不是要求系统分配器RSS精确归零。

### A1-b：ART、bionic、信号与allocator

Java分配/GC/回调和guest JIT并行；真实guest guard fault有正例，无关native signal必须保持previous/default语义。
预期crash在Manifest声明的私有`:fault_probe`进程执行，使用**同一app UID**；不要用改变UID的isolatedProcess冒充主app身份。
检查真实signal/exit，不让instrumentation因失联而报告PASS；每个故障进程由外部supervisor有界清理。

审计FEX信号安装/转发/恢复与ART及多session重建的共存、bionic TLS和allocator初始化/退出。
不在handler中加普通mutex、Foundation或分配；不以root、Termux、SELinux调整、LD_PRELOAD/glibc wrapper掩盖普通app问题。
跨DSO异常/分配释放使用锁定的一致运行库配置，不能盲导mimalloc TLS slot。

### A1-c：Foundation和只读观察

通过现有`cmake/SpatialFoundation.cmake` / `shadps4::foundation`接DebugBus status/capabilities/pause/stop与结果查询。
控制只投递请求，100次诊断中关闭/重启，registry排空后才销毁session。TCP=false。
reflection/packing用实际NDK依赖完成诊断payload round-trip、截断/错误schema；核实Crypto++/fmt/mimalloc等实际模块所需闭包，
不建空target或另造反射/网络框架，不能将尚未启用的能力报告为已接通。

增加锁外的immutable SafePoint snapshot observation hook，发布后才允许LLDB读取。
执行到此项先读仓库 [LLDB工作流](../fex-lldb-host-guest-workflow.md)及适用native-debugger skill。
用匹配**该APK**加载库Build IDs的符号，在hook读取thread/generation/epoch、可靠寄存器、映射和guest RIP字节并离线解码。
异步JIT stop不能直接将CPUState标成有效，也不evaluate inferior helper；不扩展guest单步协议。

## 8. A2：Swan最终APK验收与可信证据

### A2-a：一个明确APK身份，两个构建用途

最终24项的app运行使用同一普通`validationDebug` APK/目标进程；允许此验证构建启用明确的test-only注入，以覆盖可控竞态及故障。
它必须链接生产wrapper/同一backend，不能只链接诊断shim或手工seed的替身。
另产`V0_BUILD_TESTS=OFF`的APK/库，验证无gate/trace/注入符号，生产wrapper存在，执行基础正例、unknown/rejected出口与Stop smoke。
二者记录各自source/构建/部署身份；OFF smoke不替代前者的完整矩阵，也不混写成同一个APK已经跑过所有检查。

默认app minSdk36，因此最终运行需要实际Swan/API36/4KiB；构建机有SDK36不代表已经有目标设备。
debuggable用于instrumentation/LLDB即可，无需系统签名、特殊UID或平台特权。APK无需游戏或外部runtime。

### A2-b：instrumentation与runner

提供仓库内build/install/run/evidence入口，更新`scripts/android/README.md`。
instrumentation启动并驱动目标app内测试函数，不能在adb-shell另起ELF再贴上APK标签。
命令行可采用官方[AndroidJUnitRunner/instrumentation流程](https://developer.android.com/studio/test/command-line)，
但supervisor必须解析实际测试终态、native crash、失联、缺项及timeout；adb命令退出0本身不是PASS。

所有失败/超时保留，按case+iteration+run-id去重且不覆盖；expected-crash用显式预期signal/exit匹配。
延续runner反例：无APK只有ELF、CLI/host混入app、缺suite/子项、SKIP、静默退出、重复冲突、旧SHA、instrumentation失败出口。
每次先写开始事件，再写完成/失败事件；中途crash后能恢复已完成记录和未完成项。

app内报告实际UID/PID、SDK/targetSDK/page、loaded libraries的路径及Build IDs（读取实际映像/ELF note，不只用编译时常量），
APK安装身份、fixture哈希与输入输出。source→native→APK→设备安装/加载四个阶段都有一致性证明。
保留实现/runner/fixture/全部gitlinks/静态库SHA、依赖锁、symbols、stdout/stderr/logcat、exit/timeout和每轮epochs/耗时。
记录SDK与native API的区别；不得事后修改旧run的源码身份。

### A2-c：24项映射与最终门槛

以下为推进映射，次数/细项以 [原24项表](android-fex-round2.md#5-二周目验收清单)及本文件补充矩阵为准，不是新缩减的验收表。

| 正式R2项 | 实现/辅助阶段 | 最终证据 |
|---|---|---|
| B01/B02 | E0、A0 | 独立checkout全量与增量构建，全APK ELF/依赖/ZIP/dlopen；runner负例、B05与受影响桌面构建 |
| C01–C04 | 保留G1；E0 | 同APK双owner、100次控制/每类交错、真实字段使用、拒绝/deadline与epoch |
| M01–M05 | 保留G2；E3 | 同APK100 epoch/100 remap、HLE真实publication10、排他/失败恢复与Q1–Q3 |
| H01/H02/H05 | E1/E2 | 同APK真实ABI/方向长度pin、重叠HLE错误隔离、发生点与清理 |
| H03/H04/H06 | E3 | 同APK每owner1000 HLE、100两层/100混合清理、scope/重入拒绝 |
| A01–A03 | A0/A1 | UI真实输入闭环、生命周期、ART与同UID隔离故障 |
| F01/F02 | A1 | 100诊断生命周期、实际reflection/packing与依赖闭包 |
| D01 | A1 | 同APK符号、LLDB只读snapshot/映射/RIP解码 |
| L01 | A2 | CPU/HLE/输入10分钟、Stop≤1秒、资源基线/峰值/RSS趋势 |

正常fixture至少10次；单例默认10秒watchdog，Stop热身后每次≤1秒，soak supervisor12分钟。
压力矩阵可按原要求设置与总次数匹配的有界总期限，不能改变单次Stop门槛；LLDB人工暂停期间不计停止延迟。
若卡住保存已完成事件并清理，supervisor强杀不计Stop成功。

归档`round2-results.json`24项与独立`v0-results.json`60项；CLI辅助证据保留environment/coverage。
全部实现但唯一缺口是无设备，才允许ROUND2_IMPLEMENTED_DEVICE_PENDING；尚缺H3/JNI/矩阵则仍IN_PROGRESS。
完整G4通过也不等于Vulkan、完整PS4游戏或V0_ACCEPTED。

## 9. 提交、复核与交接

每批交付实现、正式测试、runner登记、原始失败/修复后日志、状态/锁序设计和准确progress。
生产出口仍应在tests=OFF验证；只检查Release构建名不足以证明没有test hook。

当前本地24提交未在origin；本spec及部分关联历史文档也未提交。发布可供另一台机器检出的基点时：

1. 明确列出并stage本任务source/spec/review/evidence/入口，不使用一把`git add .`收进无关Windows/Vortek研究、Bachata-S4脏子仓或临时externals。
2. 被新入口引用的必要未跟踪文档/证据一并纳入交付；不要改写历史run的source/hash。
3. 核对child提交可从配置remote获取；先交付child再改parent gitlink。本路线默认不需子仓源码变更。
4. 交接包含确切repo/branch/commit、APK/符号产物位置与hash、构建/安装/运行命令、已跑/未跑及下一阻塞项。
   push后实查remote ref；本地commit不等于远端可检出。

本次审核只交付复核和执行spec，没有实施上述修复、创建APK或commit/push。
下一次实施先做E0与E1-a/E2-a，同时把A0工程编译接线具体化；不要继续把全部时间留在CLI扩大总checks而推迟第一次普通app加载。
