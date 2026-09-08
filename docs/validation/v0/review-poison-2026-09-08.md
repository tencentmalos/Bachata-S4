# 发布失败保护与 sink 生命周期复核（2026-09-08）

**结论：正常的公共发布链路已接通，但“失效失败后不可恢复”尚未成立；sink 注销也缺少在途回调保护。当前是 `V0_IN_PROGRESS`，应先完成停止状态下的发布准入、失败封锁和生命周期协议，再推进运行线程中断、HLE 与 app。**

审核主仓 `95bc13fa6dc65961b46929d270daad876262d464`，FEX `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`。本轮对应[发布后续记录](followup-publication-2026-09-08.md)与[上轮审核](review-publication-2026-09-08.md)。验证范围保持 Swan / Android 16 / ARM64 / **4 KiB**。

本次未修改生产代码或子仓；只增加审核材料、最小探针与索引。相关探针在临时目录编译，设备上传文件用独立名称并在有界运行后清理。未提交或 push。

## 1. 已确认的进展

- 重新构建、链接后，Swan 原有真实 guest harness **41/41，exit 0**。G09 公共发布路径可取得新代码结果，因此上轮“公共 API 完全不触达 FEX 缓存”的缺陷确已修复。
- Swan API contract **26/26，exit 0**，包括实际执行 M13f execute 撤销和 M13g 两线程持 pin/Unmap/Protect 竞争。
- Host HLE **14/14**、page probe **22/22**；host contract 打印 **26/26**，但其中 M13f 实际跳过，不能写成 26 个用例均已执行全部断言。
- 只读 `Bytes()` 已返回 const span；可写视图单独申请。内存 token 消费入口统一校验来源及 epoch，方向正确。
- `CodeInvalidationSink` 返回失败状态，与 best-effort MemoryObserver 分开是正确选择；在 space 锁外调用避免既有锁序倒置。但锁外回调还需要生命周期和发布准入协议。
- runner 现在能处理“打印 PASS 后退出非零”和部分 FAIL+缺项，scope 及元数据也有所改善；下述无输出失败与 skip 仍是缺口。

[完整证据清单](review-poison-2026-09-08/evidence.json)记录实际 SHA、构建口径和产物 hash。FEX 为增量构建，harness 重新链接；本次不是 APK/ART 验收，也未验证 TSan。

## 2. 尚未闭合的问题

### R1 / P1：poison 只是标记，失败后仍可恢复并执行旧 JIT 代码

位置：[address_space.cpp](../../../src/core/guest_cpu/api/address_space.cpp) 约 780–800 行、Protect；[fex_context.cpp](../../../src/core/guest_cpu/fex/fex_context.cpp) 的 Run（约 511 行）。

`PublishCode` 失败时调用 RevokeExecuteLocked 并置 `code_poisoned`，但 Protect 不检查 poison，Run/CreateThread 也不将 poison 作为准入条件。撤销 guest 原始页面的 Execute 不会清除另一块可执行内存里的 ARM64 JIT 翻译。

本轮在 Swan 用失败 sink 做确定性注入：先真实执行 A=17，临时以失败 sink 代替 FEX sink，使发布 B=34 失败；随后恢复原 sink，但不调用成功失效。结果：

| 观察 | 实测 |
|---|---|
| PublishCode | 失败 |
| HasPoisonedCode | true |
| generation | 1 → 2 |
| 之后 Protect(RX) | **成功**，poison 仍为 true |
| 重新创建线程并 Run | **成功，仍执行 A=17** |

见[探针源码](review-poison-2026-09-08/poison_guest_probe.cpp)、[设备日志](review-poison-2026-09-08/poison-swan.txt)。这是故障注入证据，不是声称真实 FEX 在本次正常 fixture 中自行发生失效错误。探针正常结束为 exit 0；其输出证明安全属性失败，不能当作验收 PASS。

M13f 只检查 guest 页权限、标志及计数，没有尝试恢复执行，因此不能证明它名称所称的“nothing executable”。另外，内存 InvalidateCode 的 sink 失败分支直接返回，没有 PublishCode 的 poison 处理；任意其他范围成功发布又会清掉空间级 poison。这些路径也需要统一处理。

建议：将失败状态接入 context 的 Run/CreateThread/恢复入口及可执行权限变更，保持封锁直到相应失效与恢复完成；最小实现可以保持整个 context 不可恢复，不能因无关范围发布成功而清掉。测试须真正尝试 Run/恢复，验证拒绝且 guest 状态未变，并验证成功修复后才允许执行 B。

### R2 / P1：sink 取裸指针后解锁，Clear 不等待旧回调完成

位置：[address_space.cpp](../../../src/core/guest_cpu/api/address_space.cpp) 684–688、731–740、775–783 行；[FexCpuContext 析构](../../../src/core/guest_cpu/fex/fex_context.cpp) 约 303–306 行。

发布方在 space 锁内复制 `code_sink`，解锁后调用虚函数。Clear 仅清空槽位就返回，不阻止已取到旧指针的调用，也不等待正在执行的调用。backend 析构时“先 unregister 就不会再被调用”的注释因此不成立。

本轮 host 两线程探针把 sink 停在回调中，另一线程 Clear 后立即注册 replacement，得到：

```
clear_returned_with_callback_active=1
replacement_registered=1
old_sink_publication_succeeded_after_replacement=1
```

见[源码](review-poison-2026-09-08/sink_lifetime_probe.cpp)、[输出](review-poison-2026-09-08/sink-lifetime-host.txt)。探针刻意保持对象存活，没有人为触发 UAF；源码时序说明若 Clear 后按析构逻辑销毁 backend，已获取的指针/回调没有存活保证。旧回调完成后还可在新 sink 已注册的情况下报告成功，没有注册世代校验。

