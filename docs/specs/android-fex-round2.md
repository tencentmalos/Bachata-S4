# Android / FEX 二周目 spec：可控 CPU、真实 HLE、普通 app 验证

状态：**SPEC_READY / 尚未实施**。日期：2026-09-08。“二周目”指第二轮实现，不是两周工期承诺。

目标：在 **Swan / Android 16 / ARM64 / 4 KiB** 的普通 app 进程中，使用现有 NDK/bionic FEX backend，跑通两个 guest owner 的暂停/恢复/取消、跨线程发布、真实 typed HLE 与嵌套 callback，并交付可重建的最小验证 APK和可信证据。

这是 [V0](android-fex-v0.md)内的一个有界增量。**完成二周目仍不等于 V0_ACCEPTED**：有限 Step、Vulkan Surface、完整 CPU/原子与部分 VM 验收留在后续。16 KiB 按用户要求延期。规范语义沿用 [V0 API](android-fex-v0-api.md)，本文件限定本轮交付子集；不放宽安全、ABI 或停止语义。

执行入口：[给另一个 AI 的任务书](android-fex-round2-handoff.md)。实际起点与全部剩余事项：[一周目结项盘点](../baselines/2026-09-08-round1-closeout.md)。

## 1. 固定起点与交付边界

| 项 | 要求 |
|---|---|
| 实现基线 | 主仓 `85b57cb25a712823ff721388af0ed1ce641a68fb`；从**包含本 spec 的提交**建立 `codex/android-fex-round2`，不要切回代码基线丢掉 spec |
| FEX | `references/FEX` gitlink `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`；自有 `tencentmalos/FEX`，不回退到旧 reference 的 `f2b679f6…`，不自动升级 |
| Foundation | `1f7008848736b7c6c0779220480344fd5b5fc5e3`；复用 DebugBus 和 reflection/packing，TCP 保持关闭 |
| Android reference | `67dbf4e5…` 仅供 UI/session/input 迁移；ARM64 reference `be6bc2e9…` 供 guest/HLE 逻辑比对，二者都不直接替换主仓 |
| 工具链事实 | 一周目实际 NDK 29.0.14206865、native target API 35，在 SDK 36 设备跑 ELF；二周目如继续 API 35 须如实锁定，compileSdk/targetSdk 36 与 native API 分别记录，不伪装为 API 36 sysroot |
| backend 入口 | `cmake/fex`，`V0_ENABLE_FEX=ON` + `FEX_BUILD_DIR`；app、CLI、instrumentation 使用同一 backend target |
| CPU 范围 | x86-64 整数/SSE2 fixture；DirectMapped + ExplicitPublication；一进程一个活跃 context，两个并发 owner；不支持 owner 迁移 |
| APK | 主仓最小普通 Activity + JNI + native workers；界面显示结果/寄存器摘要并接收测试输入；不要求 Vulkan 画面 |
| 测试素材 | 仓内自有 x86-64 fixture，不带游戏、rootfs、下载 runtime 或 prebuilt core |

既有 ≥16 KiB ELF 对齐 flags 可以保留；这不是重新引入 16 KiB host-page 实机验收。用户不需要为本轮提供 16 KiB 设备、游戏或额外功能偏好。

本轮不接完整 Orbis loader/HLE、PS4 renderer、音频、VR、Winlator/Wine/Vortek、TCP debugger 或任意异步停点的精确 guest 重建。源码低 VA 限制仍视为本 backend 的政策；不把它写成 FEX 的固有地址上限，也不顺手取消。

## 2. 必须保住的一周目行为

1. publication 从关闭准入、修改字节到成功失效的全过程排除新 Run/CreateThread；不能只在 sink 回调瞬间排除。
2. failed publication/invalidation 必须封锁执行及重新赋予 Execute。多个失败范围分别修复，不能由无关成功发布清空；pin 先写后失效失败同样封锁。
3. sink 的异常转换为定义错误、计数释放；Clear 等待 in-flight，排空期间拒绝 replacement；禁止回调内自注销产生自等待。
4. token 检查来源、存活和 epoch，pin 覆盖整个使用期。旧 token、旧 handle、已销毁 context 的 ticket 不得误命中新对象。
5. 普通 Map/Unmap/Protect/RegisterAlias 在活动执行/事务中仍返回 Busy。新需求通过有 token 的事务入口实现，不能删除 guard。
6. backend-free public headers、真实错误类别、snapshot validity、未知能力执行前拒绝、线程所有权都保持。
7. 保留现有 contract/guest、poison 多区间、sink 排空和 runner 回归。测试数量可增长，不以删除难测用例或硬编码 PASS 维持绿色。

