# Orbis 同步 Tier B：随应用发布的 guest mutex 快路径与实机 A/B/A

日期：2026-09-20。分支：`feature/malos/fex_sync_performance`（自 `ed7fefd5`）。承接 [离线评估 §5](orbis-sync-offline-review-20260920.md) 与 [Tier A](fex-sync-tier-a-20260920.md)。设备 AYN Thor `9c2841a4`（Android 13 / API 33 / 4 KiB / Turnip `5ac41be677`），Bloodborne CUSA03023 诊所场景，Internal Scale 0.5、High 1×1、FDM OFF，profiler ring ON。

## 结论

1. 无竞争的 `pthread_mutex_lock/unlock/trylock`、`scePthreadMutex*` 与 `pthread_self/scePthreadSelf` 现在完全在 guest x86 内完成，只有竞争路径才进 host。同一 APK 用属性开关做 A/B/A（同进程、同视角）：同步类 HLE **104.6k → 12.1k 次/s（−88%）**，诊所 FPS **7.4 → 10.7（+45%）**，Guest-1 每 12 s 唤醒 89.1k → 29.3k、"醒后 20 µs 内再阻塞" 26.5% → 13.8%。第三段再次关闭回到 7.27 / 7.17 / 7.15，HLE 102.5k 次/s。
2. 这不是把 host 锁做得更快，而是删掉了每次 HLE 固定的 7 次地址空间锁 + 3 次 context 锁 + 完整寄存器快照（Tier A 已定位的真正串行点）。
3. 原型依据全部来自现有代码：算法是桌面 `mutex.cpp`（FreeBSD libthr）；锁字是 `tests/guest_cpu/compiled_guest` 已在真机验证的 0/1/2 三态；host 原语是 `RtlWaitOnAddress` 形态的期望值等待/按地址唤醒；发布走 `Bind()` 已有的"导入解析到 guest VA"路径。没有新协议。
4. 未做：timed lock、cond、rwlock、sem_t、kernel semaphore 的 guest 化（Sema.Wait 84% 是真等待，快路径对 §2.2 的 convoy 链无效）；完整游戏回归；其它游戏验证。

## 实现

| 部分 | 文件 | 说明 |
|---|---|---|
| 共享 ABI | `src/core/host_runtime/guest_sync_abi.h` | prefix 布局（owner@0、count@8、**state@0x18**、flags@0x20 不变）、三态常量、类型、arena 窗口 `[0x1000000000, 0x1010000000)`、5 个导入槽顺序、导出名、合成 NID `shadSyncWait/Wake`。 |
| guest payload | `guest/runtime/sync/mutex.c` | freestanding x86-64 C：`LockCommon`（CAS 0→1；owner==self 时按 flags 类型递归/EBUSY/EDEADLK/Normal 死锁，顺序与 host 一致；`spins` 自旋；`xchg→2` 后 `wait(&state,2,4)`）、`UnlockCommon`（count>1 递减；否则 owner=0、`xchg→0`，旧值 2 才 `wake(&state,1)`）。slot 值不在 arena 窗口内（0/1 静态初始化、2 已销毁、其它）→ 走 POSIX HLE 回退导入。`self` = `fs:[0x10]`。 |
| host 原语 | `src/core/host_runtime/guest_sync_waiters.h` | `GuestAddressWaiters::Wait(addr, expected, width, cancel)` / `Wake(addr, n)`：256 桶按 VA 哈希；桶锁内短 pin 原子读期望值并入队，出桶后停在每 waiter 自己的 mutex/CV；取消走 stop_token；Wake 桶锁内选取、出桶后 notify。不跨停车持 pin/地址空间锁。 |
| host HLE 同协议 | `src/core/host_runtime/guest_mutex.h` | `Mutex` 不再保存 owner/depth 副本，只保留 address/type/protocol/retired/cond_waiters；`Lock/Unlock/CondWait/Destroy/IsOwned/PendingWaits` 全部通过短 `AcquireDataSpan` 读写 guest prefix 的同一锁字（`Open()`）。CondWait 的 enqueue+guest unlock 在 cond guard 下原子；重锁走 `AcquireWord`（同一 waiters 队列，与 guest 竞争者公平排队）。Destroy 用 `retiring` 标志与刚赢得锁字的获取者握手，避免"退役后仍持有"。 |
| 发布 | `guest_runtime.cpp` `InstallSyncFastPath()` | 首次 `Bind()` 时（任何 guest 线程前）：解析 5 个导入槽（wait/wake 合成 NID + 3 个 POSIX HLE veneer）→ `CodePublication` 下拷贝 image 到 `Allocate(...,"GuestSyncFastPath",0x1800000000)` 的新映射、填槽、`Protect RX` → 8 个游戏可见符号（libScePosix + libkernel 两种后缀）改指向 payload 导出，`hle_status=guest_fastpath`。`debug.shadps4.sync_fastpath=0` 关闭；`hle_sync status` JSON 新增 `"fast_path"`。 |
| 构建 | `scripts/android/build-guest-payload`（`--entry/--namespace/--header/--symbols`）、`cmake/guest-code/payload.ld`（`.shad_imports` 段）、根 `CMakeLists.txt`（host DSO 内嵌 `guest_sync_payload.h`）、`cmake/fex/CMakeLists.txt`（测试 payload + driver） | payload 由 NDK clang x86 后端编译，1112 字节，image sha256 `a2c3fc15…`；只有 PC 相对重定位、单 RX 段。 |
| 统计 | `guest_sync_metrics.{h,cpp}` | 新 `Sync.AddrWait`（Park 相位）/`Sync.AddrWake` 操作；**本次实测 APK 不含此分类**，所以 ON 侧看不到竞争路径的行。 |

