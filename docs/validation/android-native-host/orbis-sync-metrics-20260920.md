# Orbis 同步原语：HLE 量化与对象级唤醒

日期：2026-09-20。分支：`feature/malos/hle_vr`。本轮在用户要求下先交付代码和交接记录；**尚未完成新增 counter 的真实游戏 PROF/Litep 验收，也没有游戏帧率提升结论**。

## 已实现的统计

`src/core/host_runtime/guest_sync_metrics.{h,cpp}` 为每个 runtime context 创建独立 Session；`guest_runtime.cpp` 在 FunctionAdapter 绑定时按可读导入名分类，调用时统计。sce/POSIX 别名汇入同一操作，原有 `HLE.sce…` / `HLE.pthread…` scope 保留精确入口。覆盖 mutex、condition、rwlock、POSIX sem_t、kernel semaphore、event flag、join、once、yield、sleep/usleep/nanosleep、epoll wait，以及属性操作。

默认 OFF。关闭时不创建统计 shard、不读计时器；详细内部阶段只检查线程局部 observer。开启后每个物理线程独立写 shard，异步读取使用原子字段；没有每次调用的共享统计 mutex/atomic RMW。第一次注册线程和汇总读取使用 Session 的短登记锁。Session ID 独立递增，避免 TLS cache 混入下一次会话。极端分配失败记 `untracked_calls`，定时导出失败记 `emission_failures`。

每操作量化数据：

- `calls`、`completed`、`inflight`；未返回的等待不能被当成已完成短调用。
- `elapsed_ns`、`max_ns`，六档耗时直方图：小于 1 µs / 10 µs / 100 µs / 1 ms / 16 ms，以及不小于 16 ms。
- `nonzero`、已知状态码的 `timeouts` / `busy` / `interrupted`，独立的 host error 和 exception。
- 非零返回不等于错误；try-lock 忙是正常结果。epoll 成功返回事件数，sleep 可返回剩余时间，getter 可返回数值，不能按 errno 解释。POSIX sem 的 -1 不读取额外 guest errno，保留为未细分非零，不能伪造超时分类。
- detailed 模式中的阶段次数和耗时：Lookup、Guard、Park、Publish、Reacquire。`phase_mask` 表明该操作哪些阶段已实际插桩，0 表示未覆盖，不能解释成零开销。

时间是 **HLE 调用内部、含等待的 inclusive elapsed**，不含整个 FEX→HLE veneer/marshaling，也不是 CPU 时间或关键路径。多线程及嵌套操作的耗时不能直接相加当帧长。Park 包含条件变量等待及其内部 guard 重获，不证明线程持续在内核睡眠；真正的 CPU/runnable/sleep 仍须同窗 sched sidecar。

读取是 rolling snapshot，跨字段非事务一致；静止后总计精确。start/stop 不清零，进行中的调用仍把结果记入开始时的采样。需要区间量时取同一 Session 两个 snapshot 的累计差，保留跨边界调用和 pending 数量，不能从 `max_ns` 的差得到区间最大值。

## DebugBus / Litep 使用

先启动游戏，读取当前 `context`：

```sh
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService hle_sync status
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService hle_sync start CONTEXT
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService hle_sync detail CONTEXT
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService hle_sync dump
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService hle_sync stop CONTEXT
```

`start` 为汇总模式，`detail` 额外开启内部阶段及定位事件；二者均要求当前 context，拒绝陈旧控制。`status` 返回 JSON，不发 trace；`dump` 返回 JSON 并向正在运行的 litep ring/file 输出一次快照。开启期间约每 500 ms 输出一次。进程重启不能沿用旧 Session；无 runtime 时返回 `no_session`。

启动前也可设置 `debug.shadps4.hle_sync=1` 或 `detail`，但需要在结束后清为 `0`。旧 `debug.shadps4.profile_sync` 仍控制此前的 checked caller/owner/condition enqueue→notify→resume→reacquire 观察，不被这次命令隐式打开。

Litep 名称均为进程生命周期内的静态字符串：

