# Orbis 同步原语离线评估：锁粒度、自旋、唤醒、HLE 统计与 CPU/GPU 等待链

日期：2026-09-20。评估基线：`feature/malos/hle_vr` @ `1db95bff`（根仓库 `C:\workspace\emulations\shadps4` 直接检出，子模块 gitlink 未更新）。数据包：`shadps4-orbis-sync-traces-20260920.zip`，SHA-256 `8b47170a…b413` 与交接清单一致；`python3 prepare.py` 校验 283 文件、3 组 PROF/KGSL 身份绑定通过。

本报告是**离线评估**：无设备、无 NDK r29、无可重建 Litep 索引的 MCP/SDK。三组 trace 的数字来自包内已生成的分析 JSON，加上我对原始 KGSL/sched ftrace 的独立重算；PROF 本身没有重新解析。三组 trace 均早于 `a63e900f`（kernel semaphore 定向唤醒）和 `5f025529`（POSIX sem/rwlock 每对象 CV、HLE 统计），其中只有 01 组在 TSC 统一之后。凡引用 trace 的数字都是**历史版本**的观测，凡引用源码行号的都是**当前代码**。

## 0. 做了什么、没做什么

| 项目 | 结果 |
|---|---|
| 源码审查 | `guest_mutex.h`、`guest_kernel_semaphore.h`、`guest_semaphore.h`、`guest_rwlock.h`、`guest_sync_metrics.{h,cpp}`、`guest_runtime.cpp` 同步/时钟/线程绑定、`fex_context.cpp` HLE 边界、`veneer_allocator.cpp`、`scope.cpp`、桌面 `threads/mutex.cpp` 对照、compiled guest 原型与 custom SDK v1 |
| 独立测试（本机可跑） | `tests/host_runtime/sync_metrics` 用 MSVC 2022 x64 Release 编译运行：**45 checks PASS**；`--overhead` 关闭 0.18 ns、汇总 33 ns、详细 64–66 ns/次（Windows x64 桌面，不是设备数） |
| 独立测试（本机不可跑） | `sync_performance`（domain bench / `--assert-isolation`）、`guest_sync_isolation_tests` 依赖 Linux `RUSAGE_THREAD`/guest address space，本机无 Linux/设备；未运行 |
| trace 独立重算 | 从三组 `kgsl_trace.txt.gz` 重算 GPU busy（`kgsl_pwrstats`/`kgsl_gpubusy`）、每批 GPU 执行时长（`adreno_cmdbatch_retired` start/retire tick）、提交时队列深度、GPU 频率占比；脚本与输出见 [evidence](evidence/orbis-sync-offline-review-20260920/) |
| 未做 | 重建 PROF 索引、任何设备采集、FEX 子仓改动、生产代码修改 |

## 1. 结论总览（按 收益 × 证据 排序）

