# 血源队列等待优化：统一 guest TSC 与 semaphore 定向唤醒

2026-09-20，`feature/malos/hle_vr`；承接[队列锁定位](bloodborne-job-spin-20260920.md)。本轮目标是缩短已证实的固定 tick 自旋，保留锁、semaphore、任务所有权协议。

## 原因与修改

血源 CSEzWork 的 DLAdaptiveMutex 使用固定 200,000 TSC ticks 自旋预算。AYN 的原始计数器为 19.2 MHz，旧版把指令和 HLE 都映射到此域，导致预算约 10.417 ms；前一轮实测 551 次进入 WaitSema 前的中位间隔 10.428 ms。

启用 FEX 已有 SMALLTSCSCALE：对低频 host counter 使用最小的二次幂倍数，直到 guest 频率至少 1 GHz。AYN 为 64 倍，即 **1.2288 GHz**，相同预算约 **0.16276 ms**。这不是精确模拟某个 PS4 CPU 的固定频率，也没有提高硬件计数器本身的分辨率。

同时缩放全部有关 guest 接口：RDTSC、RDTSCP、CPUID.15H，sceKernelReadTsc/GetTscFrequency，GetProcessTimeCounter/CounterFrequency，以及 VideoOut TSC 回调。公共 kernel 辅助入口在 Android ARM64 使用相同换算，GPU perf counter 的分子和分母同步缩放，转换后的 GPU ticks 不变。

ARM64 的原始频率直接读取 CNTFRQ_EL0，与 FEX 使用同一来源；不再用 100 ms 采样后按 100 kHz 取整，避免换算因子或阈值因估计误差不一致。Common::NativeClock 保持原始 host 域。纳秒、微秒、睡眠、调度、音频、profiler 与 KGSL 校准继续用原时钟。不能只开启 FEX 缩放而留下原频率的 HLE；那会重新引入[此前已修复的混域错误](ring-clock-compiled-guest-2026-09-14.md)。本轮没有修改 FEX 子仓、队列节点释放顺序或 guest 二进制。

## Citron 对照

核对本地 Citron `3da08ee52defc5003d23683b0c233b0dac88722f`：

| 源码 | 做法 | 对本轮的意义 |
|---|---|---|
| `src/common/arm64/native_clock.cpp:23`、`:44` | 分别保存时间、guest CNTPCT、GPU tick 的换算因子 | 不把 host 原始 tick 不加区分地交给 guest |
| `src/core/arm/nce/patcher.cpp:429` | CNTPCT 指令读 CNTVCT 后使用 UMULH/MADD 换算 | 指令读数也必须进入 guest 域 |
| `src/core/arm/dynarmic/arm_dynarmic_64.cpp:182`、`:300` | 读数来自 GuestClockTicks，频率来自 guest 硬件常量 | 指令与频率报告应一致 |
| `src/core/hle/kernel/k_light_lock.cpp:56` | 竞争慢路径登记 owner/waiter 后 BeginWait；scheduler lock 只保护元数据 | 这里没有额外固定 200,000 ticks 自旋，但不是血源的同一个锁 |

Switch guest 的 CNTPCT 本来就是 19.2 MHz（`src/common/wall_clock.h:16`），不能把这一数值作为 PS4 的适配结论。Citron 的平台、ABI、游戏及执行后端与当前血源不同，源码对照不能证明它运行相同工作负载会更快。采用的是其时钟域隔离原则，未照搬 Switch 锁协议。

## 针对性验证

- Android 真实 FEX：5/5。RDTSC、RDTSCP 位于真实 GuestClock 调用上下界；CPUID 频率一致；按频率归一化的过程计数器保持约 20 ms 实际休眠时长。
- Android services：78/78。含不同 host 频率、零值及 unsigned 回绕边界；原时钟/服务检查保留。
- 生产 Linker/VM ELF：3 轮均返回 51966。实际绑定 ReadTsc、两种 Frequency、ProcessCounter、ProcessTime，混合执行 CPUID/RDTSC/RDTSCP/20 ms nanosleep；不是只调用一个纯数学 helper。
- host RelWithDebInfo 和普通 playstoreDebug APK 构建通过。

首次误用了旧 build 目录里的辅助 production runner，与当前 DSO 配合在输出前退出 139；随后按当前配置重建 runner，三轮通过。该失败保留，不作为普通 APK 的运行结果。

## TSC 的实景对照与同窗证据