## 3. 架构和核心契约

### 3.1 控制线程、owner 和请求

JNI/UI/DebugBus 只读已发布快照并投递请求。每个 guest thread 的 CreateThread/Run/WriteRegisters/InvokeGuest/DestroyThread 仍由绑定 owner 执行。worker command queue 无法打断正在 ExecuteThread 中的 owner，**必须另有异步 kick 路径**。

增加以下 backend-free 能力，命名可调整但语义不能省略：

```cpp
Result<InterruptTicket> RequestInterrupt(ThreadHandle, InterruptReason); // any thread
Result<StopReceipt> WaitStopped(InterruptTicket, Timeout);              // controller
Result<QuiescenceToken> QuiesceContext(Timeout);                        // controller
Result<void> ClearCodeCache(const QuiescenceToken&);
Result<CallResult> InvokeGuest(HleScope&, GuestCodeAddress, GuestCallFrame); // owner
```

- ticket/receipt 绑定 context identity、thread generation、request epoch；`stop_epoch` 与“请求已收到”不是同一件事。ticket 必须能关联覆盖该请求的最终回执。
- owner 只有退出 JIT、完成所需寄存器 spill/快照发布、停止 guest/HLE 写入后才能 ack。controller 不能直接写 FEX CPUState 或伪造 `running=false`。
- 恢复仅消费已确认且显式指定的 epoch；更新请求继续 pending。Fault/BackendFailure > Cancel > Pause，其他原因保留在 pending bits 中。
- 对 Ready/Stopped/InJit/InHle/InCallback/WaitingHle/Faulted/Destroyed 明确请求、等待、恢复行为。超时不得谎报停止或销毁仍活动的资源。
- `RunOptions.deadline_ns` 本轮可实现，或在非零时执行前明确 Unsupported；不得继续静默忽略。`resume_after_epoch` 本轮必须生效。
- 暂停保留可恢复状态；Cancel/Shutdown 完成 invocation 清理后返回，新的 session 可以建立。停止态寄存器写入需 owner + 当前 epoch。

**FEX 接入先做源码证明。** 固定版本的 `SignalDelegatorConfig` 中存在 pause/stop spill 地址，但当前 embedder 没有 Android delivery 实现。实现者先记录所选 kick（例如独占 interrupt page/平台信号配合 dispatcher return）的调用链、host 寄存器 spill、出口 ABI、嵌套 signal 与清理规则，再接 API。不得把普通 `siglongjmp` 跨 JIT/C++ 锁栈当作停止实现；不得在 handler 里分配、加普通 mutex、调用 Foundation 或执行复杂日志。

signal 归属必须基于实际 native thread、generation、PC/故障地址和当次 backend 状态，不能把所有 SIGSEGV 当成 guest fault。记录 previous handler 的安装、转发及恢复规则，支持 app 中 ART 活动。无 HLE、无 yield、自跳转且已缓存的 x86 loop 是 kick 的必要测试，不能只在编译块入口/自然 return 测。

### 3.2 Quiescence、映射与缓存

保留 `GuestAddressSpace::Quiesce` 的停止态快速路径；新增 context 协调层负责 running owners。事务至少分为：

1. 关闭新 Run/Step/CreateThread/InvokeGuest 与 writer 的准入，取得本轮 coordinator 身份。
2. 在短锁中列出当前 owners，锁外投递请求并等待对应 ack；同时等待/拒绝 HLE span writer。一个正常 invocation 的外层 execution lease 不应使其自己永远无法 ack。
3. 只有所有目标 owners 和 writers 已静止才返回唯一有效 token。任何超时都不产生可写 token，不能开始改代码。
4. 在 token 下完成 mapping/bytes、FEX executable-range metadata、所有相关翻译失效和 generation 发布。回调在需要的锁外运行，维持 sink 存活。
5. 成功结束后开放准入并按策略恢复原本 Running 的线程；原本用户暂停的线程仍暂停。存在失败/poison 时保持封锁。