| # | 结论 | 性质 | 证据 | 预期收益（帧时长口径） |
|---|---|---|---|---|
| 1 | 血源主生产者 Guest-1 每帧约 **24%** 在等 CSEzWork 任务组，而该任务组内**任务回调本身合计 < 1 ms**，其余全是队列锁 handoff：每次 handoff 走 `SignalSema`→futex 唤醒→`WaitSema` 返回，历史 trace 中约 180–190 µs/次。当前 host 代码的 notify-under-lock 让每次唤醒至少多一次 futex 往返 | 确定（trace + 源码） | §2.2、§3.3 | 推断 5–8%，取决于 handoff 中 host 部分的占比 |
| 2 | Guest-1 每帧非等待型同步 HLE（mutex lock/unlock、SignalSema、broadcast）占 **约 10%** 帧时长，另有约 3.6k 次/帧的 FEX↔HLE 跨界固定成本（设备实测空 HLE 1.1–1.7 µs/次）**约 4%**；这些锁绝大多数无竞争（Lock 3.7 µs、Unlock 2.1 µs 均值） | 确定（trace）；收益为推断 | §2.3、§3.1 | 通用 guest 快路径上限 **12–14%**，仅当 Guest-1 仍是关键路径 |
| 3 | 每次 HLE 调用固定成本：3 次 context 全局 `lock_`、一次完整寄存器快照（含 XMM 重建）、`Current()` 的 `threads_mutex` 与 `shared_ptr` 拷贝、registry 查找。全进程约 **8–10 万次 HLE/s**，其中同步类约 8 万次/s | 确定（源码）；单次 ns 为推断 | §3.1、§3.2 | 是 #2 的组成部分；`Current()` TLS 缓存可单独低风险落地 |
| 4 | GPU 未饱和：三组 GPU busy **54.7% / 45.8% / 42.2%**，但主场景批次单批 **70–78 / 88–97 / 96–105 ms**，提交深度几乎恒为 1。0.5 倍率下 GPU 每帧下限约 75 ms（≈13 FPS）；CPU 从 130 ms 降到 ~75 ms 之前仍是 CPU 瓶颈 | 确定（KGSL 重算） | §2.4 | 决定 CPU 优化的天花板，不是本轮改动项 |
| 5 | kernel semaphore waiter 停在**域锁**上（`condition_variable_any` + 域 `mutex`），Signal 在持锁时 notify → 被唤醒者立刻撞域锁再睡一次；mutex/cond/POSIX sem/rwlock 同样在持对象或域锁时 notify。EventFlag 已是正确顺序 | 确定（源码） | §3.3 | 与 #1 合并 |
| 6 | 低优先级确定项：rwlock 每个 waiter 退出都 `notify_all`（惊群）；POSIX sem 用域锁做 CV 锁；kernel sema `Trace()` 每次调用做共享原子 RMW；absolute 时间的 cond 等待每 20 ms 轮询；无 host 端自旋（与桌面 adaptive 2000 spin 不同，但缺数据判断优劣） | 确定（源码），影响未量化 | §3.4–3.6 | 小 |
| 7 | 新 HLE 统计实现未发现正确性缺陷；口径缺口：不含跨界成本（短操作被低估 30–40%）、`sceKernelWaitEqueue` 未分类、`IsOwned` 归入 Config、stop 后 worker 仍每 500 ms 醒一次 | 确定（源码 + MSVC 单测） | §3.7 | 影响解读，不影响正确性 |

不建议做的事：把通用同步实现放进 `guest/games/CUSA…` patch；改 FEX 子仓的 TSC 缩放算法来调血源自旋预算；在没有 ParkNs 数据前给 host 慢路径加自旋；改 kernel semaphore 的 FIFO/优先级语义。

## 2. 三组历史 trace 里的等待链

### 2.1 帧与主等待

`frames.json` 的 GNM source 帧（提交 epoch，不是 present）：

| 组 | 完整帧 | 帧长 p50 / p90 / max (ms) | 帧数/时窗 | Guest-19 等 Guest-1 每帧 p50 (ms) | notify→resume / reacquire p50 (ms) |
|---|---:|---|---:|---:|---|
| 01 TSC 后 | 49 | 129.6 / 136.5 / 145.0 | 7.9/s | 104.9（49 次） | 0.032 / 0.002 |
| 02 TSC 前 | 60 | 198.1 / 241.4 / 261.4 | 5.0/s | 170.9（60 次） | 0.036 / 0.003 |
| 03 上传锁拆分 | 42 | 233.4 / 265.1 / 287.6 | 4.3/s | 194.6（42 次） | 0.039 / 0.004 |

三组里帧 owner Guest-19 被通知后恢复与重获 guest mutex 都在 0.1 ms 以内。**condition 的 notify/reacquire 路径不是问题**，问题在通知之前的生产工作。

### 2.2 Guest-1 的任务组等待 = 队列锁 handoff

01 组 `job-summary.json`（242 探针，跨 6 个 CSEzWork worker 累计，不能加成单帧）：

| 探针位置 | 含义 | 次数 | elapsed | on-CPU | runnable | sleep | 内含 HLE |
|---|---|---:|---:|---:|---:|---:|---|
| `+0x20336f3` | 取队列锁 | 12,600 | 6,640.6 ms | 1,558.8 | 387.9 | 4,693.8 | `WaitSema` 5,319 ms elapsed / 319 ms CPU |
| `+0x203373f` | 解锁+`SignalSema` | 12,601 | 535.2 | 460.8 | 27.2 | 47.2 | `SignalSema` 494 ms / 430 ms CPU（≈ 34 µs CPU/次） |
| `+0x2033752` | 任务回调 | 10,109 | 874.8 | 531.3 | 184.9 | 158.7 | p50 **0.004 ms** |
| `+0x2033827` | 查 allocator（rwlock） | 10,109 | 232.0 | 155.0 | 53.0 | 24.0 | rwlock rd/unlock 61 ms |