测量均在诊所人物可见、自动输入停止后开始；loading 样本不计入性能结论。配置为 Turnip、Internal Scale 0.5、High 1×1、FDM OFF，普通性能窗口关闭 auto probes、profile_sync、GPU timing 与文件采集，ring 保持 ON。使用 host_present 增量除以 snapshot_ns 时间差，不能把包含 overlay 的全部 presents 当游戏 FPS。

| 版本 / 顺序 | 测量 | 结果 |
|---|---|---|
| A 原版 c14cf608，PID18248/gen3 | 10 s native 采样窗口 | 7.178 FPS |
| B 仅统一 TSC，5e87c9da | 三个连续 10 s 无输入窗口 | 8.237 / 8.333 / 8.350 FPS |
| A 再装原 APK | 三个连续 10 s 无输入窗口 | 7.155 / 7.264 / 7.360 FPS |

B 的合计 8.306 FPS，对 A 复测合计 7.260 FPS，观察到 **14.42%** 提升。两个版本场景、视角保持一致，但没有固定频率/温度控制，只有短时静止场景结果，不外推整游戏。后续 fe73b415 仅补了直接读取 ARM 架构频率及格式整理，在本设备仍是相同 19.2 MHz→1.2288 GHz。

### 有效 Litep / KGSL 窗口（定向唤醒修改之前）

最终有效诊断 B2 使用 APK fe73b415、host45830d0f、JNIbc53f543，PID27754/gen3/context130；PROF `a4882221`，6.270 s、2,504,441事件、246有效chunks/0skipped，49个完整 source 帧；242个相同 exact-build EH 探针。KGSL 14 s/512 MiB 总预算，调度覆盖完整、overrun0/dropped0；58 GPU 对/0 orphan。

第一次诊断 B 的 24 s/384 MiB 调度缓冲区覆盖了2,227,769事件，只剩3.406 s完整交集，保留但不拿它代表整个12秒窗口。B2缩短采集，未靠扩大到无界缓冲解决。PROF仍保留边界未闭合 scope、missing-target flow 警告，不宣称所有事件无损。

同 PID 多次 session 后，文件还含2,646条 context1 counter；按完整 profile/context 身份排除，仅配对 context130。它们属于旧身份标签，未证明是存活的旧 guest 线程或泄漏；需要后续单独审计动态 counter 名称生命周期。本轮不把这些记录混入新队列统计。另对新进程首个session的诊断B进行独立PROF配对，不使用其缺失的调度区间：13,975次间隔中位0.166349 ms，与B2吻合，因而缩短自旋的结论不依赖后续session身份过滤。

| 指标 | 原版上一轮 | TSC 修复后的 B2 |
|---|---:|---:|
| acquire→WaitSema 中位间隔 | 10.428 ms（551次） | **0.166501 ms（6,828次）** |
| 该间隔最短 / P90 | 10.421 / 10.502 ms | 0.165304 / 0.170773 ms |
| acquire 跨worker CPU合计 / 次数 | 9,706.646 ms / 15,973 | 1,558.837 ms / 12,600 |
| acquire 每次平均 CPU | 0.608 ms | **0.124 ms** |
| 持锁释放节点 elapsed / 次数 | 1,762.716 ms / 12,499 | 89.557 ms / 10,109 |
| 任务 callback CPU / 次数 | 1,044.599 ms / 12,499 | 531.311 ms / 10,109 |

6,825/6,828次间隔低于1 ms，全部低于10 ms。独立原始计数器锚点仍测得约19.2 MHz，换算后的200000tick预算为0.162760 ms；采样间隔包含调用/探针/调度开销，不能等同纯自旋 CPU。

跨 worker CPU 合计不是帧墙钟时间。两轮诊断长度不同（12.222 s vs6.270 s），不能直接拿合计作百分比；按每次 acquire 归一后 CPU 降约80%。持锁释放节点的 elapsed 大幅下降，支持之前“持锁者被忙等竞争者挤压”的方向，但还没有同锁对象级 owner 链。

B2最长选中 task-group 通知窗口32.535 ms，其最终 notifier 的 queue acquire29.333 ms / CPU6.217 ms，payload仅0.130 ms。相比之前105.365 ms样例显著变短，但不同采集长度的最大值不构成严格长尾分位数对照。

最慢完整 source 帧144.984 ms，Guest19等Guest1通知117.716 ms；唤醒到运行0.0385 ms、重获锁0.0017 ms。生产者在此窗口 CPU57.691 ms、sleep47.479 ms、runnable12.546 ms，不能说全是GPU或全是CPU计算。另有 submit worker 的 KGSL wait77.786 ms/CPU0.021 ms，GPU独立等待仍在；不与上述CPU窗口相加。