建议：引入 registration generation 与在途 lease，关闭注册后禁止新借用，并等待已有回调排空后才允许 backend 资源销毁；或使用明确持有生命周期的对象加等价停止协议。不能只把回调移回 space 锁里，否则重新引入 context→space 的锁序问题；等待也不能占着回调完成所需的锁。

### R3 / P1：运行准入未覆盖发布事务，不能把“只差停止正在运行的线程”当作剩余范围

这不是要求立刻实现复杂中断。即使开始时所有线程都已停止，当前 token 也没有阻止新 Run/CreateThread 进入；PublishCode 先 memcpy，然后 FEX sink 才检查 running。新调用可能在检查前后进入 JIT，或者代码先被修改，再因已运行线程而失效失败。

所以仍缺一个**全部线程已停止时也必须存在的事务准入门闩**。单个 DiscardTranslations 的 context 锁不能覆盖“开始修改字节 → 丢弃翻译 → 完成发布”整段。上轮对这个区别的结论仍有效。

建议先做限定的 TryQuiesce：原子关闭新执行/线程准入，确认所有线程已停止，等待/拒绝 writer，在整个事务结束后才开放；遇到运行线程返回 Busy，且不修改字节、权限、generation。随后再扩展到中断并等待 owner 停止回执。前者不能以“中断还未开始”为由继续缺失。

### R4 / P1：runner 的无输出失败仍退回 NOT_RUN

位置：[run-v0-tests](../../../scripts/android/run-v0-tests) 570–574 行、suite 与验收项映射。

失败传播只遍历 `run.cases`。若 suite 在第一条结果前退出或 timeout，该集合为空，验收项不会被 taint。

本轮使用无输出直接 exit 7 的合成 contract suite：suite metadata 正确写 `exit_code=7, usable=false`，但 M01 仍为 NOT_RUN，summary **failed=0**。见[合成 suite](review-poison-2026-09-08/synthetic-empty-exit7.py)、[JSON](review-poison-2026-09-08/empty-crash-results.json)。这是 runner 故障测试，不是一次真实 V0 成绩。

应按 suite 的预期子项/验收覆盖传播进程失败，不依赖已经打印哪些行；同时区分未找到二进制、尚未尝试设备执行，与确已启动后崩溃/超时。无输出崩溃、部分输出后超时、缺少可执行文件都需独立验证。

### R5 / P2：skip 只是附加文字，仍被统计成 PASS

M13f 在 macOS 无法创建 RWX 时打印 skipped 后正常 return，RunCase 随后照常输出 `[M13f] ... PASS`，总数仍是 26/26。本轮再次复现，见 [host-contract.txt](review-poison-2026-09-08/host-contract.txt)。runner 正则只认 PASS/FAIL，M13f 还已加入 M13 的必需映射。

因此“skip 可见”改善了人工阅读，却没有修正机器结果。Swan 确实执行了这条用例，但当前 runner 的 contract 输入来自 host；它没有自动消费本次另行运行的 Swan contract 结果来补该缺项。

应提供结构化 SKIP/NOT_RUN 子项及原因，只有来自明确设备 suite 的真实通过证据才能补足。scope 延后也应由需求决定，不能只在观察到 PASS 后转 DEFERRED；本轮空失败 runner 的 in-scope 又变成 60、deferred=0，说明分母仍随是否运行而改变。

## 3. 构建进展的更正

“之前 host CMakeLists 只存在于临时目录且已丢失”不符合仓库事实。原审核明确使用 `cmake -S cmake/fex ... -DV0_ENABLE_FEX=OFF`；该入口早已提交，当前也仍存在。本轮从新目录实际配置、构建成功，见 [existing-entry.txt](review-poison-2026-09-08/existing-entry.txt)。临时的是 build 目录，不是源 CMakeLists。

新增 tests/guest_cpu/CMakeLists.txt 可用，但属于第二套独立入口，不能据此认定上轮第 3 步的 guest_cpu_fex target 已完成。当前 cmake/fex 的 V0_ENABLE_FEX=ON 仍 FATAL_ERROR；NDK backend 仍通过 shell 手工链接。建议复用/收敛既有入口，避免两份目标列表继续漂移，补真正的 guest_cpu_fex target、实际工具链锁和统一构建产物标识。

## 4. 下一轮建议范围

1. **优先修 R1–R3**：先形成全部线程已停止时的完整事务；失败后封锁执行，注册/注销等待回调排空。把本次“失败后恢复旧 A”和“注销时旧回调仍活跃”作为必须转为拒绝/安全排空的回归。
2. **修 R4–R5 与构建口径**：无输出失败不能是普通 NOT_RUN，skip 不计 PASS；host/设备证据分别记录；复用已有 CMake 入口，完成实际 backend target。
3. **之后扩展到运行线程**：无 HLE guest 循环的中断、stop epoch 回执、超时撤销，完成 T02/M09。等待不能持有 CPU/VM 锁，失败前后都要验证内存未被部分修改。
4. **再接真实 HLE、callback 与 app**：长度/lease、8 整数/9 浮点/混合参数、两层 callback，之后 JNI/Surface 生命周期。有限 Step、LLDB 安全点和 ART 信号共存仍是 V0 最终门槛。

当前可以认可“正常、顺序的公共代码发布已修好”和“host pin 与映射操作竞争已有实测”；不能认可完整安全发布事务已关闭，也不宜仅依据 21 PASS / 0 FAIL 推进到 app 集成验收。