`long-task-waits.json` 的四笔 Guest-1 任务组等待（31.1–32.5 ms）里，最后通知者 worker 在同一窗口内：取锁 29 次 / 29.3 ms（CPU 6.2、sleep 22.0，`WaitSema` 24.5 ms），Signal 29 次 / 2.3 ms（CPU 2.0，≈ **70 µs/次**），任务回调不进前三（< 0.4 ms）。6 个 worker × ~29 次 ≈ 174 次串行 handoff 覆盖 32 ms，即 **≈ 185 µs/handoff**。

`spin-budget-evidence.json`：TSC 修复后 12,600 次取锁中 6,828 次（54%）自旋到预算 0.1665 ms 后进入 `WaitSema`；修复前 15,973 次只有 551 次（3.4%）进入等待，但每次先烧 10.4 ms CPU。**修复前是烧 CPU，修复后是排队等 host 唤醒**；两种情形任务回调 CPU 都只占任务组等待的很小一部分。

推断（标注为推断）：血源 `DLAdaptiveMutex` 的解锁是“写交接哨兵 + `SignalSema(1)`”的直接交接协议，一旦有 waiter 登记，后续每次解锁都交给睡眠者，自旋者抢不到锁，形成 lock convoy。PS4 上 futex 唤醒是几微秒，这里每次 handoff 含 HLE 进出、host 域锁、futex 唤醒、Android 调度延迟（worker runnable 平均 31 µs），放大到百微秒级。这是游戏算法与 host 唤醒延迟的相互作用，通用可做的只有压低 host 侧每次 handoff 的成本。

### 2.3 Guest-1 lane 内的同步 HLE 占比

`frame-*.json` 的 `owner_and_explicit_producer_contributions`（每帧只有前 15 行，03 组 MutexUnlock 因此被截掉，属低估）：

| 01 组 4 帧合计 558.1 ms | elapsed | 占帧 | 次数 | 均值 |
|---|---:|---:|---:|---:|
| 未插桩/JIT 区间 | 304.8 ms | 54.6% | – | – |
| `scePthreadCondWait`（等任务组） | 134.0 | 24.0% | 12 | 11.2 ms |
| `scePthreadMutexLock` | 25.7 | 4.6% | 7,007 | 3.7 µs |
| `sceKernelWaitSema` | 20.0 | 3.6% | 162 | 123 µs |
| `scePthreadMutexUnlock` | 14.6 | 2.6% | 6,992 | 2.1 µs |
| `pthread_mutex_lock` | 9.1 | 1.6% | 354 | 25.6 µs |
| `sceKernelSignalSema` | 3.3 | 0.6% | 50 | 67 µs |
| `pthread_cond_broadcast` | 3.2 | 0.6% | 120 | 26 µs |

非等待型同步 HLE 合计 **≈ 10.0%**；对应约 3.6k 次调用/帧，按设备实测空 HLE 往返 1.1–1.7 µs（`docs/guest-compiled-functions.md`），跨界固定成本 **≈ 4–6 ms/帧（3–4%）**，不在上表 scope 内。03 组同类合计约 7.3%（Unlock 被截）且 Lock 均值升到 7.3 µs（更多竞争）。

03 组全窗口 `scope-summary.json`（9.976 s，全部线程）给出调用量级：`scePthreadMutexLock/Unlock` 各 251.7k（25.2k/s，均值 13.5 / 5.6 µs）、`pthread_mutex_lock/unlock` 32k/34k（均值 28.6 / 5.6 µs）、`scePthreadSelf` 191k（19.1k/s，body 0.6 µs）、`RwlockRdlock/Unlock` 各 14.7k、`WaitSema` 7.8k、`SignalSema` 5.8k（均值 **135 µs**）、`CondWait` 2.3k、`cond_broadcast` 2.4k（38.7 µs）。同步类 HLE 合计约 **8.1 万次/s**；加上 `sceGnmIsUserPaEnabled` 72k、`SetPs/VsShader` 72k、`ClockGettime` 10.6k 等，全部 HLE 约 9–10 万次/s。

推断：POSIX 别名 `pthread_mutex_lock` 均值 26–48 µs 远高于 `scePthreadMutexLock` 4–7 µs，很可能是竞争锁（如 guest libc 分配器锁）走了 Park；需要新 `hle_sync detail` 的 `Mutex.Lock.ParkCalls/ParkNs` 才能确认。

### 2.4 CPU 调度与 GPU（独立重算）

`cpu-gpu.json` 线程状态（sched_switch/sched_wakeup，非 HLE 推断）：

