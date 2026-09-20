# Orbis 同步 Tier A：host 侧唤醒/锁粒度修复与实机 A/B

日期：2026-09-20。分支：`feature/malos/fex_sync_performance`（自 `1db95bff`）。承接 [离线评估](orbis-sync-offline-review-20260920.md)。设备 AYN Thor `9c2841a4`（Android 13 / API 33 / 4 KiB / Turnip `5ac41be677`），Bloodborne CUSA03023 诊所场景，Internal Scale 0.5、High 1×1、FDM OFF，profiler ring ON（与基线一致）。

## 结论

1. Tier A 的 host 修改按设计生效：域锁/对象锁的 `Guard` 阶段从 0.7–5.2 µs 降到 0.2–0.4 µs，worker 线程"唤醒后 20 µs 内再次阻塞"的比例从 27–28% 降到 25–26%。**但帧率没有变化**（8.7–8.8 FPS，基线口径约 9.2，见下文口径说明），Sema.Wait / Cond.Wait 的 park 时长也没有变。
2. simpleperf 定位到真正的串行点：**`GuestAddressSpace::lock`**（每次同步 HLE 摸 7 次：slot 读 2、owner/depth 写 2、执行 lease 释放/获取 2、RIP 可执行 `Query` 1）加 FEX context `lock_`（3 次）。进程 30% CPU 在内核 futex 唤醒路径，libc 里 `__aarch64_cas2_acq`+`pthread_mutex_lock/unlock` 5.5%，调用者几乎全是 `ReadData`/`WriteData`/`ExecutionLease`/`FexCpuContext::Run`。
3. 因此 host 侧再优化的上限很低（A2 只减少了 2 次锁往返/HLE 和无谓的 `notify_all`）。真正的收益必须来自**减少 HLE 次数**，即随应用发布的 guest 快路径（Tier B），预期把 Guest-1 每帧约 3.6k 次同步 HLE 中的无竞争多数留在 guest 内。

## 基线（APK 3ba6e584 / host 914dd36f）

首次拿到当前代码的 `hle_sync` 实景数据（20.2 s 汇总 + 10.4 s 详细）：

| 操作 | 次/s | 均值 µs | Lookup | Guard | Park% | Park µs | Publish |
|---|---:|---:|---:|---:|---:|---:|---:|
| Mutex.Lock | 51,022 | 11.5 | 2.61 | 0.72 | 5.6 | 114 | 2.15 |
| Mutex.Unlock | 51,555 | 4.1 | 1.90 | 0.12 | – | – | 1.61 |
| Sema.Wait | 2,696 | 3,402 | – | 5.21 | 84.6 | 4,168 | – |
| Sema.Signal | 2,280 | 10.4 | – | 0.85 | – | – | – |
| Cond.Wait | 503 | 41,026 | – | – | 98.7 | 42,463 | reacquire 27 |
| Rwlock.Read | 3,048 | 3.7 | – | 1.12 | – | – | – |

同步类 HLE 合计 **11.6 万次/s**。root sched 采集 12 s：Guest-1 每秒 8.8k 次唤醒，其中 31.4% 醒来后 20 µs 内再次阻塞；唤醒者是其他 guest 线程（Guest-19/20–24/4），即 host 锁 convoy，而不是 guest 级 park（整个进程 guest park 约 6k/s）。

## Tier A 修改（host，契约不变）

| 文件 | 修改 |
|---|---|
| `guest_kernel_semaphore.h` | waiter 停在自己的 mutex/CV；Signal/Cancel/Delete 释放域锁后再 notify（`DeferredWakes`）；`Trace()` 超过 256 条后不再做原子 RMW |
| `guest_mutex.h` | Unlock、CondWait 内的竞争者唤醒、CondNotify 均在释放对象 guard 后 notify；Cond waiter 改 `shared_ptr` |
| `guest_semaphore.h` / `guest_rwlock.h` | 对象改 `shared_ptr`，Post/Unlock 在域锁外 notify；rwlock 读者退出不再广播 |
| `guest_runtime.cpp` `Current()` | thread_local `weak_ptr` 缓存，热路径不取 `threads_mutex` |
| `address_space.cpp` `WriteData` | lease 释放与 observer 快照合并到一次持锁（3→2 次） |

放弃的方案：owner/depth 直接原子写 sync arena（绕过 VM 写路径）。它让 `guest_native_mutex_tests` 的 "range retirement blocks one object" 失败，即违反"退役中的映射必须挡住发布"契约；已回退。

### Tier A2（APK e5d7bce9 / host 37fc2f53）

- `fex_context.cpp` resume：生产配置 `resume_internal_drains=false` 时跳过独立的取消检查锁段，由 admission 块内的 `pending` 检查覆盖（3→2 次 `lock_`）。
- `address_space`：`leases_idle` 增加等待者计数，`ReleasePin`/`ReleaseExecutionLease`/`WriteData` 只在有 drain 等待时 `notify_all`（避免 `condition_variable_any` 内部互斥锁）。

### 设备单测（NDK r29，A2 后）

kernel semaphore 588/0（无关对象 2 / 未满足 waiter 2 次切换）、rwlock 60/0、condition 67/0、native mutex 65499/0、cancel relay 64 轮 452/0、`--assert-isolation` sem/rwlock 各 1 次切换、guest_cpu_contract 253/0、guest_execution 全 PASS、hle_abi 全 PASS、session_lifecycle 840/0、compiled_guest 13/0、veneer 19/0、hle_registration 13/0。