区分 controller quiesce epoch 与用户 pause/cancel epoch。事务失败/超时只能撤销自己的内部请求，不能消费外部更新的请求。实现前写一张锁顺序/状态转换表；不得持 CPU/VM/registry 锁等待 owner 或 sink。

提供有 token 的 Unmap/Remap/Protect（或等价 memory transaction），支持同 VA 替换代码。任何提交点都同步 `FexSyscallHandler` 的 executable-range 查询表；禁止让查询表无限追加或保留卸载区间。全 cache 清空可作为首版保守策略，但必须清理每线程 lookup/direct-link/共享缓存等实际入口，且不改变 guest 寄存器、内存或 invocation 身份。

若写入/重映射后失效失败，进入 fail-closed 并提供显式修复或关闭 session 路径；不能在未证明全部恢复时宣称回滚成功。M08 的 guest store 只能在受控 ExplicitPublication 协议下进行：确保其他执行者不会并发执行被改区域，store 后从 HLE 边界把发布交给 controller，所有 owner 退到可写安全点后再提交，不从 HLE callback 自己阻塞等自己。

### 3.3 真实 typed HLE 与嵌套 callback

用最小 fixture gate 接入当前 `HleCallRegistry`，迁移已有 reference 的思想而非混入旧 FEX 私有结构。

- 注册时定义并验证 signature，包括整数/浮点、buffer 指针和长度参数的关联、In/Out/InOut、nullable、最大长度、guest pointer 返回规则。聚合、varargs、host 指针返回等未支持签名注册即拒绝。
- 先完成有界参数解码和所有 span 的 pin，再调用 native HLE；调用返回/异常/取消后释放。`sizeof(T)` 只能证明单对象，不能替代 buffer 长度。验证长度乘法/地址溢出、零长/null 规则、只读输出、跨未映射边界；错误参数不能调用 host 函数或写坏 guest。
- 区分 SysV call 与 Orbis syscall ABI，明确第 4 整数参数 RCX 与 syscall R10 的适配；不透传 Android syscalls。8 整数、9 double、混合参数必须由真实 guest 指令构造，核对 spill 栈槽、callee-saved、RSP/red zone、返回 GPR/XMM 与 host FP 环境。
- `HleScope` 绑定 owner/context/thread/generation/invocation；InvokeGuest 只在合法 scope 使用。两层 callback 独立保存 guest 状态、FS/GS、栈、返回 gate 和父 invocation；设有界 depth 上限并验证拒绝不改状态。
- 不允许递归普通 Run。非 owner 的 InvokeGuest 默认 WrongThread；不能为了 callback 解除全局执行准入让同一 ThreadHandle 并发执行。
- 取消等待通过 cancel-aware wait 唤醒，在正常 owner 栈清理；异常在 HLE 边界转换，不跨 JIT unwind。callback 收到取消后逐层展开已登记 invocation，不丢 outer state 或泄漏 pin。
- 未注册 syscall/gate **立即通过合法 backend 出口停止**，不能只置共享 bool 后继续执行下一条有副作用的 guest 指令。结果归属 per-thread/per-invocation，两个 owners 的错误不能相互窃取。

### 3.4 Android app、Foundation 与观察

建议目录 `android/fex-validation/` + `src/android/validation/`（可调整并更新入口）。使用普通 Activity、JNI session handle 和两个 native owners；验证脚本直接驱动 app instrumentation。app 可以用标准 Android 控件显示 guest 返回值，输入必须真的送入 guest buffer/HLE 流程再返回，不能只在 UI 上改计数。

