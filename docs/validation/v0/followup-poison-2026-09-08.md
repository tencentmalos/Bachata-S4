# 发布失败保护、sink 生命周期与执行准入（2026-09-08）

**结论：R1–R5 已全部关闭。停止状态下的完整发布事务现在成立：事务期间新执行被拒，失效失败后旧 JIT 代码不可达且可修复，注销 sink 会等待在途回调排空。状态仍为 `V0_IN_PROGRESS`：运行中线程的中断、真实 HLE 与 app 未开始。**

基线主仓 `95bc13fa6dc65961b46929d270daad876262d464`，FEX `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`（未改动）。设备 Swan / Android 16 / ARM64 / 4 KiB。本轮对应[poison 复核](review-poison-2026-09-08.md)的任务表第 1、2 项。

## 1. 三条 P1 的共同根因

R1、R2、R3 表面是三个问题，实际同源：**缺一个覆盖整个发布事务的准入门闩**。token 只排除了其他 writer，没有排除执行；poison 只是个没人读的标记；sink 指针取出后无人负责其生命周期。分散打补丁会互相矛盾，所以做成一套统一机制。

关键约束：锁序是 **context → space**（backend 持自己的锁读 `CodeGeneration()` 生成 snapshot）。因此门闩必须由 context **主动询问** space，绝不能反向回调。

### R1：poison 只是标记，失败后仍可恢复执行旧代码

复现（Swan，注入失败 sink）：`poison=1`，但 `Protect(RX)` **成功**，guest 重新执行旧常量 **17**。

根因是我上一轮只撤销了 guest 页面的 execute——而 ARM64 JIT 翻译住在 backend 自己的可执行内存里，撤销 guest 页权限对它毫无作用。

现在 poison 封锁两条回到执行的路：`AcquireExecutionLease` 和 `Protect` 的 execute 授予。并且按范围记录，**不再因无关范围发布成功而清除**（原实现 `code_poisoned = false` 无条件执行）。

| 观察 | 修复前 | 修复后 |
|---|---|---|
| `Protect(RX)` while poisoned | 成功 | **拒绝**（WrongState） |
| 执行 | 成功，跑旧 A=17 | **拒绝**（execution lease 封锁） |
| `R1_stale_code_executed` | 1 | **0** |
| 修复后执行 | — | 成功，**rax=34（新 B）** |

最后一行重要：这是**封锁**不是**砖化**。成功的失效或重新发布会解除封锁，见[设备日志](followup-poison-2026-09-08/r1-poison-swan-after.txt)、[探针](followup-poison-2026-09-08/poison_recovery_probe.cpp)。

### R2：sink 注销不等待在途回调

复现：`clear_returned_with_callback_active=1`、`old_sink_publication_succeeded_after_replacement=1`。我上一轮析构里"先 unregister 就不会再被调用"的注释不成立。

现在 `Clear` 等待 `sink_calls_in_flight` 归零后才返回，并引入 registration generation：在旧注册下开始的回调不能提交结果。

一个**测试方法学问题**：复核的探针只在 `Clear` 返回**之后**才释放回调，因此对正确实现必然死锁——它无法表达修复后的契约。我另写了从第三个线程定时释放的探针，两种结果都能观察：

```
clear_waited_ms=155
callback_completed_before_clear_returned=1   (修复前为 0)
stale_publication_reported_success=0         (修复前为 1)
```

见[探针](followup-poison-2026-09-08/sink_drain_probe.cpp)、[输出](followup-poison-2026-09-08/r2-sink-drain-host.txt)。

### R3：事务期间未禁止新执行进入

新增 `ExecutionLease`：backend 在 Run/CreateThread 前向 space 申请，事务活跃或 poison 时被拒；反过来，有 lease 未释放时 `Quiesce` 返回 Busy 且不修改任何东西。

Swan 实测（全部线程已停止）：

```
Run during transaction          = 0  [Busy in AcquireExecutionLease]
CreateThread during transaction = 0  [Busy in AcquireExecutionLease]
Run after transaction           = 1  rax=17
```

`Step` 不需要 lease：它在执行任何东西之前就返回 Unsupported。见[日志](followup-poison-2026-09-08/r3-admission-swan.txt)。

## 2. runner 的两处误计

### R4：无输出失败退回 NOT_RUN

