# Guest debugger 与 guest 0 FPS：当前审计及定位路径

> 后续实现已完成，见[同日 guest debugger 交付](guest-debugger-implementation-2026-09-15.md)。本文件保留审计时状态与游戏现场证据；其中 Step/RSP/断点的缺口已部分关闭，原白屏根因仍未关闭。

日期：2026-09-15。主仓 `codex/android-fex-round2` / `4da582b7` 加现有未提交改动；
FEX 子仓 `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`。
本轮为源码审计与现场取证，没有修改生产执行路径，也没有运行完整回归。
用户确认触发点是 **按圈／进入 PLAY 后**，不是仅指冷启动慢。

## 结论

当前已经有可用的 **执行控制和安全点状态基础**，但还没有可以直接连接并完整使用的
shadPS4 x86 guest debugger。定位 0 FPS 的近期主线应为：
**不停止进程的 owner/HLE/等待对象记录 + 必要时精确 Build ID 的 host LLDB + guest 只读关联**。
不要先移植 FEX Linux gdbstub，也不要把实现完整单步作为定位卡顿的前置条件。

**guest 0 FPS 只说明统计窗口内没有新的 guest flip，并不等于 guest RIP 为零、
所有 guest 线程停止、FEX 出错或 GPU 死锁。** 场景切换等待、CPU 活跃轮询、
VM/HLE 锁竞争和已经停止提交后的旧帧重绘都可以产生相同表现。

## 1. 这次现场能证明什么

设备 AYN Thor / API33 / ARM64 / 4 KiB，PID22870，startTimeTicks22458922。
仍用私人 Turnip；host/JNI 为 RelWithDebInfo，FEXCore 为 Release。
对应前一轮部署 host Build ID `badb52cd40c31a3bc1f24f35742911f37687369e`，
JNI `3c0867b6790a9c0be1d48367faff0ba9526452b1`；
本轮 native debugger 要求两个 DSO 的精确符号匹配后才 attach。

- 最早取得的截图显示第1轮已经到 TMNT 主菜单，StatusLayer约58 FPS。
  没有捕获原白屏开始与恢复的完整时间，因此不能量化本次停顿持续多久。
- 采集过程中，同一个 PID 的 Session generation 从1变为2、3、4。
  第1轮截图不能与第2轮初始 `guest_flip=0` 拼接成“统计错误”；
  先前口头提出的计数矛盾已撤回，**本轮未确证计数器缺陷**。
- debugger attach 前取得的 ring 主要保留第2轮工作线程。
  主 owner TID25111约1.371秒窗口中，mutex lock/unlock各6467次，
  cond wait413次；累计边界时长分别约176/127/551ms，最长cond wait31.8ms。
  这证明高频同步与条件等待值得优先分析，**scope墙钟时长不是CPU采样占比**。
  此窗口不是被完整标定的第1轮 post-PLAY 白屏。
- 同一未attach ring 中，音频43.225秒内累计消费2,070,592 frames，
  44次设备采样的xrun/reconnect均为0。它排除了该采样区间“音频完全不消费”，
  不排除其他音频/HLE语义问题，也不能替代原白屏现场。
- 第3轮 host LLDB stop epoch3：主 owner26414位于
  `GuestMutexDomain::Unlock → Write → recursive_mutex → pthread_mutex_lock`；
  26504/26506位于GuestKernelSemaphore等待，26505位于AudioOut的Require异常路径；
  GPU工作线程位于PrepareFrame/SubmitExecution/Refresh，Present线程位于
  PrepareLastFrame/SubmitExecution/Refresh。**一次采样不能证明这些wait形成死锁环，
  也不能把一次ioctl解释为GPU长期卡住。**
- detach后第3轮真实计数从3409增长到6698个guest flip/host present，
  说明至少这一段继续出帧。没有据此验收正确画面或 post-PLAY 流程。
- 后续scrcpy renderer返回全黑，缺少同期物理主屏佐证，不能当作白屏/黑屏根因证据。