- APK 使用同一 `guest_cpu_fex` target；不要复制链接列表、复用 shell ELF 作为运行进程或引入 glibc service。JNI DLL 与 FEX 静态库的 PIC/STL/异常/RTTI/allocator 选择必须在锁文件中统一。
- Foundation DebugBus 暴露 status/capabilities/pause/stop/结果查询，控制操作只投递请求。reflection/packing 用一个独立诊断 payload 的编码解码证明真实 NDK 依赖闭包，不用来代替 Orbis ABI adapter。TCP capability=false；不为诊断临时另建网络/反射框架。
- session 先关闭准入和诊断注册，等待 in-flight 与 owner 停止，再销毁 thread/context/address space，最后释放 JNI global refs；禁止 JNI/main thread join 一个仍未取消的 loop。Activity detach/back 不得留下写旧 session 的 callback。
- app 内 `sysconf/getpagesize` 必须实测 4096；记录 SDK、ABI、实际加载 native Build IDs、APK hash 与 fixture hash。`adb shell getconf` 不能替代进程值。
- 增加在锁外、已发布 immutable SafePoint snapshot 后调用的 native observation hook。LLDB 只读该 hook 的快照、native/guest 映射、RIP bytes；guest 解码使用记录版本的 x86-64 工具。异步 fault 只填可靠字段，保留 host PC/si_code，Unknown 不标成精确 guest RIP。
- 本轮 StepScope=None，执行前 Unsupported。UI 不显示可用 Step；之后在独立里程碑补 T06/T07，不用一个 JIT block 冒充一步。

## 4. 顺序和交付门槛

| Gate | 主要改动位置 | 完成后可观察到什么 | 依赖 |
|---|---|---|---|
| G0 证据与构建基线 | runner / artifact verifier / lock / docs | clean build 可定位，历史辅助成绩不再冒充 APK 覆盖；已有回归可复现 | 无 |
| G1 运行控制 | api/context、execution、fex backend / Android signal adapter | 两 owner 真并发，无 HLE loop 可 pause/resume/cancel；epoch 和快照可靠 | G0 |
| G2 运行中事务 | address_space / context coordinator / FEX range metadata | 100 epoch 双 owner patch、token remap、poison 失败恢复 | G1 |
| G3 HLE | typed adapter / gate / invocation stack / fixtures | guest 真正传参、长度 pin、TLS 与两层 callback；异常/等待可取消 | G1；发布路径用 G2 |
| G4 普通 app | Android app/JNI、Foundation 接线、证据工具 | 同 backend 在 ART 进程内运行、停止、观察和重复清理 | G1–G3 |

可在早期建立 app 编译壳以发现 NDK 依赖问题；在 G1–G3 的行为门槛通过前，不把壳/启动页算作 G4 完成。每个 gate 单独形成可复核提交；失败先保留最小复现，再改实现和回归。

验收项可跨 gate 分步收敛：G0 先完成 native clean baseline 与正确的 runner 判定，不等待尚未实现的 APK；R2-B01 的 APK 闭包在 G4 完成。G2 的 WaitingHle/InvokeGuest 竞态在 G3 接通后补测，所有 app 环境重跑在 G4 完成。中间完成子项保留辅助证据，不删减最终 24 项门槛。

## 5. 二周目验收清单

以下 **24 项全部 MUST**。测试初始均为 NOT_RUN；“覆盖 V0”表示关联，只有满足原项全部语义、环境和次数才可升级 V0 原项。G1–G3 可先跑 adb-shell ELF 快速迭代，但 CPU/HLE/VM 项最终必须在同一个普通 APK 身份下再跑。B 类按表执行；D01 用匹配此 APK 的符号。

正常 fixture 至少连续 10 次；特殊次数见表。普通单例 timeout 10 秒，stop 请求到安全点每次 ≤1 秒（热身后，不在 LLDB 人为暂停时计时），soak supervisor 12 分钟。超时计 TIMEOUT/失败，supervisor 强制清理不能当作 Stop 成功。