| 03 组 9.976 s | on-CPU | 其中 HLE | runnable | sleep |
|---|---:|---:|---:|---:|
| Guest-1 | 5,334 ms | 1,128 | 1,376 | 3,266 |
| GpuComm | 2,571 | 0 | 38 | 7,366 |
| Guest-20…24（各） | 2,280–2,350 | 460–515 | 1,220–1,360 | 6,330–6,450 |
| Guest-19（帧 owner） | 1,328 | 486 | 210 | 8,437 |
| Guest-51…56（CSEzWork，各） | 1,155–1,312 | 61–82 | 353–449 | 8,280–8,470 |

前 30 线程 on-CPU 合计约 3.45 核；Guest-20…24 每个都有 1.2–1.4 s runnable，存在真实调度竞争。01 组 Guest-1 on-CPU 3,336 ms/6.27 s，其中 HLE 826 ms（25%）。

从原始 KGSL ftrace 重算（脚本 `kgsl_busy.py`）：

| 组 | GPU busy（pwrstats / gpubusy） | 批次执行时长求和占窗 | 批次 p50 / p99 / max (ms) | 提交时深度 1 / 2 / 3 / ≥4 | GPU 频率占比 |
|---|---|---:|---|---|---|
| 01 | 54.7% / 54.4% | 54.2% | 0.49 / 70.2 / 77.7 | 493 / 73 / 68 / 7 | 550 MHz 43%，615 19%，680 24% |
| 02 | 45.8% / 45.4% | 45.5% | 0.49 / 85.5 / 96.7 | 1080 / 104 / 91 / 8 | 615 47%，680 38% |
| 03 | 42.2% / 41.6% | 41.8% | 0.51 / 89.2 / 105.2 | 929 / 103 / 63 / 23 | 615 40%，680 28% |

三种口径一致，可信。最长批次退休时 `inflight=0`，即主场景批次独占 GPU 70–105 ms 且没有下一帧重叠。KGSL submitted→retired 与 `Vulkan.Post→WorkerSubmit` 排队 P99 70–88 ms 是同一批次的等待，不与 CPU 帧相加。**含义**：01 组 CPU 帧 ≈ 130 ms、GPU 主批次 ≈ 75 ms，当前 CPU 瓶颈；若 CPU 侧优化把帧压到 75 ms 以下，0.5 倍率下 GPU 立即成为下限（≈13 FPS）。GPU 频率大部分时间未到 680 MHz 顶点，属于 DVFS 观测，不据此下调度结论。

## 3. 确定问题（当前代码，按影响排序）

### 3.1 每次 HLE 调用的固定成本与全局串行点

`src/core/guest_cpu/hle/veneer_allocator.cpp:13-27`：veneer 是 `mov r10,rcx; mov rax,imm64; syscall; ret`，每次 HLE 都是一次 FEX syscall 出口。`src/core/guest_cpu/fex/fex_context.cpp`：

- `:1903-1921` 进入时持 context 全局 `lock_`，`CaptureSnapshot(…, HleBoundary)` 复制全部 GPR、重建 EFLAGS、重建全部 XMM（`:1917`），并 `stopped_changed_.notify_all()`。
- `:1957` 构造 `HleScope`；`:634` `registry_.Find(operation)` 返回 `shared_ptr`。
- 返回路径 `resume_continuation`（`:1566-1610`）再持两次 `lock_`（取消检查、`AcquireExecutionLease`），并可能 `WaitForQuiescenceRelease`。

一次 HLE 至少 3 次全进程 `lock_` 与一次几百字节的快照拷贝，与 8–10 万次/s 的调用率相乘，是一个随 HLE 频率线性增长的 GIL 式串行点。快照是调试器/安全点契约的一部分，不能删；但它使“减少 HLE 次数”成为比“优化 HLE 内部”更高杠杆的方向。单次成本没有独立测量，设备上空 HLE 往返 1.1–1.7 µs 是唯一实测参照。

### 3.2 `Current()` 每次调用取 `threads_mutex`

`src/core/host_runtime/guest_runtime.cpp:957-963`：`Current()` 每次 `lock_guard(threads_mutex)` + `map::find` + `shared_ptr` 拷贝。调用点：mutex Lock/Unlock/CondWait（`:1989-2001`, `:2011-2029`）、kernel sema 全部操作（`:1656`，仅为取 `attributes.priority`）、rwlock（`:1936`, `:1948`）、`scePthreadSelf`（`:2310-2311`）、`ErrnoAddress`（`:1527`）。按 03 组约 8 万次/s。`active_thread` 已是 `thread_local`，用 `thread_local` 缓存 `Owner*`（带 generation 校验，Attach/Finish 时更新）即可去掉这把全进程短锁；这是 2026-09-17 审计已列出但未落地的项。

