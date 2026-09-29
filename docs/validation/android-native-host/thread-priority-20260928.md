# 宿主线程优先级：guest 调度属性与模拟器线程分级（2026-09-28）

分支 `feature/malos/swan_performance`。
设备：Pico Swan `PB3110PGL6240001G`（`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006`）。
证据：[`evidence/thread-priority-20260928/`](evidence/thread-priority-20260928/)。

## 1. 原状态：两端都不生效

| 路径 | 原行为 |
| --- | --- |
| `Common::SetCurrentThreadPriority`（Linux/macOS/Android） | `pthread_setschedparam(SCHED_OTHER, 非 0 优先级)`：Linux 的 SCHED_OTHER 只接受 0，调用返回 EINVAL，返回值没人检查 |
| `Common::SetCurrentThreadPriority`（Windows） | 实现可用，但全仓**没有任何调用点** |
| 桌面 guest `posix_pthread_setschedparam` / `setprio` | 只写 `attr`，`TODO: _thr_setscheduler` |
| Android `GuestRuntime` 的 `scePthreadSetschedparam`/`SetPrio` 等 | 只写 owner 属性 |
| guest `pthread_create` 带的调度属性 | 两端都存下来，宿主线程不变 |

## 2. 实现

原则：**只用分时调度的权重（nice / Windows 线程优先级级别），不用实时策略**。guest 的 FIFO/RR 不映射为
宿主 SCHED_FIFO：游戏里的自旋等待在 RT 下会饿死其它线程，Android 应用也拿不到 RT。

### 2.1 以进程起始 nice 为基准（游戏内发现后修正）

Swan 实测：优先级关闭时，所有模拟器与 guest 线程的 nice 都是 **-10**，不是 0。Android 把前台应用的主线程
设为 -10，之后创建的线程都继承这个值。第一版按绝对值设 nice（主线程 0、GpuComm -4），在 Android 上反而把
所有线程相对系统进程降了 4–10 级（§4 的 A/B 就是这一版）。

现在 `SetThreadNice(thread, offset)` 的参数是**相对基准的偏移**：

- 基准 `ThreadNiceBase()` 是进程第一次调整时、发起调整的线程的 nice。所有调整都经过 `SetThreadNice`，
  所以那一刻这个线程还没被改过，读到的就是继承值。
- 由已调整线程创建的子线程会继承调整后的值；它的偏移 0 仍回到进程基准，而不是创建者的值。
- 桌面 Linux 从终端启动时基准通常是 0，行为与原设计一致。Windows、macOS 新线程不继承，基准按 0 处理。

### 2.2 通用接口（`src/common/thread.{h,cpp}`）

- Linux/Android：`setpriority(PRIO_PROCESS, tid, base + offset)`，夹在 `max(20 - RLIMIT_NICE, -20)..19`。
  Android 应用的 `RLIMIT_NICE` 是 40，下限 -20（本机普通 app 进程 `/proc/<pid>/limits` 均为 40）。
  下限高于基准时（例如桌面 Linux 默认 `RLIMIT_NICE=0`），线程被调高后回不到基准，于是**整体不调整**。
- Windows：`OpenThread(THREAD_SET_LIMITED_INFORMATION)` + `SetThreadPriority`，偏移 ≤ -6 → HIGHEST，
  ≤ -3 → ABOVE_NORMAL，≥ 3 → BELOW_NORMAL，≥ 10 → LOWEST。不用 TIME_CRITICAL。
- macOS：`pthread_setschedparam(SCHED_OTHER, 31 - offset)`，夹在 15..47。
- `SetCurrentThreadPriority(ThreadPriority)`：Low +4、Normal 0、High -4、VeryHigh -6、Critical -8（偏移）。
- 开关：Android `debug.shadps4.thread_priority=0`，桌面 `SHADPS4_THREAD_PRIORITY=0`，进程启动时读一次。
- 诊断：DebugBus `thread_priority status | reset`。status 输出 `nice_base`、`nice_floor`，以及每个线程的
  名字、来源、偏移、实际设置值、当前 nice 和变更次数（≤1024 个线程）。前 128 次变更写入 `Common` 日志。

### 2.3 guest 调度属性 → 偏移（`core/libraries/kernel/threads/host_priority.h`）

Orbis 数值越小越优先：

| guest 策略 | 优先级 | 偏移 |
| --- | --- | --- |
| FIFO/RR（1/3） | 256–447 | -4 |
| | 448–639 | -2 |
| | 640–703（默认 700 所在段） | 0 |
| | 704–767 | +2 |
| OTHER（2） | 768–863 | +2 |
| | 864–959 | +4 |
| 其它/越界 | | 0 |

- Android（`guest_runtime.cpp`）：owner 在 `Attach` 时记下宿主 tid 并应用创建时的属性；`set_scheduling` /
  `set_priority` 在 `threads_mutex` 下更新属性后立即应用。已结束的 owner 不会被这些查找命中。
- 桌面（`pthread.cpp`）：`RunThread` 初始化后在线程锁下记下 `host_ref` 并应用；`setschedparam` / `setprio`
  更新后应用；`ExitThread` 置 Dead 时清掉 `host_ref`，之后的修改不会落到被复用的 tid（macOS 上是悬空
  `pthread_t`）。