| R2 ID | 输入与必须断言 | 次数 / 证据 | V0 关联 |
|---|---|---|---|
| R2-B01 | 独立 checkout，按锁构建 FEX/backend/普通 APK；检查全包 ELF machine/依赖/版本、ZIP、dlopen；保留 ≥16 KiB ELF 对齐；B05 编译；受影响桌面目标构建 | 一次完整 clean 构建和一轮增量；缺工具链的具体项 NOT_RUN | B01–B03/B05/B06 |
| R2-B02 | runner 负例：无 APK 只有 ELF、host/CLI 结果混入 app、缺子项、SKIP、无输出退出/timeout、重复 case ID、旧 binary hash、缺 required suite | 自动回归；FAIL/TIMEOUT 不被后次 PASS 覆盖；重复记录必须有 iteration 身份 | 证据可信度 |
| R2-C01 | 两 owners 同时在 JIT 中各自更新不同进度，native TID 不同；单个 owner stop 时另一个继续 | 用 barrier/独立观测证明重叠，不能串行 Run | T01 |
| R2-C02 | 已热身的无 HLE/yield 自跳 loop，外部 Pause→WaitStopped→Resume，最终 Cancel | 100 次，记录每次 request/ack/epoch、p50/p95/max；max≤1秒 | T02 |
| R2-C03 | Pause/Cancel 与 Resume 协调交错；timeout 后迟到 ack；旧 ticket/handle/context 拒绝 | 每类交错至少 100 次，新请求不丢；fault 优先级另有故障注入 | T03 |
| R2-C04 | 停止态写 GPR/RIP/XMM/MXCSR/FS/GS 后真实执行使用新值；运行/stale epoch setter 拒绝；非零 deadline 支持或执行前拒绝 | 字段有效位与独立 sentinel；保留 host FP 环境 | T05 |
| R2-M01 | 两 owners 运行同一 A block，controller 停止全部、patch B、恢复后两者只输出 B | 100 epoch，保留每线程 ack 和实际结果 | M09/M07 |
| R2-M02 | quiesce 期间新 Run/CreateThread/InvokeGuest/writer 被拒；活动 pin/WaitingHle/迟到 kick/注销 sink 的 timeout 与清理 | 可控 barrier 故障注入；无提前写入、外部 pause/cancel 不被撤销 | M13/T03 |
| R2-M03 | token 下同 VA unmap/remap A/B，复用现有 thread；ClearCodeCache 后 GPR/FP/内存不变，重新执行正确 | 100 次；验证被移除 range 不再由 FEX 查询返回 | M11/M14 |
| R2-M04 | guest 实际 store 改另一段已执行代码，经 HLE/协调者 ExplicitPublication 后再执行新版本 | 至少 10 次；拒绝请求 TransparentSMC；保存 guest 字节和结果 | M08 |
| R2-M05 | 一周目 M13h–o/G10 等多 poison、sink 异常/排空、失败后 Run 与 Protect 拒绝、成功修复恢复全部保持 | 既有 suite 全回归；新 Run/Resume/InvokeGuest 也不能绕过 poison | M07/M13 |
| R2-H01 | 真实 guest 的 8 整数、9 double、混合参数和返回，验证 RCX/R10 gate 区别、栈、callee-saved/red zone | 各 10 次；native HLE 调用日志绑定 invocation | H01 |
| R2-H02 | 真实 buffer 长度/方向 pin，另一线程 remap/protect 竞争；溢出/null/零长/只读/过期 buffer 与错误签名负例 | 每类 10 次；失败时 host 函数未调用、guest sentinel 不变 | H04/M13 |
| R2-H03 | 两 owner 各自不同 FS/GS/TLS；guest→HLE→guest→HLE→guest 两层 callback | 每 owner 1000 次 HLE，至少 100 次两层嵌套，outer/inner 状态正确 | H02/H03 |
| R2-H04 | native HLE 异常、WaitingHle Stop、内层 callback Cancel，随后重新建立 session | 100 次混合循环，等待 Stop≤1秒，无跨 JIT unwind/残留 pin | H05/T04 |
| R2-H05 | 未注册 syscall/gate 后紧跟 sentinel store；另一 owner 正常 HLE 并发 | 错误 owner 立即停，后继 store 不执行，另一 owner 无错误串值 | H04/D05 子集 |
| R2-H06 | 非 owner InvokeGuest、普通 Run 重入、过期 HleScope、超 depth callback | 明确拒绝且状态/栈/计数不变 | H06 |
| R2-A01 | 普通 Activity/JNI 内初始化，UI/instrumentation 输入→guest→真实 HLE→文本输出；不需外部 runtime | app 内 page=4096、backend/FEX/API/加载库身份实测 | A01/P04/P05 |
| R2-A02 | 前后台/back/Activity recreate、运行 loop 时 Stop、100 次 session create/run/stop/destroy | 前后台/recreate 各20次；owned threads/mapping/FD/JNI refs 回到热身基线，无旧 callback | L01/L03；不是 Surface A03/A04 |
| R2-A03 | ART/Java 分配回调与 JIT 并行；实际 guest guard fault；无关 native signal previous/default 行为 | 预期 crash 由 app 身份隔离测试进程执行，记录实际 signal/exit；不吞信号 | D03/D04/M05 子集 |
| R2-F01 | Foundation status/capabilities/pause/stop 与 guest 并发，诊断请求中关闭/重启 | 100 次，registry 排空；返回稳定快照或定义错误 | F01/F03 |
| R2-F02 | 实际 NDK reflection/packing payload round-trip、截断/错误 schema；TCP capability=false | 实际依赖/Build IDs 和 app 输出，无 stub target | F02 |
| R2-D01 | LLDB 停 snapshot hook，读取 native/guest/thread/epoch/寄存器和 RIP bytes，离线 x86 解码 | 与同 APK fixture sentinel 一致；只读，不 evaluate inferior helper | D01/D02 |
| R2-L01 | app 内 CPU/HLE/输入循环 10 分钟，结束正常 stop；中途故障保留已完成记录 | owned resources 基线和峰值、RSS趋势、线程/FD，不要求 RSS精确回零 | L02 的 CPU 子范围/L03 |