本次尚未得到“原post-PLAY白屏 → 所等待对象 → 对象生产者为何不能完成”的因果闭环。
不能写成“Oboe修坏了”“FMOD必然死锁”或“FEX执行错了”。

### 调试器本身的扰动

初次默认Xcode adapter与NDK server版本无法证明兼容，工具在attach前拒绝；
改用已有CodeLLDB1.12.0 / bundled LLDB21.1.7 / NDK29 server21后成功。
启动模块事件使默认event cursor过期；随后按精确stop epoch使用结构化thread/stack读取。
这些是工具会话问题，不是guest故障证据。

第一次暂停因恢复事件游标耗时较长；本次全部stop区间必须排除出性能统计。
Foundation Oboe controller当前把“有排队PCM且callback两秒没推进”判为设备失败；
全进程暂停后，controller若先于audio callback恢复，就可能触发这条路径。
第3轮AudioOut异常栈与此风险相容，但**没有取到本次具体error值，不能认定已证明该因果**。
后续长暂停应有明确debug-suspend标记或恢复宽限并单独验证，不能为了调试永久关闭真实失效检测。

本轮session `2d0f9d38692048118ec6fc0fafcfd223` 已cleaned，
工具确认targetAlive=true、startTime不变、TracerPid=0；没有杀游戏或改guest寄存器。
one-shot scrcpy会话均已清理，Swan未操作。

## 2. 当前完成度：分层判断

| 能力 | 本仓当前状态 | 可信边界 |
|---|---|---|
| Run/Cancel/Pause、owner与generation、ticket/stop epoch | 已实现 | 受控停止有receipt；不是任意host暂停都成为guest安全点 |
| 安全点寄存器读写 | 已实现 | ReadRegisters在running时拒绝；Write要求owner、完整停止和正确epoch |
| HLE状态/嵌套回调 | 已实现基础 | HleScope保留thread/invocation/depth；HLE入口snapshot只读，不代表native等待也被暂停 |
| guest→HLE→guest关联 | 运行机制已有，调试视图缺失 | 不能用host backtrace自动替代guest调用栈 |
| 精确guest单步 | 未实现 | FEX adapter Step明确Unsupported，capability为None |
| guest断点/条件断点/watchpoint | 未形成生产调试服务 | INT3翻译和代码失效机制存在，不等于断点插入/命中/恢复/重插完整生命周期 |
| 任意JIT停止的完整x86架构状态 | 未实现 | AsyncJitStop有类型，但不是完整异步恢复器 |
| guest模块/函数/混合栈展示 | 部分原始信息存在，缺统一导出 | loader知道模块，HLE知道操作；未组装成只读debugger registry |
| 外部guest RSP/DAP服务 | 未接入生产APK | 不能把native lldb-server当x86 guest server |
| profiler/等待原因 | HLE作用域与少量GuestPoll已有 | 缺锁/条件/信号量对象身份、持有者和wake关联 |

关键源码：
[SnapshotKind/validity](../../../src/core/guest_cpu/api/registers.h)、
[ReadRegisters/Step/HLE发布](../../../src/core/guest_cpu/fex/fex_context.cpp)、
[HleScope](../../../src/core/guest_cpu/hle/scope.h)、
[HleCallFrame](../../../src/core/guest_cpu/hle/call_adapter.h)。

旧2026-09-07文档中“没有统一Run/停止契约”“尚无HLE嵌套机制”等描述已被后续实现超越；
完整单步、guest debugger transport和异步架构恢复仍未完成。
不应给一个掩盖上述差别的总完成百分比。

## 3. FEXCore能提供什么，不能直接信什么

以下判断针对本地固定FEX提交，不冒充最新上游全量状态。

1. `CPUState.rip`源码明确注明JIT活跃时可能不准确。
2. `GetGuestBlockEntry()`返回当前block tail的起始RIP，仅是块归属。
3. `RestoreRIPFromHostPC()`读JIT header/tail及变长host/guest offset表；
   PC在合法块内时可得到映射位置，否则**静默回退CPUState.rip**。
   外部工具必须把“映射成功”和“fallback旧值”分开。