明确保留的边界：POSIX sem_t、rwlock、kernel semaphore、cond、timed lock 全部仍是 HLE；`Mutex.Lock/Unlock` 无竞争调用从此不再出现在 `hle_sync`；arena 对象到 Session teardown 才释放，停车中退役映射不支持（与之前一致）。

### 放弃/修正的中间方案

- 直接把 `WriteOwnership` 改为绕过 VM 路径的原子写（Tier A 已回退）：本轮通过 `AcquireDataSpan` 短 pin 满足 "range retirement blocks one object" 契约，同时给 guest 快路径留出同一锁字。
- 第一版 B 测量只按了一次 Cross，停在标题/上线提示（30 FPS），作为功能性证据保留在 `tierB-on-menu-only/`（fast_path installed，菜单正常），不用于 FPS 结论。

## 设备单测（NDK r29，V0 测试树，同一 DSO 源码）

| 套件 | 结果 |
|---|---|
| `guest_sync_fastpath_tests`（新，真实 FEX 执行生产 payload + 生产 `GuestMutexDomain`） | 29/0：`pthread_self` 0 HLE；静态初始化恰好 1 次 HLE 建对象；10000 对 lock/unlock **0 次 HLE**（1.25 ms）；sce 入口同对象；guest 持锁 host `IsOwned`/trylock EBUSY/非 owner EPERM；host 停车 → guest unlock 经 wake 导入交接；guest 停车 → host unlock 交接；4 个真实 FEX owner 100000 次受保护递增、锁字归零、只用了 wait/wake；递归 attr 二次加锁在 guest；errorcheck 自锁 EDEADLK 无 HLE；sce trylock 返回 `0x80020010`；已销毁 slot 回退 HLE EINVAL；停车中取消 → `Cancelled`，host 仍持有并可释放，随后正常使用/销毁。 |
| `guest_native_mutex_tests` | 65499/0（含 range retirement、cancel/unlock 64 轮、retirement race 64 轮、concurrent lookup lifetime） |
| `guest_condition_tests` / `guest_services_tests` / `guest_rwlock_tests` | 67/0、78/0、60/0 |
| `guest_kernel_semaphore_tests` / cancel relay | 588/0、453/0（64 轮） |
| `compiled_guest_tests`（旧原型） | 13/0 |

## 实机 A/B/A（APK c0a7cd36 / host 77d0e9f8 / JNI 832732f3，签名与交接证书一致）

同一 APK、同一进程（PID 9558）、同一诊所位置（`scene-after.png`：A/B 视角一致，A′ 角色姿态略有不同，仍是同一房间同一朝向），只切换 `debug.shadps4.sync_fastpath`；每段：启动 → 标题 → 三次 Cross（确认上次未退出对话框 / 离线游玩 / 继续）→ 诊所加载后再等 60 s → 预热 ≥250 presents → 3×10 s FPS（metrics OFF）→ 20 s `hle_sync` 汇总 → 10 s detail → 12 s root sched 采集 → 正常 STOP。

| 指标 | A：OFF（gen1） | B：ON（gen3） | A′：OFF（gen4） |
|---|---:|---:|---:|
| FPS 3×10 s | 7.47 / 7.33 / 7.35 | **10.91 / 10.75 / 10.41** | 7.27 / 7.17 / 7.15 |
| 汇总窗 presents / 20 s | 146 | 209 | 144 |
| 同步类 HLE 次/s | 104,568 | **12,100** | 102,533 |
| Mutex.Lock / Unlock 次/s | 46,486 / 46,902 | 0 / 0（guest 内） | 45,579 / 46,006 |
| Sema.Wait 次/s / park% / park µs | 2,150 / 82.8 / 5,229 | 1,550 / 76.2 / 6,324 | 2,098 / 83.8 / 5,061 |
| Cond.Wait 次/s / park ms | 408 / 44.8 | 561 / 34.0 | 398 / 45.4 |
| Guest-1 wakeups / 12 s，短 reblock | 89,108，26.5% | **29,303，13.8%** | 79,914，33.4% |
| Guest-20…24 wakeups，短 reblock | ~41k，21% | ~23k，8.5% | ~34k，24% |
| 全部 guest+GPU 线程 wakeups，短 reblock | 415,452，22.8% | 206,026，13.4% | 357,545，24.7% |
| Guest-1 on-CPU / 12 s | 7.26 s | 8.24 s | 6.25 s |
| sched overrun | 0 | 0 | 0 |

口径：FPS 来自 `host_present` 差分；`hle_sync` elapsed 含等待非 CPU；Sema/Cond 的 park 时长变化含帧节奏变化，不单独归因。B 的 Rwlock.Read guard 升到 6 µs（3.5k/s）是更高帧率下的争用变化，仍远小于被删除的 mutex 开销。

## 证据

[evidence/fex-sync-tier-b-20260920/](evidence/fex-sync-tier-b-20260920/)：三段各自的 `hle_sync` JSON、`fps.txt`、`identity.txt`、`property.txt`、`fast_path.txt`、`warmup.txt`、场景截图、sched 采集元数据/per-CPU 统计/线程名与 `sched-wake-analysis.txt`、原始 trace 的 SHA；`run_condition.sh`、`measure.sh`、`sched-capture.sh`、`compare_sync.py`、`sched_wake_analysis.py`；`manifest.json`。原始 `sched_trace.txt.gz` 留在本机 scratch。

## 下一步

- 把 `Sync.AddrWait/AddrWake` 分类后的 APK 再跑一次，得到竞争路径可见的统计。
- 二期：cond（Bionic seq 协议）、rwlock、sem_t 的 guest 化；kernel semaphore 仅无竞争 `WaitSema` 可 CAS（对 convoy 链无效）。
- 其它标题（TMNT）验证快路径不改变行为；完整游戏回归未做。