R2-C01 与 R2-H03 是真正 guest 并发，但不替代 M15/M16 的 LOCK/TSO 验收。R2-A02 不使用 ANativeWindow，不能标 A03/A04；R2-L01 无绘制，不能标完整 L02。CPU/HLE 在 CLI 已通过、app 未运行时，二周目相应最终项仍 NOT_RUN，另存辅助结果。

## 6. 工具、证据和完成状态

复用并扩展 `scripts/android/check-v0-environment`、`build-fexcore-android`、`verify-v0-artifacts`、`run-v0-tests`；新增 APK build/instrumentation/evidence 入口时更新 `scripts/android/README.md`。不要把原始日志只留在 /tmp，不得预填 PASS。原 V0 runner 可保留 CLI 模式，但须报告环境和 coverage，不以文件存在推断完成。

每次设备运行记录：

- implementation/spec/test-runner SHA、dirty diff hash（如有）、全部 gitlinks、FEX upstream/downstream 锁和 Foundation 实际模块；不以 branch name 代替 SHA。
- NDK package/Clang/API、SDK/Gradle/JDK/CMake/Ninja 版本与依赖锁；设备 SDK/ABI/kernel/app page、app 身份和 instrumentation 版本。
- APK SHA、安装/加载一致性证明、每个 native library Build ID 与符号、fixture source/hash；不能只记本地待部署文件。
- 每个 case/iteration 的 expected/actual、environment、coverage、applicability、request/ack/stop epoch、耗时、失败/timeout、原始日志/hash。
- 分离 `round2-results.json`（24 项）与完整 `v0-results.json`（60 项并显式延期/条件适用性）。V0 成绩不由 R2 关联列直接换算。

归档到 `docs/validation/round2/<run-id>/`，另交 `implementation-report.md`、可复现命令、状态/锁序技术决定及后续欠项。二进制/APK/完整符号用构建产物位置和 hash 引用，不提交大文件或游戏。

只有 G0–G4 和全部 24 项满足规定环境/次数、源码与部署产物一致，才报告 **ROUND2_ACCEPTED**。缺实现/有失败使用 ROUND2_IN_PROGRESS；全部实现且唯一缺口确为无法获取设备时可报 ROUND2_IMPLEMENTED_DEVICE_PENDING，并逐项给出未运行原因。即使二周目通过，完整 V0 仍为 **V0_IN_PROGRESS**，直至其后续欠项另外满足。

## 7. 后续独立里程碑

1. **调试和 CPU 语义**：有限 Step T06/T07、C01/C02 完整 flags/FP、C03/C04 CPUID/拒绝、D05 全 trap、M15/M16 原子/ordering；先保证支持范围内正确和范围外拒绝。
2. **native Vulkan 显示**：A02–A04 的 Surface/swapchain/input/HLE、M12 GPU/code observer、完整 L02 soak；沿用本轮 session/owner 控制，避免把 Stop 绑定 Surface 存活。
3. **loader/映射和游戏**：M06 segment/BSS、M10 真 alias、完整 M05，受控迁入 Orbis loader/HLE/ARM64 host 服务和依赖；先非 VR 游戏的执行/画面/输入/音频，再 PSVR/Move/Beat Saber。
4. **16 KiB**：仅在用户恢复此目标后开展 B04/M02/M03 与其余 host-page 适配/设备回归。