### 3.3 持锁 notify 与 waiter 停在域锁上

| 位置 | 现状 | 后果 |
|---|---|---|
| `guest_kernel_semaphore.h:100-113` `Wake()`、`:256-263` Signal | 在域 `mutex` 内 `waiter->changed.notify_one()`；waiter 在 `:190-192` 以域 `mutex` 为 CV 锁 `wait` | 被唤醒者需重夺仍被 signaler 持有的域锁，至少多一次 futex 往返；`condition_variable_any` 自身还有内部 mutex，一次唤醒最多三次锁交接 |
| `guest_mutex.h:352-362` Unlock | 在对象 `m.guard` 内 `notify_one` | 竞争 handoff 同样多一次往返（仅限该对象的 waiter） |
| `guest_mutex.h:459-469` CondNotify | 在 `state->guard` 内 notify | 同上 |
| `guest_semaphore.h:83-94` Post、`:121,141` Wait | 域 `guard` 作 CV 锁，持锁 notify | 所有 sem_t 共享一把锁 |
| `guest_rwlock.h:142-155` Unlock、`:122` `Waiting` 析构 | 域 `mutex` 内 `notify_all`；每个 waiter 退出都 `notify_all` | 惊群 O(n²) |
| `guest_runtime.cpp:1786-1795` EventFlag Set | 先释放 `ef->mutex` 再 `notify_all` | **正确顺序**，可作参考 |

Bionic `pthread_cond_signal` 不做 wait-morphing，这一往返在 Android 上是真实开销。修法是标准的：持锁选定 waiter、置 `done`、出队，释放域/对象锁后再 notify；kernel sema 的 waiter 应停在自己的 mutex/CV（或 futex 字）上而不是域锁。这直接作用于 §2.2 的 handoff 链。三组 trace 中 `SignalSema` 70–135 µs 还包含旧代码的跨对象广播，**当前代码已好于 trace**，但持锁 notify 与域锁停车在当前代码仍在。

### 3.4 kernel semaphore 的其他共享成本

`guest_kernel_semaphore.h:50-72` `Trace()`：除 Create 外每次 Wait/Signal 都对 `static std::atomic<u32> count` 做 `fetch_add`（`:57`），一次 Wait 调用 2 次、Signal 1 次；只前 256 次真正打日志。这是全进程共享缓存行上的原子 RMW，应改为按需/首次采样。`Dispatch` 每次做多段 `string_view` NID 比较、`make_shared<Waiter>`、`std::list` 插入与 `remove`（O(n)）；每次 Wait 前为超时指针做 `AcquireDataSpan` 与 `Query`（`:143-152`）。单项都小，但都在 handoff 关键路径上。

### 3.5 mutex 域

- `guest_mutex.h:357` Unlock 与 `:338` Lock 在对象 `m.guard` 内做 checked VM 写（`WriteOwnership`），其内部再取 `GuestAddressSpace::lock` 并可能等待 `WaitDataAdmissionLocked`。锁序 注册表→对象→VM 一致，没有发现反序；但只要 host 持有权威 owner/depth，每次 unlock 都必须写 guest 内存，且映射事务期间会把对象 guard 一起卡住。这是 guest-owned 状态要解决的根因。
- `:575-587` absolute 时钟的 `CondTimedwait` 每 20 ms 醒一次重查时钟。血源 trace 里没有出现该入口，但对使用 `pthread_cond_timedwait` 绝对时间的引擎是稳定的 CPU 税。
- `:502` CondWait、`:447` CondNotify 每次都取注册表 `guard`（域级 std::mutex）做查找。血源 cond 操作约 470 次/s，目前不构成热点；对 cond 密集的游戏会。
- 没有 host 端自旋。桌面 `threads/mutex.cpp:15,200-218` adaptive 类型自旋 2000 次再 yield；这里 adaptive（type 4）与 normal 同样直接 park。是否该加有限自旋没有数据：需要 `Mutex.Lock.ParkCalls/ParkNs` 直方图，若大量 Park 短于 50 µs 才值得考虑，且应放在 guest 快路径而不是 host 慢路径。

### 3.6 POSIX sem / rwlock

