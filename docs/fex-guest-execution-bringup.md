# FEX guest 执行接入（backend adapter）

当前为 **V0_IN_PROGRESS**。Swan / Android 16 / ARM64 / 4 KiB 上的独立 NDK/bionic ELF 已能经公共 CPU API 执行 x86-64 整数、分支、栈读写、显式 store/load、SSE2 fixture，并区分 return gate 与裸 HLT。2026-09-08 复核修复后执行 harness 36/36 通过；补上发布事务的两处准入缺口后为 **37/37**。这不是完整 V0 验收。

- 最新结果、根因与原始证据：[2026-09-08 复核与修复](validation/v0/followup-2026-09-08.md)。
- 历史审核：[8e0e85dc 审核](validation/v0/review-2026-09-08.md)。
- 公共 API：[context.h](../src/core/guest_cpu/api/context.h)；后端：[fex_context.cpp](../src/core/guest_cpu/fex/fex_context.cpp)。
- 汇编 fixture：[guest_fixtures.S](../tests/guest_cpu/fixtures/guest_fixtures.S)；测试：[guest_execution_tests.cpp](../tests/guest_cpu/guest_execution_tests.cpp)。
- 前置：[Android bionic 构建](fex-android-bionic-build.md)、[host page 适配](fex-host-page-size-adaptation.md)。16 KiB 已按用户要求后置。

## 1. 两个不同阶段的根因

### 配置层重载清掉 64 位模式（上一轮已修复）

`FEXCore::Config::ReloadMetaLayer()` 根据已注册的配置层重建 meta layer。嵌入方没有注册可重建相应值的层，却先 `Set(CONFIG_IS64BIT_MODE)` 再 `Reload`，导致该值被清掉。上一轮将顺序改为 Reload → Set，并读回检查，首次得到正确的整数结果。这是有效修复，但不能解释修正后仍然存在的所有失败。

### 共享译码缓存漏失效（本轮修复）

旧 `FexCpuContext::InvalidateCode` 只遍历存活线程调用 `ClearCodeCache(thread)`。harness 每次运行后销毁线程，下一次在同一 guest VA 写入新 fixture，再调用失效接口。此时线程集合为空，接口不做任何事却返回成功。

FEX context 的共享 CodeBuffer/L3 缓存仍然存活。重建线程后可以复用旧 VA 对应的第一段翻译。因此后续测试虽然名为 branch、store、SSE2 或 bare HLT，实际可能继续执行最初的 integer fixture。这解释了首个整数用例通过、后续结果不符，以及裸 HLT 被当作正常返回的组合。

修复使用当前 FEX 已有的公共接口：持有 `GetCodeInvalidationMutex()` 独占锁，先调用 `InvalidateCodeBuffersCodeRange` 失效共享翻译，再对存活线程调用 `InvalidateThreadCachedCodeRange`。没有存活线程时也必须执行第一步。这个协议可对照 FEX Windows frontend 的 `InvalidationTracker`；本轮没有修改 FEX 子仓。

## 2. 更正先前诊断

旧版本文的“内存访问指令没有效果”和“已排除陈旧缓存”推理过强，现撤回：

- guest/host 三种读法都读到零，只证明目标槽未见预期写入，不能证明已执行指定的 store fixture。
- push/pop 后 RSP 与初值相同，不能证明 push/pop 曾执行；一段不动 RSP 的旧翻译也会满足该断言。
- 同一 fixture 连续执行得到相同错误结果，不能排除稳定命中同一旧翻译。
- 改页内偏移、探查高 VA、优先怀疑 TSO，都不能替代核对当前 VA 实际命中的翻译。

仅修正 adapter 缓存失效，原有 33 项由 13 FAIL 变为全 PASS，已足以解决这组失败。无需为此继续修改 VIXL 或 TSO 设置。JIT 反汇编仍是后续诊断工具，不能以此次通过推断其他 FEX 模式均已验证。

## 3. 验证范围

