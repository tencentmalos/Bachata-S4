# 发布事务增量审核与下一轮任务（2026-09-08）

**结论：这轮修复有实际进展，但发布事务仍不能对外宣称可用，23 PASS 也不能当作完整验收通过数。建议下一轮收窄为“发布事务闭环 + 可信 runner”，再接真实 HLE 和 Android app。状态保持 `V0_IN_PROGRESS`。**

审核主仓 `e23bf61dc6b83ea92f31c5ab8fef7da84ff0366e`，FEX `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`。主仓 `origin/codex/android-fex-v0` 与 FEX `origin/feature/malos/host-page-size` 经 `git ls-remote` 核实均包含当前相应 HEAD；FEX 工作区干净。主仓 origin 仍为旧 `tencentmalos/shadPS4` URL，本次查询成功；整理 remote 是低优先级维护，不是执行阻断。

本次只增加审核记录及有界复现证据，没有修改 production code 或子仓。旧[审核](review-2026-09-08.md)、[缓存修复记录](followup-2026-09-08.md)保持其历史含义。

## 1. 已独立确认的进展

- 原有设备 harness 在 Swan / Android 16 / 4 KiB **37/37 通过，exit 0**。新增 G06j 确实验证 FEX adapter 拒绝来自其他地址空间的 token。
- 独立新 host 构建：API/VM **22/22**、typed ABI **14/14**。
- `Write()` 在活动事务中拒绝；`PublishCode` 的校验、memcpy、generation 更新现在处于同一内存锁下，关闭了原来的检查后使用间隙。
- FEX `InvalidateCode` 与 `Run` 认领 running 使用同一 context 锁，没必要为“失效函数执行期间互斥”再加冗余 bool。需要的是覆盖整个发布事务的准入控制。
- `V0_IN_PROGRESS` 比旧 `V0_BLOCKED` 更符合当前状态。执行能力未完成不是外部依赖阻断。

证据与构建口径见 [evidence.json](review-publication-2026-09-08/evidence.json)。Android 是增量 FEX 构建、重新链接 shell ELF，不是 APK/JNI/ART 验证。测试文件使用独立设备路径，有界运行后清理。

## 2. 需要处理的发现

### P1-A：只读 pin 返回可写 span，绕过新 writer 准入规则

位置：[memory.h 的 PinnedSpan::Bytes](../../../src/core/guest_cpu/api/memory.h)，约 205–206 行；`AcquirePinnedSpan` 的只读准入及新增 M13c。

`AcquirePinnedSpan(range, false)` 返回 `Writable()==false`，但 `Bytes()` 无条件提供 `std::span<std::byte>`。调用者无需 cast 即可写入。对 RW mapping，在 quiesce 期间申请只读 pin 并写 `Bytes()[0]` 成功，即使同一时刻 `Write()` 已返回 Busy。因此“所有 writer 路径关闭”的声明不成立；对真正的只读 host mapping，该接口还允许 caller 表达会触发 host fault 的写入。

本轮最小探针在 macOS 和 Swan 都观察到：`reader_writable_flag=0`，内存成功写成 `153 (0x99)`，`explicit_write_accepted=0`。这是单线程确定性负例，无需并发运气。

修复方向：只读访问提供 `span<const byte>`；可写访问必须显式检查 writable lease，或使用不同的 ReadPinnedSpan/WritePinnedSpan 类型。不要只增加注释让 caller 自觉遵守 Writable 标志。回归应同时覆盖只读 span 的类型/拒绝路径、活动事务中的 writable 申请，以及正常写 lease 的释放。

### P1-B：token 归属只在 FEX adapter 校验，内存发布入口仍接受外来 token

位置：[address_space.cpp](../../../src/core/guest_cpu/api/address_space.cpp)，`InvalidateCode` 约 644–650 行、`PublishCode` 约 672–683 行。

两个入口只检查 IsValid 与数字 epoch。每个地址空间独立计数，A、B 第一次 quiesce 都是 epoch 1。持有 A 的 token，调用 B 的 PublishCode 或 InvalidateCode，会被接受；B 自己的真实 token 也仍处于活动状态。