| 类型 | 名称 / 解释 |
|---|---|
| 汇总 scope | `HLE.Sync.Snapshot` |
| 身份/质量 | `HLE.Sync.Session`、`Context`、`SnapshotNs`、`Enabled`、`Detail`、`Shards`、`UntrackedCalls`、`EmissionFailures` |
| 每操作累计 counter | 例如 `HLE.Sync.Mutex.Lock.Calls`、`.Completed`、`.ElapsedNs`、`.Ge1ms`、`.Ge16ms`、`.Busy`、`.Timeouts`、`.Interrupted`、`.HostErrors`、`.Exceptions` |
| 阶段累计 counter | `.PhaseMask`、`.LookupNs`、`.GuardNs`、`.ParkCalls`、`.ParkNs`、`.PublishNs`、`.ReacquireNs` |
| detailed 阶段 scope | `HLE.Sync.Lookup/Guard/Park/Publish/Reacquire`，嵌套在原始 HLE 入口下 |
| 等待入口定位 | 阶段入口立即输出 `HLE.Sync.Wait.Operation/Arg0/Thread/ThreadGeneration`，即使调用不返回也留有定位依据 |
| 已返回长调用 | `HLE.Sync.LongCall`，附 `.Long.Session/Operation/Kind/Arg0/Thread/ThreadGeneration/BeginNs/EndNs/Result` |

`Arg0` 是原始第一个 ABI 参数：可能是指向句柄的 slot、32 位 semaphore ID 或时间参数，并非统一的真实 mutex 地址。`Kind` 对应 JSON 行的 `kind`；`Operation` 是本 Session 的 HLE operation ID。长调用定位最多每物理线程每秒 8 笔，被抑制数通过 `.LocatorDropped` 保留；完整调用计数和耗时直方图不采样。等待入口定位和详细阶段没有这个限频，因此高频 detail 模式可能显著扰动。

可用现有 Litep `list_counters(name contains HLE.Sync)`、`query_counters(accumulated=true)` 比较累计增量；`MaxNs` / `PhaseMask` / `Detail` 是状态值，不能使用 accumulated。`find_scope_hotspots` 查询详细阶段，再用完整 source frame 和 sched/KGSL 窗口定位等待链。**这些实际 PROF 消费步骤本轮未跑完，不能把 mock transport 单测写成 MCP 验收。**

## 已修复的无关对象唤醒

POSIX sem_t 和 rwlock 之前各自整域共用一个 condition_variable_any，操作 A 会唤醒等待 B 的线程。改成每对象 CV：POSIX Post 唤醒一个 waiter；若该 waiter 在取得 token 前取消，持 predicate guard 将可用 token 的通知交给另一个 waiter。rwlock 仍在同一对象内广播，以保留读者/写者选择与取消协议。

domain metadata mutex 仍在，查找/guest slot 校验仍可能竞争；这次不是完整按对象分锁。Kernel semaphore 之前已完成每 waiter 定向通知，本轮只增加阶段测量，不重复声称修复。Mutex 已经是每对象 guard/CV，没有照搬桌面 adaptive mutex 的 2000 次 spin。

| 32 次操作无关对象 | AYN Android 原版→本版 | Linux x64 同一 HLE 代码 原版→本版 |
|---|---:|---:|
| POSIX sem 等待线程主动切换 | 33→1 | 34→1 |
| rwlock 等待线程主动切换 | 34→1 | 33→1 |

这是 getrusage(RUSAGE_THREAD) 实测，允许初始进入等待、取消和调度噪声。旧二进制运行 `--assert-isolation` 退出 2，新版通过。并非从重叠 scope 推断出来，也不是整游戏收益。

## Linux / Windows x64 on Android 对照

桌面 Linux 的 `threads/mutex.cpp` 使用 TimedMutex；非 Windows 的 TimedMutex 包装 std::timed_mutex，adaptive 类型可先 try-lock/spin，默认 error-check 不自旋。桌面 rwlock 使用每对象共享锁；Android HLE 另有逻辑 guest owner、checked memory、取消和生命周期成本，不能直接换成 pthread mutex 就声称 ABI 等价。