已在 §3.3 表中。另注：`guest_semaphore.h:55` Init 在持域 `guard` 时调用 `allocate()`（可触发 VM 映射与 quiesce）；因等待者不持 pin，未发现死锁，但 Init 期间所有 sem_t 操作被挡。`guest_rwlock.h:103-105` Lock 时惰性 Create 也在域锁内写 guest。

### 3.7 HLE 同步统计（`guest_sync_metrics.{h,cpp}`）

审查结论：Session/TLS 生命周期、每物理线程单写者 shard、嵌套 Call 恢复、stop 后在途完成、Session ID 单调避免 TLS 混用、异常/host error/结果码分类、长调用限频，与文档一致；MSVC 45/45 通过，本机 `--overhead` 汇总 33 ns、详细 64–66 ns/次。未发现会导致误报“成功”或漏计的缺陷。需要在解读与后续工作中注意的口径问题：

1. `Call` 在 `FunctionAdapter::Invoke` 的 `call(frame)` 前后取时（`guest_runtime.cpp:286-296`），**不含** veneer/syscall/快照/`HleScope`/registry 与寄存器回写。对 Unlock（body 2–3 µs）这类短操作，guest 可见成本被低估约 1.1–1.7 µs，即 30–40%。
2. `Classify`（`guest_sync_metrics.cpp:47-133`）未覆盖 `sceKernelWaitEqueue`/`sceKernelAddUserEvent` 等 equeue 等待（VideoOut/Gnm flip 等待常用），也未覆盖 barrier；`scePthreadMutexIsOwned` 落入 `Mutex.Config`。血源不受影响，其他游戏会漏。
3. `PhaseMask` 明确了未插桩阶段；kernel sema Wait 锁前的 pin/Query、CondWait 的注册表锁与查找都不在阶段内，对应 `LookupNs`/`GuardNs` 会偏小。
4. `Session::Enable(false)` 不停止 worker jthread（`:218-241`），停止后仍每 500 ms 醒一次直到 Session 析构；成本可忽略，但“关闭后零开销”不严格成立。
5. 汇总读取是 rolling snapshot、非事务一致（已文档化）；区间量必须取同一 Session 两次累计差。

### 3.8 通用快路径的接入点已经存在

`guest_runtime.cpp:1244-1278` `Bind()` 已为 guest libc 做了“把导入直接解析到 guest 代码 VA 而不是 veneer”的路径（`veneers.emplace(symbol.name, provider->virtual_address)`），并校验提供者位于可执行 ELF 段。app 自带的同步 payload 可作为第二类 provider 走同一入口，与 CUSA patch 的 `GuestPatch::Manager` 精确 SHA 绑定完全分离。custom SDK v1（`guest/custom/v1`）与 compiled guest 原型（`tests/guest_cpu/compiled_guest/entry.cpp`，设备 13/0、1 万次无竞争锁 0.40 ms vs 2 万次空 HLE 22–35 ms）证明了机制，但原型只有单个状态字与 wait/wake 两个 host 调用，不是 Orbis ABI。

## 4. 推断（需要数据确认）

1. **handoff 成本分解**：约 185 µs/次 ≈ `SignalSema` HLE 34–70 µs CPU + futex 唤醒与调度（runnable 平均 31 µs）+ 被唤醒者重夺域锁的二次 futex + `WaitSema` 返回与后续取节点/allocator rwlock（约 14 µs）。若 §3.3/3.4 把 host 部分压到 ~20 µs、去掉二次交接，单次 handoff 可能降 30–40%，任务组等待从 ~32 ms 降到 ~20 ms，对应 5–8% 帧时长。前提是 CSEzWork 的任务粒度不变。
2. **快路径收益上限**：Guest-1 lane 上非等待同步 HLE 10% + 跨界 3–4%，快路径处理无竞争多数（Lock 3.7 µs / Unlock 2.1 µs 均值说明多数不 Park），收益上限 12–14%；worker 线程收益不进关键路径。合并 #1 后 01 组帧长可能从 ~130 ms 降到 ~105 ms，仍高于 GPU 下限 75 ms，因此不会立刻撞 GPU。
3. **JIT 主体**：Guest-1 lane 54.6%（01）/45.7%（03）是未插桩 guest JIT 代码，是比同步更大的单项，属于 FEX 代码质量/热点函数范畴，不在本报告范围。
4. **spin 是否有益**：无法离线判断。血源 guest 自身自旋 0.16 ms（TSC 后）仍有 54% 进入等待，说明持锁段 + handoff 链超过预算；host 侧加自旋在直接交接协议下对该锁无效，对普通 pthread mutex 是否有效要看 Park 时长分布。
5. **`pthread_mutex_lock` 别名慢**：见 §2.3。
6. **GPU 单批 70–105 ms**：批次执行时长与 GPU busy 三口径一致，判断为真实 GPU 工作而非 fence 报告延迟；具体是哪部分 shader/带宽需要 RenderDoc/counter，另议。