4. host PC映射到某条x86指令，仍不保证该指令所有副作用处于执行前：
   一个guest指令可能展开成多条ARM64指令。不能直接改RIP回头重跑。
5. 当前普通ARM64 JIT以x28作STATE，但只在确认JIT区域和运行阶段后成立。
   HLE/Android线程/dispatcher/spill中间不能统一套用这个解释。
6. `ReconstructCompactedEFLAGS(..., true, ...)`会写Frame中的PF/AF字段；
   名字像查询函数也不能直接在“只读LLDB分析”里调用。离线重建需输入host GPR/PSTATE，
   并按有效字段分别标注，未知YMM/x87不补零冒充已恢复。
7. 主仓在ExecuteThread退出后清掉 `t_binding` 再进入HLE。因此
   **host停在HLE时t_binding为空是合法状态**，不是“没有guest正在执行”的证据。
   应从HleScope、HleCallFrame及owner registry关联外层guest。
8. `ENABLE_GDB_SYMBOLS=OFF`；LookupExecutableFileSection目前返回nullopt。
   单开JIT命名配置不能凭空得到本仓的PS4模块名/函数级符号。

证据：
[FEX CoreState](../../../references/FEX/FEXCore/include/FEXCore/Core/CoreState.h)、
[Core重建算法](../../../references/FEX/FEXCore/Source/Interface/Core/Core.cpp)、
[Arm64Emitter](../../../references/FEX/FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.h)。

FEX Linux gdbstub还在LinuxEmulation层，依赖其ThreadManager/SyscallHandler/SignalDelegator，
不是链接Core自动得到。当前固定源码仍有：Z/z只Pause并返回OK、G不支持、
p按字节offset解释寄存器编号、thread action没有消费tid来实现选定线程控制。
FEXGDBReader的unwind_frame为空、frame id固定，不能当作混合栈展开器。
见[GdbServer](../../../references/FEX/Source/Tools/LinuxEmulation/LinuxSyscalls/GdbServer.cpp)、
[FEXGDBReader](../../../references/FEX/Source/Tools/FEXGDBReader/FEXGDBReader.cpp)。

