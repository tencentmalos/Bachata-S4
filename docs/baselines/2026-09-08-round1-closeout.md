# 一周目提交状态与剩余事项（2026-09-08）

一周目的实现与复核材料已提交并推送到 `tencentmalos/Bachata-S4` 的 `codex/android-fex-v0`：
**`85b57cb25a712823ff721388af0ed1ce641a68fb`**。本记录只盘点该提交，不表示二周目已经实施。
最初的 [a7128893 基线](2026-09-07-android-fex-foundation.md)保留不变。

下一轮直接使用[二周目 spec](../specs/android-fex-round2.md)和[执行任务书](../specs/android-fex-round2-handoff.md)。

## 1. 提交和证据对应关系

- 本次 push 包含此前未推送的 `95bc13fa`、`e3746e36` 等主仓提交，以及本次 `85b57cb2`；远端此前停在 `e23bf61d`。
- FEX 固定为 `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`，远端分支 `feature/malos/host-page-size` 已含该提交。
- Foundation 固定为 `1f7008848736b7c6c0779220480344fd5b5fc5e3`，远端分支 `codex/shadps4-android-fex-v0` 已含该提交。
- 本轮没有改动或推进子仓 gitlink。两份 Android/ARM64 reference 继续固定在 `67dbf4e5…` / `be6bc2e9…`。
- push 后通过 `gh api repos/<owner>/<repo>/git/ref/heads/<branch>` 复核主仓、FEX、Foundation、Android frontend 和 ARM64 五条远端 ref，均与上述版本一致。二周目的新开发分支尚未创建，由执行者按任务书从固定起点建立。
- [事务加固报告](../validation/v0/transaction-hardening-2026-09-08.md)及其 JSON 记录的是当时 `e3746e36 + dirty patch` 的测试。现在对应源码进入 `85b57cb2`；提交前逐一核对了 8 个源码 hash 和 6 个 suite 日志 hash。**不倒改历史 JSON 为 clean commit 实测**。
- 归档 patch 中的空白上下文行保留原始 diff 格式。生产源码和其他新增文件通过 `git diff --check`。
- 当前工作区另有 Windows/Vortek 研究文档、相关索引修改、`references/Bachata-S4` 的两个 Android 文件修改及 `externals/dear_imgui/`。它们不是本次事务修复的一部分，均保留且未纳入本次提交；fresh checkout 不能依赖它们。

已验证：Swan Android 16 / SDK 36 / ARM64 / 4096-byte host page，contract **34/34**、真实 guest **45/45**、bionic 初始化 **12/12**；host contract **33 PASS + 1 SKIP**、手工 ABI frame **14/14**、page probe **22/22**。本次提交前另复跑 Python runner **6/6**。这些是独立 native harness，尚无普通 app / ART / Vulkan / 游戏验收。

## 2. 验收数字还需要怎样理解

[归档 results.json](../validation/v0/transaction-hardening-2026-09-08/results.json)的实际输出是 **11 PASS / 0 FAIL / 46 NOT_RUN / 3 DEFERRED_BY_SCOPE**，共 60 项。57 项处于当前 V0 范围。46 项 NOT_RUN 中有 10 项明确记录 `auxiliary_status=PASS`，并非 46 项都没有代码。

runner 标 PASS 的 ID 为 `B02 B05 M01 M04 M07 M13 P01 P02 P03 P04 P05`。这仍然不是普通 app 的完整验收成绩：

1. **本次盘点新确认：B02 是独立 ELF 的辅助证据。** [CheckNativeElf](../../scripts/android/run-v0-tests)只遍历若干 probe，没有 APK；计算了 LOAD alignment 但没有据此拒绝，`readelf -hld` 也不是版本需求表检查。应统一使用产物校验器，完整检查 APK 中所有 native 库、依赖和符号版本，缺 APK 时保留辅助证据而不是给完整 B02 PASS。
2. M01/M04/M07/M13 目前分别来自 host/adb-shell harness，尚未在 app 身份下覆盖规范要求。P01–P03 是独立页大小/布局探针，P04–P05 是初始化；它们各自的成立范围不能扩大成 ART/FEX 执行安全。
3. F04/C04 是条件验收。网络未启用、AVX 未声明不能记 PASS；下一轮要用机器可读 applicability 和拒绝测试区分条件不适用与未实现，不能为了提高比例改写旧结果或静默改变分母。