## 5. 随应用发布的 HLE FEX 通用快路径：评估与分层建议

### 5.1 目标形态

```text
无竞争：guest x86 → 原子 CAS/acquire 于 guest 内存 → 临界区 → release → 返回，0 次 HLE
有竞争：guest 置“有 waiter”位 → HLE shad_sync_wait(addr, expected, timeout) → host 停车（可取消）
解锁：  guest release；仅 waiter 位被置时 HLE shad_sync_wake(addr, n)
```

这与 Wine `RtlWaitOnAddress`/关键区、Bionic 的 mutex/cond 完全同型。**host 只需要两个通用原语**，全部 Orbis 语义（类型、递归、errorcheck、errno/sce 返回、静态初始化、cond 序号协议、rwlock 计数）都在 app 自带的 guest 代码里，跨游戏共用，不进 CUSA patch。

### 5.2 必须一次性统一的状态与协议

1. **唯一权威状态在 guest**：沿用现有 ABI 前缀布局 owner@+0、depth@+8（`Prefix`，`guest_mutex.h:42-48`），libc 可写的 +0x20 flags 保留。host 不再保存 `Mutex::owner/depth` 副本；host 慢路径只管理等待队列与取消。禁止 CAS 与 host owner 同时判定成功。
2. **owner 身份**：guest 侧从 `fs:[0x10]`（`Tcb::tcb_thread`，`guest_runtime.cpp:928-932`）取 `handle_va`，与当前 HLE `Current()->handle_va` 同值；errno 地址为 `handle_va + 8`（`ErrnoAddress`，`:1527-1529`）。这两项使 `scePthreadSelf`、`pthread_self`、POSIX 失败写 errno 都可在 guest 完成。
3. **两套入口**：`scePthread*` 返回 `0x8002xxxx`，`pthread_*` 返回 -1 并写 errno；同一 guest 实现两层壳。
4. **静态初始化**：slot 值 0/1 表示需初始化（`FindMutex`，`:171-186`）；guest 侧检测到后走一次 HLE 初始化（分配 arena 对象、登记），之后不再进 host。
5. **cond**：Bionic 式 `seq` 字 + waiter 计数；wait = 记 seq → guest 解锁 → `shad_sync_wait(&seq, seq)` → 重锁；signal/broadcast = `seq++` → 若有 waiter → `shad_sync_wake`。`scePthreadCondSignalto`（指定 owner）保留 HLE 慢路径。递归深度在 guest 保存与恢复。
6. **host 原语语义**：`wait` 在按 guest VA 哈希的桶锁内入队、再读 expected（用短 pin 读 host 别名，不跨停车持 pin）、释放桶锁后停在每线程自己的 parker 上；`wake` 桶锁内选取 n 个、置位、出桶、释放桶锁后唤醒。取消用现有 per-thread `hle_cancel`（`fex_context.cpp:1478`）；映射退役/unmap 时对范围内的 parker 以 EFAULT 唤醒（可挂在 `PrepareMapping` 的 drain 上）。直接 futex on host 别名也可行，但取消与退役仍需要 parker 表，所以建议自建表。
7. **kernel semaphore / event flag**（二期）：需要 id→guest 对象表；无竞争 `WaitSema` 可 CAS 扣减，但只要 `waiters>0` 或值不足就走 HLE 以保持 FIFO/优先级；因此对 §2.2 的 convoy 链**快路径本身无效**，那条链只能靠 §3.3/3.4 的 host 侧改进。
8. **调试/统计影响**：guest 快路径后 `hle_sync` 看不到无竞争操作；`HLE.Sync.Mutex.Lock.Calls` 会骤降而不是“变快”。需要 guest 侧采样计数（custom SDK `shad_sdk_counter`）或明确接受盲区；安全点快照/guest 调试器对 guest 锁状态的解释也需更新（owner 在 guest 内存而不是 host map）。

### 5.3 分层与顺序