根因是 taint 只遍历 `run.cases`——suite 在打印第一行前就死掉时该集合为空，什么都不会被标记。

改为按 suite **拥有**的子项传播（新增 `SUITE_OWNERSHIP`），与它打印了什么无关。同时区分**从未启动**（二进制缺失、无 adb、push 失败）与**启动后崩溃**：前者仍是 NOT_RUN，后者是 FAIL。把二者混为一谈会要么隐藏崩溃，要么给没构建的 suite 编造失败。

合成 suite（无输出 `exit 7`）：其拥有的 **11 个验收项全部 FAIL**，此前是 NOT_RUN、`failed=0`。

### R5：skip 仍计 PASS

`Skip()` 现在产生独立的 `SKIP` verdict，runner 视之为未运行的子项。macOS 输出变为 `28/29 passed, 1 skipped`。

这直接导致 M13 在纯 host 运行下变成 NOT_RUN——**这是正确的**，M13f 在 macOS 上确实没测到东西。但 Swan 允许 RWX 且真实通过了它。复核指出 runner 没有消费设备 contract 结果，所以我把 contract suite 也加入设备运行，并让结果按"FAIL 优先、真实判定覆盖 SKIP、SKIP 不覆盖真实判定"合并。M13 因此由**真实设备证据**补齐，而不是放宽规则。

验收项现在还记录证据来源（`environment`: HOST / DEVICE / HOST+DEVICE），此前一律硬编码为 HOST。

## 3. 构建入口的更正

复核对我上一轮的说法的纠正是对的：**`cmake/fex` 早已提交**（`3092cd10`），我说"host CMakeLists 只存在于临时目录且已丢失"不属实，丢的是 build 目录不是源文件。我新增的 `tests/guest_cpu/CMakeLists.txt` 是第二套入口，已**删除**。

真正缺的 `guest_cpu_fex` target 现在补在既有入口里，`V0_ENABLE_FEX=ON` 不再是 FATAL_ERROR（那段文字也已过时——adapter 早已实现并在执行 guest 代码）。四个设备二进制现在都由同一个 CMake 入口产出：

```
scripts/android/build-fexcore-android          # FEXCore 本体，编译选项不外泄
cmake -S cmake/fex -B <dir> -DV0_ENABLE_FEX=ON -DFEX_BUILD_DIR=<fexcore build>
```

FEXCore 仍不从这里配置：它自己的 CMake 会带来编译选项和 allocator 设置，不能泄漏进本工程（spec §3）。缺 `libFEXCore.a` 或 fixtures 时明确报错/警告，不产出链接得上但不执行任何东西的 stub。

## 4. 本轮结果

| 套件 | 结果 |
|---|---|
| host contract（macOS） | 28/29 passed, **1 skipped**（M13f 需要 RWX） |
| host HLE ABI | 14/14 |
| host page size | 22/22 |
| device contract（Swan） | **29/29**，exit 0 |
| device guest execution（Swan） | **41/41**，exit 0 |
| device bionic smoke（Swan） | 12/12 |

验收 **21 PASS / 0 FAIL / 37 NOT_RUN，58 项在范围内，2 项 `DEFERRED_BY_SCOPE`**，与上一轮相同且无回归。新增回归 M13h（poison 封锁与修复）、M13i（事务双向准入）、M13j（sink 排空）。

## 5. 未做与未验证

- **复核任务表第 3、4 项未开始**：运行中线程的中断与 stop epoch 回执（T02/M09）、真实 typed HLE 与 app。当前 `Quiesce` 遇到正在执行的线程返回 Busy 并且不修改任何东西——这是复核建议的"限定 TryQuiesce"，不是完整的停止能力。
- **TSan 仍未验证**：本机 TSan 连最小 `std::thread` 程序都段错误（exit 139、无输出），与被测代码无关。M13g/M13j 只有断言覆盖，没有数据竞争检测器覆盖。
- **`ExecutionLease` 用裸指针**而非 weak 引用，依赖"backend 不得比 space 活得久"这一既有契约（`CreateContext` 已声明）。若将来允许 space 先销毁，这里需要改。
- **poison 按单一范围记录**：并发发布多个不同范围时，后一次失败会覆盖前一次的记录范围。当前只有单范围发布场景。
- **M13f 在 macOS 上仍是 SKIP**，其 execute 撤销断言只由设备执行。