二周目先修正结果的 **case / environment / artifact / coverage** 区分，重新生成结果；本盘点没有重跑或改写上述历史成绩。

## 3. 全量剩余事项

以下覆盖 V0 的全部 60 个 ID。阶段“R2”表示二周目必做的相应子范围，不保证原 V0 同名验收整项随之完成；“后续”仍是 V0 欠项，详见二周目逐项映射。

| V0 ID | 当前实际基础 / 缺口 | 安排 |
|---|---|---|
| B01 | 有 NDK 脚本，尚未从无作者 build/cache 的独立 checkout 构建 APK | R2-G0/G4 |
| B02 | 两个 Android probe ELF 辅助检查；缺 APK 全库、符号版本、依赖闭包判定 | R2-G0/G4 |
| B03 | 有产物检查脚本，无 APK ZIP/JNI 证据 | R2-G4 |
| B04、M02、M03 | 16 KiB 双设备/子页专属用例 | 用户明确延期，非当前 blocker |
| B05 | public API 消费者编译通过；后续新增 API 需保持无 FEX/JNI include 泄漏 | R2 持续回归 |
| B06 | 可选配置检查存在，未完成受影响桌面目标构建 | R2-G0 构建隔离回归 |
| F01、F03 | Foundation 最小 target 存在；缺 session registry 控制、执行并发与注销排空 | R2-G4 |
| F02 | reflection/packing NDK 闭包未验证；历史 fmt 缺口需按实际依赖复核 | R2-G4，修依赖接线，不造同名 stub |
| F04 | 未启用 TCP | 后续；明确 capability=false |
| C01 | 真实整数/分支已跑，缺有效 RFLAGS 等完整 oracle | 后续；R2 保留回归 |
| C02 | 真实 SSE2 基础已跑，缺完整 load/store、MXCSR 舍入与 host FP 恢复证据 | 后续完整补齐；R2 HLE/状态写入不得破坏 FP 环境 |
| C03、C04 | Config 拒绝不支持请求已有基础；未验证实际 CPUID/禁用指令与 profile 一致 | 后续；AVX 继续不声明 |
| H01 | typed adapter 只在手工 frame 测试；缺真正 gate、SysV 栈/寄存器返回 | R2-G3 |
| H02 | 单个 fixture 的 TLS 证据不能替代两个 owner 各 1000 次 HLE 往返 | R2-G3 |
| H03 | 未接 InvokeGuest/HleScope/两层 invocation 栈 | R2-G3 |
| H04 | 指针仅检查 sizeof(T)，没有长度描述或调用期 pin；真实未知 syscall 拒绝未闭合 | R2-G3 |
| H05、H06 | 没有实际 HLE 的异常、可取消等待、callback 取消和非 owner 拒绝闭环 | R2-G3 |
| M01、M04 | host/设备 contract 覆盖映射与拒绝路径；尚缺 app 环境证据 | R2-G4 重跑 |
| M05 | API 权限拒绝已测；真实 guest guard/跨页 fault 未测 | R2-G4 信号/故障子范围；完整跨页矩阵后续 |
| M06 | segment/BSS helper 未实现 | 后续 loader 准备 |
| M07 | 已有实际 FEX A→B 发布及失败恢复，停止态 100 次改写 | R2 回归并迁到 app |
| M08 | 缺真正 guest store→HLE 请求→quiesce/publication | R2-G2/G3 |
| M09 | Quiesce 仍对正在执行的线程返回 Busy，尚不能停止两个 running owners | R2-G1/G2 |
| M10 | alias 元数据基础；没有同 backing 双 VA/双 JIT 译码实测 | 后续，R2 不宣称 alias 完成 |
| M11 | 普通 mapping 在执行/事务中均 Busy；缺 token remap 与 FEX executable-range 更新 | R2-G2 |
| M12 | observer 算术测试；缺 production tracker 的 code/GPU 联合通知 | 后续 GPU 跟踪 |
| M13 | pin/事务/poison/sink 排空已有 34 个 contract 子项中的相应回归；缺真实 HLE span 接入 | R2-G2/G3/G4 |
| M14 | 有真实失效重译，但 public ClearCodeCache、状态保留与完整 JIT 权限审计未闭合 | R2-G2 |
| M15、M16 | 未验证真正双 guest 的 LOCK 边界压力和 ordering litmus | 后续；不能从 T01 推导原子/TSO 正确 |
| P01–P03 | 独立布局/页大小验证已有证据；不是 16 KiB Android 执行验收 | R2 回归，16 KiB 设备适配后续 |
| P04、P05 | bionic 初始化/真实线程对象生命周期 12/12；没有 ART 共存证据 | R2-G4 重跑 |
| T01 | 支持建立多个 thread entry；现有顺序 Run 不等于真正并发 | R2-G1/G3 |
| T02 | public CpuContext 尚无 RequestInterrupt/WaitStopped；无 HLE loop 不能正常停 | R2-G1 |
| T03 | ticket/receipt 数据类型与 reason 优先级已有，RunOptions 还未消费；无实际 epoch 竞态 | R2-G1 |
| T04 | 未实现 WaitingHle 可取消等待 | R2-G3 |
| T05 | 停止态 Read/WriteRegisters 已有；完整字段写后执行、epoch 和 running 拒绝待测 | R2-G1/G4 |
| T06、T07 | Step 明确 Unsupported，scope=None；缺有限指令单步及 HLEBoundary 分类 | 后续独立里程碑；R2 保持执行前拒绝 |
| D01、D02 | 有 snapshot 数据类型，缺匹配 APK 符号的 LLDB 安全点实测及 guest bytes 解码 | R2-G4 |
| D03 | 有模拟 fault 元数据测试；缺实际 host context 与归属/质量证据 | R2-G4 有界故障子范围；不要求任意异步停点精确重建 |
| D04 | 当前 delegator 提供配置，没有 app signal 路由/ART 共存验证 | R2-G1/G4 |
| D05 | 普通 HLT 已与 return gate 区分；非法指令/未注册 trap 未覆盖完整集合 | R2 未知 gate 拒绝；完整非法指令集后续 |
| A01 | 无主仓 JNI/普通 Activity 验证 app；reference glibc 启动链不是目标实现 | R2-G4 |
| A02–A04 | 无原生 Vulkan Surface、输入→guest→画面和 swapchain 生命周期 | 后续显示里程碑；R2 仅文本 UI 和 CPU/session 生命周期 |
| L01、L03 | 有零散创建/销毁与 runner 失败处理，缺 app 全资源循环及故障清理 | R2-G4 |
| L02 | 无 10 分钟 fixture/绘制/输入联测 | R2 先做 CPU/HLE/app 10 分钟 soak；绘制部分后续 |