[FEX官方crash指南](https://wiki.fex-emu.com/index.php/Development%3ADebugging_Crash)
也走host调试器检查JIT/CPUState的路线；其中端口和固定布局描述有历史成分。
这里的结构布局和能力判断以本地源码为准，不能照抄旧wiki偏移。

## 4. 如何更清楚地划出guest/host边界

不用改变当前正确的执行控制方式，先把既有边界发布出来：

`GuestJit → syscall veneer → ExecuteThread退出 → HleScope → native work/wait
→ 可选InvokeGuest子调用 → HLE返回 → 重新取得执行许可 → GuestJit`

每个owner记录：

- process启动身份、Session generation、context id、guest thread id+generation、native TID；
- 当前阶段：JIT / dispatcher / HLE active / HLE waiting / callback / VM drain / stopped；
- operation id+NID+名称、独立HLE call sequence、invocation/parent invocation、depth；
- 最后guest边界RIP/RSP/FS、host PC（若确实捕获）、snapshot kind/validity/时间；
- 当前wait的对象id+generation、开始时间、相关owner/producer；
- 最近执行/进入HLE/退出HLE/guest提交的时间和计数。

HLE invocation不一定每次跨越都改变，所以**还要HLE call sequence**。
回调必须保留外层waiting记录，不能一个布尔值把外层覆盖。
native线程名只是提示：音频控制线程可继承Guest-1，不能据名称认定它就是主guest owner。

三种PC应并列显示：host PC、guest block RIP、mapped guest RIP。
再分别显示current-safe / boundary-history / block-only / mapped-instruction / unknown精度。
guest调用者返回地址、RBP链、stack scan是不同等级证据；RBP省略、优化或尾调用时不能
把启发式stack scan输出成可靠调用链。深层guest unwind随后用模块DWARF/CFI补齐。

## 5. 面向0 FPS应优先补的一整块能力

优先交付一个“停帧现场包”，无需先做全功能guest debugger：

**A. 不停止进程的owner与wait记录。**
在当前HleScope/FunctionAdapter和各等待原语接入上述身份；
mutex记录guest锁对象与host VM gate分别等待的时长，不能只标一个pthread_mutex_lock。
cond/semaphore记录signal目标、返回原因和wake；VM记录交易epoch、发起owner、
待drain owners、pin数、持有阶段；Audio记录credit/completion/device error；
GPU记录submission/flip/fence目标值和实际完成值。
采用预分配记录及有界导出，不在热路径格式化字符串、分配内存或跨owner抓大结构。
发布需满足C++数据竞争规则，不能用普通字段加一个裸seqlock冒充安全读取。

**B. 同generation的停帧触发与留证。**
以新的guest flip停止推进为触发条件，排除尚未首帧、暂停和Stop；
先dump现有ring，保留触发前窗口，再短时记录恢复窗口并自动结束。
每个包带APK/host/FEX/driver身份、generation、事件丢弃计数、
每线程实际覆盖范围和debugger停顿区间。进程级ring不能当作单Session文件，
不同线程1MiB环形窗口也不能当作同一长度。

根据同一时间窗口分类：
- 无新flip但owner/HLE持续推进：追场景逻辑/同步协议，不称FEX停死；
- owner等mutex/cond/sema：建立waiter→object→owner/producer链；
- owner等VM drain：追未到安全点的owner、pin或锁持有者；
- guest提交持续、PM4不推进：追命令处理/label；
- PM4及host submit推进、retire不明：补fence/timeline实测，不猜GPU死锁；
- flip/present持续但纯白：转像素/资源/渲染路径；帧率不是正确画面证明。

**C. 必要时host LLDB只读guest视图。**
从同一session/stop epoch取native线程和匹配DSO符号；优先HLE边界的具名参数与快照。
JIT现场离线读取有界header/tail/offset表，验证代码范围与generation；
不在暂停的app里执行Core helper，不改变target架构为x86。
若只有block归属或旧CPUState，明确降级，不产生伪精确guest RIP。

验收只聚焦这些新观测：已知持续guest循环、mutex真实竞争、条件等待/唤醒、
VM drain、嵌套callback、纯白但持续present、两个Session重启；
测试必须证明分类及身份正确、不会让诊断读取阻塞主线程、不会改变Cancel/锁语义。
再复现真实post-PLAY停顿并给出对象生产者链。不是再铺一轮全部HLE回归。

之后才补guest断点/单步/RSP：使用本仓的owner调度、VM token、缓存失效和stop receipt；
不能用host ARM64 single-step冒充guest单步，不能设置MAXINST=1长期改变生产执行，
也不能通过清空FMOD pending或跳过等待来制造“恢复”。

## 本地证据索引

原始PROF、native栈和日志留在 `build/white-screen-20260915/`（不提交游戏内存/资产）：

- `initial-scrcpy.png`：第1轮主菜单58 FPS。
- `recovered-ring.prof`：实际第2轮、attach前，10,232,630字节，
  SHA256 `20881f4de54a6081ebff35194c8cedf6289bd9d656f8fbf28ad9ffd71d5ad06f`。
  文件名保留原命名，不能误读为第1轮恢复过程。
- `third-generation-ring.prof`：第3轮且受debugger暂停扰动，20,666,238字节，
  SHA256 `7d374e7f0b770302e6d8e9a6f48b5112137da190d3f145c7fc245fe278b57719`。
- `ring-summary.json`、`debugger-samples.json`、`status-after-debugger.txt`。
- 全部native会话审计与清理证据：
  `~/Library/Application Support/spatial_debug_tool/android-native-debugger/exports/shadps4-white-20260915-cleaned/`。

两份PROF容器完整，但线程环形窗口有不成对scope边界；分析时必须保留truncated标记。
未获得真实GPU时间，不做GPU时间结论。