## Tier A 实机结果（APK ececa16c / host c6d2d129，签名与交接证书一致）

| 指标 | 基线 | Tier A |
|---|---:|---:|
| Mutex.Lock Guard µs | 0.72 | **0.37** |
| Sema.Wait Guard µs | 5.21 | **0.36** |
| Sema.Signal Guard µs | 0.85 | 0.23 |
| Mutex.Lock Park% / µs | 5.6 / 114 | 5.1 / 120 |
| Sema.Wait park µs | 4,168 | 4,320 |
| Cond.Wait park ms | 42.5 | 45.4 |
| Guest-20…24 短 wake-reblock | 27–28% | 25–26% |
| Guest-1 短 wake-reblock | 31.4% | 30.4% |
| FPS | ≈9.2（metrics ON 的 20 s 窗口） | 8.79 / 8.67 / 8.75（metrics OFF 10 s ×3） |

FPS 口径不同：基线只有 metrics ON 的窗口；Tier A 的三个 10 s 窗口为 metrics OFF，随后 metrics ON 的 20 s 窗口 173 presents ≈ 8.5 FPS。两者差在噪声范围内，不构成 Tier A 提升或退化的证据；结论是"没有可见变化"。

### Tier A2 实机结果

同一 Session 内视角与前两次不同（多按了一次 Cross 触发转身，draw/dispatch 11.3k/s vs 6.8k/s，worker on-CPU 3.1 s vs 2.2 s），FPS 6.09 / 6.09 / 6.15 **不可与前两次比较**。与场景无关的逐操作指标与 Tier A 相同：Mutex.Lock 11.5 µs（Lookup 2.75 / Guard 0.37 / Publish 1.93）、Unlock 4.0 µs、Sema.Signal 12.3 µs；Guest-1 短 wake-reblock 28.2%。结论：去掉的那两次 context `lock_` 与无谓 `notify_all` 不是争用点。

## simpleperf（APK ececa16c，8 s，cpu-clock 999 Hz，fp 栈）

| 归属 | 占比 |
|---|---:|
| kernel | 29.8%（其中 63% 记在 `_raw_spin_unlock_irqrestore`，即关中断的 futex 唤醒/调度段；内核栈未展开到用户态） |
| JIT（unknown） | 29.5% |
| libshadps4_host.so | 15.2% |
| libc | 11.7%（`__aarch64_cas2_acq` 2.7%、`pthread_mutex_unlock` 1.7%、`pthread_mutex_lock` 1.2%、`syscall` 0.7%） |

host 自身热点：`ValidateRangeLocked` 7.0%、`FindMutex` 4.9%、profiler `encode_span_begin_dyn/encode_span_end/TlsBuffer::ensure` 合计 **9.5%**（ring ON 的代价，每次 HLE 一个 span）、`MemoryManager::IsValidMapping` 3.7%、`Query` 3.2%。

`pthread_mutex_lock` / CAS 自旋的调用者（callee 图）：`GuestMutexDomain::Write<12 bytes>`（owner/depth 发布）、`GuestAddressSpace::ReadData`（slot 读）、`ExecutionLease::operator=`、`FexCpuContext::Run`/`InvokeGuest`。全部指向地址空间锁与 FEX context 锁。

## 下一步（Tier B，随应用发布的 guest 快路径）

设计已在 [离线评估 §5](orbis-sync-offline-review-20260920.md) 给出；本轮证据进一步说明：只要 Mutex.Lock/Unlock 仍走 HLE，无论 host 内部怎么改，每次都要付 7 次地址空间锁 + 3 次 context 锁 + 完整快照。接入点：`guest_runtime.cpp` `Bind()` 已有把导入解析到 guest 代码 VA 的 provider 路径；guest 侧从 `fs:[0x10]` 取 owner handle、`handle_va+8` 写 errno；host 只需新增 wait/wake-on-address 两个原语。竞争路径（Sema.Wait 84% park、Cond.Wait 99% park）仍走 HLE，本轮 Tier A 修改在那条路径上继续有效。

另一个零风险项：普通运行时关闭 profiler ring（当前默认 ON），可回收约 1–2% 帧时长；不改代码，属于运行配置。

## 证据

[evidence/fex-sync-tier-a-20260920/](evidence/fex-sync-tier-a-20260920/)：基线与 Tier A 的 `hle_sync` JSON、FPS 窗口、sched 采集元数据与原始 trace SHA、`sched_wake_analysis.py` 输出、simpleperf 报告文本与原始数据 SHA、`compare_sync.py`/`measure.sh`/`sched-capture.sh`。原始 `sched_trace.txt.gz`（38 MB）与 `tierA-perf.data`（4.9 MB）留在本机 scratch，不入库。

Windows 主机构建修复（同分支）：`build-host-android`/`generate-guest-fixtures` 的 `.exe` 后缀与 SDK 路径；ffmpeg Android adapter 在 Windows 下用 `sh` 跑 configure、`--tempprefix`、`--host-cc/--host-cflags`、MSYS 路径改写、`library.mak` 的 `AR_RSP` 响应文件归档、只安装归档并从源码树取头文件；`prepare-bionic-turnip` 先关闭临时文件再 replace；app Gradle 增加可选 `signing.debug*` 覆盖。host tools 需要 `/Od` 与 `protobuf_MSVC_STATIC_RUNTIME=OFF`（MSVC 19.44 在 protobuf 上 ICE），未改仓库脚本，见本机 `build/run-host-tools-config.cmd`。