## 4. 下一轮实现必须正视的代码事实

- [CpuContext](../../src/core/guest_cpu/api/context.h)没有控制请求接口；[execution.h](../../src/core/guest_cpu/api/execution.h)中的 ticket/receipt **只是类型**。[Run](../../src/core/guest_cpu/fex/fex_context.cpp)调用 ExecuteThread 前后没有处理 `resume_after_epoch` / `deadline_ns`，不能称为已实现异步控制。
- `FexSignalDelegator` 当前仅提供 callback RET 地址。FEX 提供 dispatcher pause/stop 地址并不等于 Android signal handler 已实现安全退出。先在 pinned 源码上证明 spill/return 路径，再做控制协议。
- `FexSyscallHandler::unexpected_syscall` 当前是 context 共享布尔值，HandleSyscall 只置位。真正双 owner 时可能串 fault 归属，且未知 syscall 后未立即阻止后续 guest 副作用；必须改为 per-thread/invocation 的退出记录。
- `RegisterExecutableRange` 目前只追加，主要由 CreateThread 调用。事务 remap 必须同步 FEX 查询元数据和译码，不能只改 mmap 或让新 thread 的注册掩盖旧数据。
- [HLE DecodePointer](../../src/core/guest_cpu/hle/call_adapter.h)的 ValidateRange 后裸指针无法表达长度与调用期生命期；新调用 gate 必须在接入前补这层。
- [cmake/fex](../../cmake/fex/CMakeLists.txt)是既有 backend 构建入口；主仓 app 应链接同一 target。不要又引入一套复制的 FEX 源文件/链接列表。

二周目聚焦“可控 CPU + 真实 HLE + 普通 app 进程中的可观测闭环”。有限 Step、完整 CPU/原子兼容、Vulkan、完整 shadPS4 loader/HLE/renderer、非 VR 游戏、音频和 PSVR/Beat Saber 依次保留在后续工作中。