## Orbis semaphore 的定向唤醒

TSC修复后更多竞争转为正常睡眠，暴露了另一个具体放大点：`GuestKernelSemaphore` 的所有对象共用一个 `condition_variable_any`。`Wake` 已经按FIFO/优先级及可满足计数选好了 waiter，但Signal随后仍 `notify_all`，把其他对象及本对象未满足条件的 waiter 一起叫醒，竞争同一个短 metadata mutex。

改为每个 Waiter 自带条件变量，仅通知已置 done 的 waiter。Cancel/Delete逐个通知受影响对象的waiter；Stop仍走可取消等待。保留计数、可满足 waiter 扫描、优先级、错误码、句柄生命周期和超时写回语义。仍有 semaphore registry/state 的短 mutex，并未宣称完全无锁或完全按对象分锁；POSIX sem_t 的另一套实现本轮未改。

Citron `KLightLock` 在选定 owner/waiter 后对相应 KThread 结束等待，亦非所有 semaphore 的广播；采用定向通知原则，不替换为 Switch 的调度协议。

- 旧实现反例：32次 signal 后，不相关对象和同对象未满足条件的 waiter 分别76/78次主动切换；588项中2个预期失败。
- 修复后 Android：588/0，对应2/3次切换；包括原有计数/优先级/取消/销毁/超时映射退役，以及新增64轮Signal/Stop并发。该计数是测试进程的实际线程context switches，不是通过加生产计数器得到。
- LLVM ThreadSanitizer：511/0，无race报告。最初macOS16 KiB页上的3项失败来自旧测试硬编码4 KiB重映射；改为HostPageSize后通过，Android的映射退役断言继续保留。
- 修正独立Android测试目标缺少`log`的链接依赖，并加入不依赖完整renderer的同一套semaphore测试入口。

最终版本 APK **432129ae** / host **f299a128** / JNI **bc53f543**，host Build ID已与APK内库核对一致。普通PID5695/gen1、无探针，三个10秒静止窗口为 **9.737 / 9.823 / 9.789 FPS**。与前面的A/B相比镜头距离略有改变，未单独做定向唤醒的严格同视角A/B/A，因此不把全部差值归因于第二项修复。

短时触屏左摇杆移动、右摇杆转动视角均在前后截图中可见，标记为诊所MOVE/相机操作的有限验收；不包含战斗、存档循环、音频同步或完整游戏稳定性验收。最后正常UIStop，Stopped/user_stop；auto-tag next-session清空、当前未装探针、profile_sync0、GPUtimingOFF、file captureOFF、ringON、TracerPid0，所有自动输入已停止，两个独立tracefs instance及本轮设备临时目录均清理。

归档期间又观察到PID8942/gen1正常Running。系统exit-info将验收PID5695的后续退出记为USER REQUESTED/FORCE STOP（发起PID29947），不是native crash；本轮未发起该重启。保留新会话，不再输入或Stop；只读状态确认ringbuffer ON、file capture_active0。上述Stopped/user_stop指本轮验收结束时的状态，不能当设备此刻仍停止。

[归档证据](evidence/bloodborne-tsc-20260920/manifest.json)含APK/host身份、测试、FPS窗口、同窗分析摘要和原始大文件SHA。游戏ELF、APK、PROF/ftrace、构建库和截图保留在忽略的`build/validation/bloodborne-tsc-20260920/`，不随源码提交。

## 范围与后续

高频 guest TSC 改变的是硬编码 ticks 的实际时长，依据频率换算成秒的游戏代码保持原速。本轮不是全游戏回归，不证明全部 PS4 标题都兼容，也不消除 GPU 独立的长等待。

下一步先在定向唤醒的最终版本重采实景Litep/KGSL，重新排序CPU生产者、renderer与GPU成本；本报告B2的逐函数数据属于第二项修复之前，不能直接当最终版本的剩余耗时。如果队列contention仍明显，再在对象级关联owner、waiter、持锁期间allocator调用，评估节点释放移出临界区的生命周期边界。不得跳过 semaphore、伪造完成或直接提前解锁。GPU 方向继续定位实际 submit worker 的 KGSL wait，不能把 CPU 与 GPU 重叠等待相加。

## 源码交付

- `a63e900f`：Orbis semaphore定向唤醒与反例/并发检查。
- `30f1016a`：统一FEX/HLE时钟域及真实guest时钟检查。

提交目标为 `origin/feature/malos/hle_vr`。FEX、Foundation、Citron子仓未修改；独立的未跟踪 `externals/dear_imgui/` 保留原状。