| 测试 | 本轮结果 | 边界 |
|---|---|---|
| 原有真实 guest harness | 33/33 | 整数、分支、栈、store/load、SSE2 paddq、裸 HLT、契约拒绝 |
| 同 VA 替换，销毁后重建线程 | 100 次发布通过 | 暴露共享缓存跨线程生命周期存活问题 |
| 同 VA 替换，保留两个停止线程 | 100 次发布、每次分别运行两个线程通过 | 同一个 host owner 顺序运行；不是多 owner 并发测试 |
| 扩充后的 harness | 36/36 | 新增三项断言 G08a–c，包含线程清理 |
| 再补事务准入后 | 37/37 | 新增 G06j：他空间 token 被拒 |
| macOS host API/VM、typed ABI | 22/22、14/14 | 无 FEX；typed ABI 仍是手工 frame。API/VM 含新增 M13c |

构建使用本地 NDK 29.0.14206865、API 35，设备 Android 16 / SDK 36，运行时页大小 4096。执行的是 adb shell ELF，尚未验证 APK/JNI/ART。构建为增量 FEX 构建及重新链接，非 clean-room；产物 SHA-256、日志及源码补丁见最新证据。

## 4. 嵌入方需维护的状态

| 义务 | 当前处理 |
|---|---|
| GDT 在首次执行前建立 | 每线程持有 GDT，生命周期覆盖 FEX 线程 |
| CallRet stack 由嵌入方分配 | 每线程分配并随线程回收 |
| 配置重载与设置顺序 | Reload 后设置并校验 64 位模式 |
| 显式代码发布 | 共享缓存与线程缓存都失效；全局 publication/quiesce 事务仍待完成 |

fixture 从汇编源码生成，并保留反汇编列表，修正了旧 `49 01 C8` 被误当作 `add r8, r9` 的问题。汇编器可验证编码语法；反汇编便于审阅，但仍需独立检查断言与预期语义，不能声称“错误指令一定构建失败”。

`kGuestAddressPolicyLimit` 是 V0 的保守放置策略，不是 FEX 的 64 GiB 硬上限。FEX 对掩码索引命中仍比较完整 guest 地址；索引冲突不等于执行错误 block。放宽策略应单独做低/高 VA 对照实验。

## 5. 尚未完成的接口与验收

`Run` 的状态检查和 running 认领已在同一 context 锁下完成；`InvalidateCode` 在该锁下拒绝 running 线程，再完成上述缓存失效。因此本轮修复覆盖停止后的缓存更新。

### 发布事务：已补的两处与仍缺的一处（2026-09-08 后续）

复核报告列出的两个具体缺口已补，均有回归用例：

| 缺口 | 修法 | 用例 |
|---|---|---|
| `Write()` 不检查 active quiescence，可绕过事务 | 与 `AcquirePinnedSpan` 同一准入规则；只读 pin 仍放行，owner 经 `PublishCode` 发布 | M13c（host） |
| `InvalidateCode` 只查 `IsValid()`，接受他空间 token | 新增 `QuiescenceToken::IsFrom`，比对已持有的 liveness block | G06j（设备） |

`PublishCode` 原先也是"持锁查 epoch → 放锁 → 校验并 memcpy"，同一类窗口，已改为单次持锁完成。

**仍然缺的是 `Quiesce` 与 CPU context 的协作**。它只检查瞬时 pin，`stopped_threads` 恒为 0，
无法停止一个即将开始执行的线程。context 锁能保证 `Run` 与 `InvalidateCode` 不交错
（两者持同一把锁，本轮确认后撤销了一个多余的 gate 标志），
但那是互斥，不是"事务期间 context 参与停机"。
在此之前，token 不能当作多线程改代码的完整安全凭据。

### 其余未完成项

有限 `Step` 仍明确返回 Unsupported；中断/暂停、真实 HLE gate/callback、HLE buffer 长度与调用期 pin、信号与调试协议、JNI/app/原生 Surface 生命周期均未验收。SSE2 paddq 通过不等于 MXCSR 舍入测试通过，裸 HLT 分类通过不等于完整 D05 通过。旧 runner 仍有验收项范围与版本元数据问题，应以有界原始证据描述本轮结果。