| 层 | 内容 | 风险 | 依赖 |
|---|---|---|---|
| A：host 低风险修复 | notify 移出锁；kernel sema per-waiter 停车；`Current()` TLS 缓存；去掉 `Trace()` 原子；rwlock 惊群与 POSIX sem 每对象锁 | 低，语义不变 | 现有测试 + 新增 ctx-switch 计数反例 |
| B：app 自带 guest 同步 payload | pthread/scePthread mutex、cond、rwlock、sem_t、`Self`；host 新增 wait/wake-on-address | 中，ABA/生命周期/退役协议 | §5.2 全部；`Bind()` provider 接入 |
| C：kernel sema / eventflag 快路径 | 无竞争路径入 guest；竞争仍 HLE | 中 | B 的原语；id→对象表 |

同样机制可顺带覆盖非同步高频 HLE：`sceGnmIsUserPaEnabled`（7.2k 次/s，常量返回）、`sceKernelReadTsc`（RDTSC 已在 FEX 内缩放，可 guest 内联）；`sceGnmSet*Shader` 7.2k 次/s 属 PM4 写入，理论上可 guest 化但是更大的工程，此处只登记。

## 6. 验证缺口与最小验证方法

| 缺口 | 最小验证 |
|---|---|
| 当前代码（定向唤醒 + 隔离后）在血源的真实 Park/Guard/Lookup/Publish 分布 | 设备进入诊所后 `hle_sync start CONTEXT`，两次 `status` 取累计差；再短时 `detail`，读 `Mutex.Lock.ParkCalls/Calls`、`ParkNs`、`GuardNs`、`LookupNs`、`PublishNs`，`Sema.Wait/Signal.ParkNs/GuardNs`，`PosixSem.*`、`Rwlock.*` 是否有量。这直接回答“快路径能吃掉多少”“要不要自旋”“别名锁为何慢” |
| 统计口径校准 | 同一窗口比较 `HLE.Sync.Mutex.Unlock.ElapsedNs/Completed`、Litep `HLE.scePthreadMutexUnlock` scope 均值、custom SDK `TimedScope` 包住 lock/unlock 对的 guest RDTSC 时长：三层差即跨界成本 |
| 工具扰动 | 同场景 OFF/ON(汇总)/ON(detail)/OFF 的 present 间隔与 `hle_sync` 自身 counter |
| A 层改动 | 复用 `guest_sync_performance --assert-isolation` 思路：对 kernel sema 与 mutex 增加“一次 Signal/Unlock 后 waiter 的 `ru_nvcsw` 增量 ≤ 1”反例；TSan 64 轮取消竞态保留；`guest_kernel_semaphore_tests` 588 项回归 |
| A 层收益 | 同视角 A/B/A：present 间隔、`Sema.Signal.ElapsedNs` 均值、worker runnable、任务组等待（现有 242 探针 + `long-task-waits`）、KGSL busy |
| B 层原语 | compiled guest 原型扩展：两 owner 争抢 + Stop 取消 + unmap 后 EFAULT 唤醒 + 期望值改变时不停车；再接 pthread 语义测试（`guest_native_mutex_tests`/`guest_condition_tests` 改为对 guest payload 施测） |
| 本机未跑 | Linux/Android 域 bench 与隔离测试（需 Linux 或设备 + NDK r29）；PROF 重索引（需对应 Litep） |

## 7. 证据

- [evidence/orbis-sync-offline-review-20260920/](evidence/orbis-sync-offline-review-20260920/)：`kgsl_busy.py` 与三组输出、MSVC `sync_metrics` 单测与 overhead 输出、trace ZIP SHA-256。
- 数据包内引用：`samples/*/analysis/{cpu-gpu,frames,job-summary,long-task-waits,spin-budget-evidence}.json`、`samples/03-*/analysis/summary/scope-summary.json`、`samples/*/kgsl/kgsl_trace.txt.gz`。
- 上游报告：[HLE 量化与对象级唤醒](orbis-sync-metrics-20260920.md)、[TSC 与 semaphore 定向唤醒](bloodborne-tsc-20260920.md)、[队列自旋定位](bloodborne-job-spin-20260920.md)、[锁拆分后等待链](buffer-upload-litep-20260920.md)、[native mutex / guest 快路径审计](native-mutex-gil-audit-2026-09-17.md)、[compiled guest 机制](../../guest-compiled-functions.md)。

本报告未修改生产代码、FEX/Foundation 子仓或任何 trace 文件；没有 FPS 改善结论。