### 2.4 模拟器线程分级

- High：`GpuCommandProcessor`、`VkRecord`、`VkSubmit`、`GpuDone`、`PresentThread`、`shad:AudioOut`。
- Low：`PipelineCompile`、`PipelinePreload`、`PipelineCacheSave`、`PipelineCacheIO`。

## 3. 单测

`thread_priority_tests`（`tests/host_runtime/thread_priority_tests.cpp`）覆盖：

- 映射表；
- 当前线程与其它线程的偏移；
- 夹到下限；
- 已调整线程创建的子线程回到进程基准；
- High/Low 分级；
- status/reset 文本；
- 下限高于基准、开关关闭两条分支。

Swan（root shell；host `libshadps4_host.so` Build ID `83324d0be4c7265335453510addf03749f4a6b07`）：

| 情况 | 结果 | 文件 |
| --- | --- | --- |
| 基准 0，`RLIMIT_NICE` 40 | 23/0 | `swan-base0.txt` |
| `nice -n -10`（模拟前台 app 的基准） | 23/0 | `swan-base-minus10.txt` |
| `ulimit -e 0`（下限 20 高于基准） | 12/0，不做调整 | `swan-floor-above-base.txt` |
| `debug.shadps4.thread_priority=0` | 12/0，不做调整 | `swan-disabled2.txt` |

`swan-normal/floor-positive/disabled.txt` 是修正前的绝对值版本（19/0、11/0、11/0），作为历史保留。
属性测后已清空，设备临时目录已删除。

Windows：本机没有桌面构建目录，只用 NDK clang（`--target=x86_64-pc-windows-msvc` + VS2022 头文件）对
`thread.cpp`、`pthread.cpp`、`diagnostics_commands.cpp` 做了语法检查。新代码没有错误，剩下的是既有的
`ntapi.h` `_QUEUE_USER_APC_FLAGS` 与新 SDK 的重定义冲突，以及它连带的错误。未链接、未运行。
macOS 分支未编译。

## 4. 游戏内（血源，Swan，修正前的绝对值版本）

APK `20587c51`（驱动 86ca）。脚本 `tools/ab_tp.sh` 每轮冷启动进存档点，测出生视角，再转向诊所内部测一次，
每个窗口 10–20 s；`sample_tp.sh` 按 `/proc/tid/schedstat` 统计每帧 on-CPU 与排队等待。

**优先级在普通 app 进程里确实生效**（`nice_floor=-20`，status 里的 `now` 与设置值一致）。血源的分布：

- Guest-7…14、16…19 设成 FIFO 256–260（偏移 -4）；
- Guest-15 设成 765（+2）；
- 主线程 Guest-1 和工作线程 Guest-20…41 保持默认 700（0）。

A/B 结果（30 FPS 封顶，所以只看 CPU）：

| 窗口 | FPS | GPU busy | 总 on-CPU / 排队（ms/帧） | Guest-1 nice, on-CPU / 排队 | GpuComm nice, on-CPU / 排队 |
| --- | --- | --- | --- | --- | --- |
| on1 出生 | 29.95 | 84% | 73.5 / 19.7 | 0, 27.3 / 2.7 | -4, 13.6 / 0.7 |
| off1 出生 | 30.00 | 85% | 73.6 / 19.5 | -10, 27.2 / 2.6 | -10, 13.5 / 0.8 |
| on2 内部 | 30.04 | 93% | 72.6 / 36.1 | 0, 24.2 / 4.2 | -4, 14.7 / 1.3 |
| on3 内部 | 30.02 | 91% | 71.3 / 28.3 | 0, 24.3 / 3.8 | -4, 14.2 / 1.2 |
| off4 内部 | 30.04 | 92% | 75.1 / 28.0 | -10, 25.3 / 4.3 | -10, 14.0 / 1.5 |

开和关之间的差别小于同一模式不同会话间的波动，**没有可测收益**；封顶下本来也不期望 FPS 变化。

**修正后的相对基准版本没有做游戏内 A/B**：那一轮与驱动验证混在一起，被启动脚本打断（见
[`turnip-alloc64` 证据](evidence/turnip-alloc64-20260928/README.md)）。

### 4.1 同期 GPU 故障（与本改动无关）

优先级关闭（等同旧行为）的 4 轮里，有 3 轮出现已知的 `CP (DDE BR) opcode error opcode=0` → device lost，
快照为 devcd5/6/7；绝对值版本开启的 3 轮都没有出现。关闭即旧代码，所以这不是本改动引入的故障。
这个故障对时序敏感，样本太小，不能据此说调低线程优先级能规避它。

## 5. 未做 / 限制

- 修正后版本的游戏内优先级分布与 A/B；不宣称任何性能收益。
- 单测跑在 root shell 里；普通 app 能否降 nice 依据的是 `RLIMIT_NICE=40`，以及 §4 status 的 `now` 列。
- 不涉及 cpuset/uclamp/CPU 亲和性。
- `pthread.h` 中 `ORBIS_KERNEL_PRIO_FIFO_LOWEST/HIGHEST` 的命名与数值方向相反（只是命名问题，未改）。