新增 `tests/host_runtime/guest_sync_performance.cpp` 同时运行 production Android HLE domain 和 native/std/futex 机制：1/4/12 线程，共享/独立对象，3轮、每线程4000对操作；输出 wall ns/pair、所有工作线程 CPU ns/pair 和抽样 p50/p95/p99。Android 与 Linux 各有原版和新版 162 行。**native 行不是完整桌面 Orbis ABI，对比尚未包括 FEX。**

Linux x64 为 Ubuntu20.04/kernel5.4，Clang16 + 任务目录中私有 libstdc++12/glibc2.36；Android为 NDK r29/arm64/API33/Bionic。未固定 CPU 频率/亲和性，Linux 主机也不是独占；不要把跨机器绝对比值归因为 Android 内核或 FEX。源码原版来自 aeb087d2，新增 bench 在本提交。

参考已核对的 [Wine critical section / WaitOnAddress](https://github.com/wine-mirror/wine/blob/7b3fff76fa5178f6ce0141b2c776afa2a822f101/dlls/ntdll/sync.c) 和 [Unix wait backend](https://github.com/wine-mirror/wine/blob/7b3fff76fa5178f6ce0141b2c776afa2a822f101/dlls/ntdll/unix/sync.c)：值得采用的是无竞争原子快路径、有限自旋、按地址队列、期望值等待与先唤醒后睡眠防丢失协议；Wine 自身仍有内部短 spinlock，不能描述成完全无自旋。Linux [NTSYNC](https://docs.kernel.org/userspace-api/ntsync.html) 专用于 NT 语义，需要内核支持，不能直接当通用 Orbis 后端；本机是否可用未探测。

HLE FEX 快路径必须作为**随 APK/应用发布的运行时能力**，不放在 `guest/games/CUSA…` patch。后续设计须统一 guest 原子 owner/depth、host 慢路径、condition release/reacquire、Destroy、Stop、映射退役和 ABA；禁止 guest CAS 和 host owner/depth 同时各自认定成功。此前 compiled guest prototype 证明跨边界有成本，但不是生产 Orbis ABI；本轮未新增生产快路径。

## 验证与交付边界

[原始小型证据与 artifact SHA](evidence/orbis-sync-20260920/validation.json)：

- 新 metrics：Android 45/0，LLVM ThreadSanitizer 45/0；线程分片、嵌套、停止后在途完成、Session更换、异常/错误码/数值返回、长调用限频均覆盖。
- 最终 Android：rwlock60/0、mutex65499/0；64轮并发 Post/Stop 无漏唤醒（检查数随获胜分支变动）；新无关唤醒断言通过。Linux同样64轮通过，LLVM TSan64轮通过，无race报告。
- 阶段插桩初版还跑过 condition67/0、services78/0、kernel semaphore588/0；不要把这些写成所有最终功能的完整回归。
- 统计本身的无 SDK 微基准：AYN汇总模式稳态约116ns/次；详细模式加一段空 Guard 约230ns/次。该数字不包含真实 Litep 写事件、额外阶段或整游戏扰动；完整 OFF/ON/OFF 尚未做。
- 最终 host RelWithDebInfo 与普通 APK 构建/安装成功：APK `3ba6e584…`，host `914dd36f…`。游戏二进制和构建产物不入 Git。
- 中间诊断 APK 曾在 Bloodborne 显示真实诊所画面，`hle_sync status` 能返回 Session/context，但当时统计 OFF，未捕获新的 PROF。用户要求优先提交后，停止后续采集；最终含对象级唤醒 APK 尚未做真实游戏流程验收。
- 设备已通过 Settings → Graphics 将 Internal Scale 从用户此前0.25切回0.5；代码默认本来就是0.5。当前停在 Settings，没有运行中的游戏；下一次启动生效。未新增自动输入、调试器或文件 capture。

首轮 Android benchmark 不慎与用户游戏并行的文件留在本地 build，不作为可比性能证据；提交的是确认 Stop 后重跑的 idle 文件。无全游戏回归、完整 Linux Orbis ABI 测试、HLE FEX 生产快路径或 FPS 改善结论。