本轮设备结果：`foreign_token_is_from_target=0`、`same_epoch=1`、`publish_accepted=1`、`invalidate_accepted=1`，B 内存被写成 `0x42`。G06j 只覆盖 `CpuContext::InvalidateCode`，不能证明 GuestAddressSpace 的另外两个入口也完成了绑定。

修复方向：所有 token 消费入口统一检查来源、存活及 active epoch，失败不修改字节、generation 或缓存。增加两个同时存活空间、相同 epoch 的负例；另测来源空间已销毁及 moved-from token。

### P1-C：公共 PublishCode/InvalidateCode 与真实 FEX 失效脱节，M07 尚未覆盖同一条链路

位置：[address_space.cpp](../../../src/core/guest_cpu/api/address_space.cpp) 约 654–666、690–694 行；[guest_execution_tests.cpp](../../../tests/guest_cpu/guest_execution_tests.cpp) 的 LoadFixture；[runner 的 M07 映射](../../../scripts/android/run-v0-tests) 约 158–164 行。

内存 API 只增加 `code_generation`，不会调用 FEX 失效。backend 只在生成 snapshot 时读取该 generation，没有在 Run 时据此拒绝旧翻译或自动同步缓存。现有 G08 的 LoadFixture 直接改 bytes、Protect，然后调用 **CPU context** 的 InvalidateCode；host M07 则只测 **GuestAddressSpace** 的 publication/generation。两套测试各自通过，不等于公共发布链路已接通。

本轮追加独立真实 guest 复现：先执行返回 A=17 的 fixture；经 GuestAddressSpace::PublishCode 写入 B=34，再调用该空间的 InvalidateCode；两个 API 均成功，generation 从 1 增到 3，但随后真实 guest **仍返回 A=17**。同一探针还证明持 token 时 Run 能进入，符合其没有完整事务门闩的源码事实。

原有 37 断言仍通过；扩充探针共 40 项，其中 2 FAIL，exit 1。见 [设备日志](review-publication-2026-09-08/publication-guest-swan.txt)、[临时 harness 补丁](review-publication-2026-09-08/guest-publication-review.patch)。该补丁只用于审核副本，没有改主仓执行测试。

修复方向：选定一个对外 publication coordinator，负责 context 准入、owner 停止确认、writer 排空、内存修改、共享/线程/alias 缓存同步及 generation 提交。避免两个同名 InvalidateCode 都声称“成功即不会执行旧译码”，其中一个却只改计数。事务建立应早于改字节；不能沿用 LoadFixture 的“先改，再 Quiesce”作为通用实现。

### P1-D：runner 仍丢失进程失败，并将部分用例升格为完整验收 PASS

位置：[run-v0-tests](../../../scripts/android/run-v0-tests)，run_suite/run_device_suite、约 514–532 行的聚合及 SUITE_MAP。

本轮使用明确标记的合成 suite：打印 M01a/b/c PASS 后 `exit 7`，完整 runner 仍产生 **M01 PASS、summary.failed=0**。退出码只留在未持久化的 output 字符串，不影响聚合；设备 timeout 还会返回空结果而退成 NOT_RUN。部分 subcase 已 FAIL、其他 subcase 缺失时也会被缺项分支记为 NOT_RUN。

另有未修复的范围问题：C01 没检查 flags，C02 没检查 MXCSR 舍入/host FP 恢复，D05 没覆盖完整非法指令类别，M13 仍无两个 host 线程之间的 pin/Unmap/Protect 竞争。M07 存在 P1-C 的跨 API 证据拼接。以上不能根据现有断言直接判完整 PASS。

当前 JSON 还把设备 C01/C02/M07/D05 写成 HOST、指向旧 host 日志；FEX SHA 仍为 f2b679f6、downstream=null，capabilities.backend 仍写 adapter 未实现。M02/M03 仍计 PASS，与当前延后 16 KiB 特有场景的 scope 不符。固定的 release_status_reason 即使未运行 guest 也声称设备 suite 全通过。

修复方向：suite 结果包含 exit_code、timeout、原始日志、产物 hash 和环境；失败优先于缺项。把“辅助证据通过”与“验收项完整通过”分别建模；DEFERRED_BY_SCOPE 单列且不入 PASS/当前分母；元数据及解释从本次实际观测生成。合成失败测试的 [JSON](review-publication-2026-09-08/synthetic-results.json) 仅是 runner 故障证据，绝不是 V0 成绩。

## 3. 下一轮可直接交给执行 AI 的任务

当前范围保持 Swan / Android 16 / ARM64 / **4 KiB**。不重新启动 16 KiB 适配，不新增网络/反射框架，不把游戏或 VR 纳入本轮完成条件。

| 顺序 | 交付目标 | 必须给出的证据 |
|---|---|---|
| 1 | 修 P1-A/B；收敛 pin 与 token 消费接口 | 本文两个负例变为拒绝；目标内存/generation 不变；原有 37/22/14 无回归 |
| 2 | 修 P1-D，结果与 scope 可信 | exit 非零、timeout、部分 FAIL+缺项都不能变 PASS/普通 NOT_RUN；无设备不声称设备通过；实际 SHA/日志/environment 可追溯 |
| 3 | 建立可复用 guest_cpu_fex CMake target、固定工具链/源码锁 | probe 与未来 JNI 复用同一 backend target；新构建目录可重建，保存 SHA/Build ID 和符号；public API 不泄漏 FEX 定义 |
| 4 | 完成 context 参与的发布事务，关闭 P1-C | 同一公共 API 从 A→B，恢复后只执行 B，100 次；token 活跃时新 Run/CreateThread/InvokeGuest 准入受控；失效失败不能解除保护后继续运行旧译码 |
| 5 | 运行线程停止与恢复 | 先可明确支持“全部已停止”的 TryQuiesce，遇到正在运行者返回 Busy 且不修改；再接入无 HLE guest 循环的中断、owner 回执和超时撤销，完成 T02/M09 |
| 6 | 真实 typed HLE + span lease/callback，继而 app | 8 整数/9 浮点/混合参数、callee-saved、长度和 pin 保护、两层 callback；之后 JNI Activity 执行同一 fixture，原生 Vulkan Surface 及 detach/后台恢复 |

步骤 1–3 是较小且可独立验收的提交；步骤 4–5 应按同一事务设计实施，避免先加局部标志再补相互矛盾的锁。TryQuiesce 的临时子集不等于完整 V0 Quiesce，也不能将正在执行线程的停止需求标成 PASS。禁止持 VM/CPU 锁等待 owner 回执，超时不得先写后失败；写入后若失效失败，至少保持不可执行/不可恢复并明确错误。

R7 的 HLE 长度关系和调用期 lease 仍未解决，必须在允许真实 HLE 接收 guest buffer 之前补齐。有限 Step、LLDB safe-point、ART 信号共存、app 生命周期仍是 V0 最终门槛；可在上述契约稳定后依次完成。Foundation 沿用现有基础能力，反射/网络按真实依赖与生命周期接入，不阻塞当前 CPU 事务修复。

## 4. 复现材料

- [内存负例源码](review-publication-2026-09-08/publication_review.cpp)：链接现有 address_space.cpp/status.cpp 即可，host 与 NDK 各编译一次。输出是漏洞观察值，退出 0 只表示探针正常完成。
- [Swan 内存负例](review-publication-2026-09-08/publication-swan.txt)、[原有 guest suite](review-publication-2026-09-08/guest-swan.txt)、[新 guest 发布负例](review-publication-2026-09-08/publication-guest-swan.txt)。
- [runner 合成 suite](review-publication-2026-09-08/synthetic-exit7.py)：在独立临时目录命名为 guest_cpu_contract_tests 并赋执行权限，把该目录交给 runner，输出也指定临时路径，避免覆盖正常 results.json。
- [证据清单](review-publication-2026-09-08/evidence.json)：版本、二进制/源码哈希与验证范围。历史临时目录路径是本次观察，不是可移植构建配置。
