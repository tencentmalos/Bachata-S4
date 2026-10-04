# 血源 Android：写跟踪缺页预测与同步唤醒代理（AYANEO Pocket DS，2026-10-03）

接 [10-02 Android 瓶颈分析](bloodborne-android-bottleneck-20261002.md) 第 5 节的前三项建议：GPU 写跟踪缺页、HLE 同步唤醒、ADPF 线程提示。本轮实现前两项并设为 Android 默认开启，第三项实测无效。第二轮（第 7 节）又把“未变内容页不衰减”改为默认，并新增 guest 线程避开小核、写缺页跳过无关纹理失效两项。

## 现场

- **设备**：AYANEO Pocket DS（`01108YHE01017563`，SG8275 / Adreno 740，Android 13，无 root），全程 Thermal Status 3。
  - 降频状态：中核（cpu3–6）上限 1.786 GHz，X3（cpu7）上限 1.594 GHz，小核（cpu0–2）2.016 GHz；GPU 680 MHz。
- **场景**：读档后的中央亚楠起点，角色静止，每帧约 930 draw，Internal Scale 0.5。
- **方法**：
  - 所有新行为都可在运行时用 DebugBus 切换，同一会话内“关/开”交替 3 轮，每轮先稳定 15–20 s、再采 20 s。设备温度在几十分钟内持续上升，帧率随之单调下滑，只比较相邻轮次。
  - 帧率与线程 CPU 取自 `/proc/<pid>/task/*/stat` 和 `debug_status` 的 guest flip 计数；缺页、预放、上传、哈希次数取自 `upload_diag status` 计数之差。
  - simpleperf 2000 Hz 帧指针调用栈；Perfetto（无 root）`sched_switch/waking`。simpleperf 只取比例，绝对时间以 /proc 和 sched 为准。
  - 原始 perf.data、Perfetto trace 只在本地，未入仓；设备上的采集文件已删除。

## 1. 结论

| 项目 | 结果 | 默认 |
|---|---|---|
| 内容哈希预测（`upload_diag watch_content`） | 26.66 → 27.73 FPS（+4.0%）；写缺页 296 → 173 次/帧 | Android 开；不衰减（新默认）时不起作用 |
| 唤醒代理（`wake_proxy`） | 27.24 → 27.93 FPS（+2.6%，在内容预测已开的基础上）；guest 线程合计少约 9 ms CPU/帧 | Android 开 |
| 两项合计（同一会话交替） | **25.72 → 27.40 FPS（+6.5%）**，每轮“开”都高于相邻“关” | — |
| ADPF `perf_hint` | 26.80 → 26.50 FPS，无收益 | 关 |
| 跨空洞预放（`watch_gaps`） | 缺页 167 → 195 次/帧，更差 | 关 |
| 未变内容页不衰减（`watch_decay off`） | 27.26 → 27.88 FPS（+2.3%）；写缺页 165 → 78 次/帧（7.1 节）。新包上（不衰减时不哈希）29.17 → 29.53 FPS，每帧进程 CPU 146.1 → 143.8 ms（7.9 节） | Android 改为不衰减，此时不再哈希 |
| guest 线程避开小核（`guest_affinity`） | 手动 taskset 对照 26.67 → 27.24 FPS（+2.1%）；新包开/关 29.53 → 29.81 FPS（已近 30 帧上限），每帧进程 CPU 160 → 139 ms（7.2、7.6 节） | Android 开 |
| 写缺页跳过无关纹理失效（`upload_diag fault_textures`） | 无可测差别：不跳过时每帧约 83 次，共 0.10–0.15 ms（7.6 节） | 开 |
| 跨区域延续预放（`watch_cross`） | 衰减开时缺页 166 → 179 次/帧；不衰减时缺页 84 → 62 次/帧，但多出的上传与 mprotect 抵消了收益，每帧 CPU 与 FPS 无差别（7.4、7.9 节） | 关 |
| 流式拷贝先进缓存（`stream_bounce`） | 26.21 → 26.03 FPS，无收益（7.5 节） | 关 |

- **最终装机**：APK `1881c71d`（host `f5d22fe4`），Turnip `01a3548f`。默认：唤醒代理（队列 32 项）、不衰减（此时不哈希）、guest 线程避开小核、写缺页跳过无关纹理失效；内容预测开关仍为开，只在衰减开启时起作用。这一版的游戏内复测见 7.6 节。
  - 第一轮游戏内 A/B 用的是 `57250251`、`55b6eb40`、`850bdef9`；第二轮用 `ed7d2a1b`（host `976cef03`，衰减仍为默认）。
- **剩余开销**：两项都开后，Guest-1 的 futex 内核时间 3.71 → 0.97 ms/帧。再关掉衰减后，冷页缺页几乎消失，每帧仍有约 75 次“流起点”缺页和 9 次“前页已脏”缺页。
- **新默认值全开**（7.6 节）：中央亚楠起点已到 30 帧上限（29.8–29.9 FPS）。Guest-1 每帧 24.1 ms，其中 JIT 19.6 ms；全部 guest 线程的写缺页处理 6.66 → 2.17 ms/帧。中大核忙碌约 80%，场景更重时会先饱和。
- **核心暂停**：连续运行约 75 分钟后，`core_ctl` 把中核簇和 X3 的 `active_cpus` 降到 0，所有线程挤到 3 个小核上，帧率掉到 3.5–5 FPS（第 8 节）。随后查到电池只剩 2%：设备接在电脑 USB（900 mA）上，供电跟不上游戏负载，测试期间一直在掉电，低电量限流（BCL）很可能是主因。之后的 A/B 必须先确认 `active_cpus` 和电量。

## 2. 每页上传热度（`page_heat`）

新增默认关闭的诊断 `page_heat on|off|reset|status`：按 4 KiB 页统计每帧的上传次数、与上次上传相比内容是否变化、被 `ObtainResidentBuffer` 覆盖的次数，以及写跟踪计数的每帧差值。

316 帧的结果（内容预测尚未实现）：

- 每帧上传 914 页，其中 677 页内容确实变了；14,030 个不同的页参与上传。
- 只有 25 页在 ≥90% 的帧里都被上传，102 页在 50–90% 的帧里。其余 13,900 页各自不到一半的帧，属于环形缓冲：每帧写一段，十来帧绕回一次。
- 每帧 2,386 次 resident 查找共覆盖 549,076 页：绑定范围很大，已预放但尚未写到的页也会被上传。

因此 10-02 报告设想的“对每帧都写的热页停止保护”只覆盖 127 页、约 97 次上传/帧，收益很小，没有实施。

## 3. 内容哈希预测（`watch_content`，Android 默认开）

### 3.1 原因

- 写缺页时，`RegionManager::MarkWriteFault` 会把同一 64 页窗口内、紧随其后且有写置信度的页一并解除保护（“预放”），省掉这些页后续的缺页。
- 页被上传时结束本轮写周期。置信度是 2 位：真实缺页置 3；被预放的页无法知道 CPU 是否写过，每轮减 1。
- 所以一个每轮都被重写的页，预放 3 轮后置信度降到 0，第 4 轮又缺页一次。实测 296 次缺页/帧、mprotect 353 次/帧。

### 3.2 实现

- 被预放或缺页的页在上传时计算内容 XXH3（页已重新写保护，哈希的就是上传的内容），与该页上次上传时的哈希比较。
- 预放页内容变了，说明 CPU 确实写过，置信度直接置 3；没变的照旧减 1。
- 只影响预测：每个脏页仍然照常上传，不跳过任何拷贝。
- 哈希存放在区域首次需要时于锁外分配的数组里；`MemoryManager::HashSparsePages` 按 VMA 处理未映射的洞，与 `CopySparseMemory` 一致。
- 上传路径改为 `RegionManager::CleanForUpload`：清脏位、重新保护、拷贝、哈希、结束写周期。

### 3.3 结果（APK `57250251`，内容预测默认关时的同会话交替）

| | 关 | 开 |
|---|---|---|
| FPS | 27.11 / 26.56 / 26.30 | 28.02 / 27.73 / 27.44 |
| 写缺页 /帧 | 296 | 173 |
| mprotect /帧 | 353 | 232 |
| 预放页 /帧 | 625 | 1,001 |
| 上传后重新保护的页 /帧 | 1,187 | 1,310 |
| 哈希 /帧（其中内容变化） | — | 1,176（502） |
| Guest-21..25（各） ms/帧 | 6.6–6.9 | 5.4–5.7 |
| Guest-1 ms/帧 | 25.8–26.8 | 25.3–25.9 |
| GpuComm ms/帧 | 22.1–23.1 | 23.4–24.2 |

- GpuComm 多出约 1.4 ms/帧：XXH3 0.40 ms（约 0.4 µs/页），多上传的拷贝 0.19 ms，其余散布在各函数的小幅上涨里。GpuComm 不在关键路径。

## 4. 唤醒代理（`wake_proxy`，Android 默认开）

### 4.1 唤醒的成本

- **单次唤醒**：独立基准 `wake_bench`（游戏运行时在设备上测）显示，唤醒一个已睡眠的线程，唤醒方要花 12–20 µs CPU（中位数 12–17 µs）。把被唤醒线程固定在单核只降到约 12 µs，所以主要成本不在选核，而在内核唤醒路径本身。内核符号不可见，无法细分。
- **主线程每帧约 176 次唤醒**：Perfetto 10 秒内 Guest-1 发起 48,566 次唤醒，主要对象是 Guest-32..36、Guest-21..25、Guest-54..59。simpleperf 中 Guest-1 的 `notify_one` 约 2.84 ms/帧，futex 内核时间 3.71 ms/帧。
- **唤醒成批出现**：Guest-1 相邻两次唤醒的间隔，72% 小于 100 µs，82% 小于 250 µs。
- **`hle_sync` 计数（全部线程，每帧）**：`Sema.Signal` 115 次，平均 33.5 µs；`Cond.Broadcast` 55 次，平均 31 µs。按等待次数换算，平均每次信号只唤醒约 1.3 个等待者，所以“唤醒一个、由它再唤醒下一个”的委托方案收益有限。

### 4.2 实现

- **代发线程**：新增 `shadPS4:Waker`，固定在算力最低的 CPU（按 `cpu_capacity`，这台设备是 cpu0–2），High 优先级。
- **接入点**：内核信号量（`DeferredWakes`）、条件变量（`CondNotify`）、快路径 mutex 的地址等待（`GuestAddressWaiters::Wake`）。等待方的状态仍在其自身锁下发布；代理只代做之后的 notify，并持有 `shared_ptr` 保证对象存活。
- **队列**：有界 MPSC，满了由调用方自己唤醒。
  - 第 4.3、4.4 节的游戏内测量用的是 4096 项。
  - 压力测试表明，生产者持续连发时代理（A510 上约 55 µs/次）跟不上，4096 项的积压意味着最坏约 225 ms 的唤醒延迟。
  - 最终版改为 32 项，并新增 `max_depth` 计数。第二轮在游戏中约 45 分钟、749 万次投递，队列一次也没满（`full` 0），最大深度 13。
- **空闲与入睡**：
  - 代理每处理完一批，在 200 µs 窗口内用 `ldxr`+`WFE` 低功耗等待新请求。请求写入 `posted` 所在的缓存行会唤醒 WFE；定时器事件流保证 WFE 不会无限期睡下去。
  - 窗口结束后进入 futex 睡眠。睡眠前后与发起方用 seq_cst 的 `posted` / `awake` 两个变量握手：要么代理看到新请求，要么发起方看到代理已睡并唤醒它，不会丢唤醒。
- **发起方开销**：代理醒着时，发起方只做一次入队和两次原子操作，不进内核。
- **开关**：每次会话启动时开启，除非 `debug.shadps4.wake_proxy=0`；DebugBus `wake_proxy on|off|spin <us>|status`。代码在 `src/core/host_runtime/guest_wake_proxy.{h,cpp}`；头文件只放一个原子函数指针钩子，未链接实现的单测照旧直接唤醒。

### 4.3 结果（APK `55b6eb40`，内容预测开，代理默认关时的同会话交替）

| | 关 | 开 |
|---|---|---|
| FPS | 28.07 / 26.89 / 26.75 | 28.69 / 27.64 / 27.47 |
| Guest-1 ms/帧 | 25.3 / 26.5 / 26.9 | 25.2 / 25.7 / 25.9 |
| Guest-17 ms/帧 | 12.7 / 13.7 / 13.6 | 10.9 / 11.6 / 11.4 |
| 全部 guest 线程 ms/帧 | 116 / 122 / 122 | 107 / 113 / 113 |
| 进程 CPU（核） | 4.65 | 4.95 |

- **代理统计**：只有约 13% 的请求需要先唤醒代理本身，其余发起方零系统调用；WFE 等待约占 0.3 个小核。
- **代理自身的唤醒很慢**：在 A510 小核上，代理每次唤醒约 55 µs（中核约 15 µs）。突发唤醒中排在后面的线程会更晚被叫醒：Guest-1 等工作线程的睡眠从 7.6 增到 8.3 ms/帧，抵消了一部分收益。
- **窗口长度**：spin 200 与 1000 µs 无可测差别（27.0 / 27.2 FPS），保持 200 µs。

### 4.4 两项合计（APK `850bdef9`，两项默认开，同会话交替“全关 / 全开”）

| | 全关 | 全开 |
|---|---|---|
| FPS | 26.59 / 25.38 / 25.19 | 27.80 / 27.21 / 27.20 |
| Guest-1 ms/帧 | 26.5 / 27.8 / 27.8 | 25.5 / 25.8 / 25.8 |
| Guest-17 ms/帧 | 13.1 / 14.2 / 14.5 | 11.3 / 11.6 / 11.8 |
| Guest-21..25 合计 ms/帧 | 34.0 / 35.5 / 35.5 | 29.2 / 30.0 / 30.6 |
| GpuComm ms/帧 | 22.7 / 23.8 / 24.3 | 25.6 / 27.6 / 28.6 |

- **GpuComm**：多出约 3.75 ms/帧（采样），其中 XXH3 0.45 ms、两处拷贝 0.38 ms，其余几十个函数各涨一点，均匀变慢。
  - 按 CPU 核分组，两种情况下 GpuComm 与 Guest-1 都各占一颗中核，并非同核争用。
  - 原因未确认，可能与小核负载上升引起的共享缓存/功耗分配有关。
  - GpuComm 每帧仍有约 11 ms 在等 Guest-20 提交，暂不在关键路径。

## 5. ADPF `perf_hint`（默认关）

- **做法**：`APerformanceHint` 会话覆盖全部 Guest 线程和 GpuComm / VkRecorder，每帧上报帧间隔，目标 33.3 ms。
- **结果**：同会话交替，关 27.11 / 26.55 / 26.74，开 26.62 / 26.37 / 26.50；每轮“开”都略低于相邻的“关”，在噪声内。
- **结论**：Thermal Status 3 下平台不再给提频空间，保留为诊断开关。

## 6. 其他实验

### 6.1 Guest-1 的帧时间组成（Perfetto，两项都开）

- **每帧构成**：运行约 25.6 ms，等工作线程约 8.3 ms，可运行排队约 1.7 ms，D 约 0.6 ms。
- **可运行排队**：约一半时间里，Guest-1 最终运行的那颗核当时正处于 idle，是 CPU 退出空闲状态的延迟，不是被别的线程抢占。
- **线程优先级**：Guest-1 是 nice −10，GpuComm / VkRecorder / Present 是 −14，但这些线程造成的 Guest-1 排队合计只有每帧零点几毫秒。
- **短睡眠**：Guest-1 有 38% 的睡眠短于 50 µs，但合计只有 66 ms/10 s。让它先自旋再睡，浪费的自旋会超过节省，没有实施。

### 6.2 缺页细分与两个预测实验

在 `MarkWriteFault` 中按缺页页的状态计数：

- **冷页**：缺页页没有写置信度；
- **前页已脏**：前一页已是 CPU 脏页，本页却被重新保护了；
- **流起点**：本页有置信度，但前面没有缺页把它预放。

内容预测开启时每帧约为冷页 84、前页已脏 6.5、流起点 84。冷页几乎都是“曾有哈希记录、置信度衰减到 0”的已知页。“流式写入中途被上传导致重复缺页”的猜测不成立。

- **`watch_gaps`**：预放时遇到无置信度的页不再停止，而是跳过，继续释放窗口内后面的页。
  - 流起点减少约 19 次/帧，但冷页增加约 20 次，总缺页 167 → 195 次/帧，FPS 也更低。
  - 判为负结果，默认关。
- **`watch_decay off`**：预放后内容未变的页不再衰减置信度。
  - 缺页 ~160 → ~84 次/帧，mprotect ~220 → ~145 次/帧，冷页 77 → 5。
  - 代价是每帧多上传约 150 页（1,330 → 1,475），其中内容未变的上传 500 → 710。
  - 这组数据是在核心暂停状态下采的（第 8 节），计数可比，FPS 与 CPU 不可比。电量恢复后的 FPS 复测见 7.1 节，结果为正，已改为默认不衰减。

## 7. 后续轮次：不衰减、线程放置及其他

7.1–7.5 节为第二轮，7.6 节为第三轮（新包），7.7 节为未单独测的小改动，7.8 节为受干扰的第四轮（只保留计数），7.9 节为第四轮的干净重测。

- **第二轮条件**：APK `ed7d2a1b`（host `976cef03`），默认同第一轮最终版（内容预测、唤醒代理开，衰减开）。
  - 05:07–05:26，电量 12% → 5%（仍接电脑 USB，持续掉电），Thermal Status 3。
  - 每个窗口都记录 `core_ctl/active_cpus` 和电量，全程中核簇 4 核、X3 在线，没有发生核心暂停；电量低于 6% 时脚本自动停止。
  - 每组同会话交替 3 轮，每轮稳定 15 s、采 20 s；同时记录每帧进程 CPU（核数 × 帧时间）和代理线程 `shadPS4:Waker` 的 CPU。
- **帧率随时间下滑**：首个窗口 27.88 FPS，最后一组约 25.9 FPS（温度与电量），所以只比较同组内相邻轮次。

### 7.1 不衰减（`watch_decay off`）

| | 衰减（开） | 不衰减（关） |
|---|---|---|
| FPS | 27.31 / 27.40 / 27.06 | 27.92 / 27.93 / 27.78 |
| 写缺页 /帧 | 165 | 78 |
| mprotect /帧 | 221 | 135 |
| 冷页 / 流起点 / 前页已脏（次/帧） | 80 / 83 / 6.6 | 0.6 / 75 / 8.8 |
| 哈希 /帧（其中内容未变） | 1,159（479） | 1,462（785） |
| Guest-1 ms/帧 | 25.6 | 25.2 |
| Guest-21..25 合计 ms/帧 | 29.4 | 25.1 |
| GpuComm ms/帧 | 25.9 | 26.5 |
| 全部 guest 线程 ms/帧 | 114.2 | 110.8 |

- 每轮“关”都高于相邻的“开”，均值 +2.3%。
- 代价是 GpuComm 多约 0.6 ms/帧：每帧多上传并哈希约 300 个内容未变的页。
- **最坏情况**：写过一次的页不再降低置信度。游戏不再写某页后，只要同一窗口里前面有缺页把它预放，它就会被当作脏页上传一次；每次缺页最多多上传一个窗口（64 页），不影响正确性。
- 已改为默认不衰减（`decay_unchanged` 默认 false）。
- **不衰减时不再哈希**：预放只看置信度是否非零，而不衰减后，内容是否被改写都不会让置信度变为 0。所以哈希的结果不再影响任何决定。
  - 改为只在衰减开启时哈希（`SnapshotForUpload`）。衰减关闭时，预放过、上传前没有缺页的页一律保留置信度，计入新计数 `kept_pages`。
  - 与上表测的“不衰减 + 哈希”相比：首次被哈希的页原本会衰减一次，现在不衰减；被改写的页原本升回 3，现在保持原值。这两点都不会让置信度变为 0，只有在 CPU 清除路径另外衰减时才可能有差别。预计省掉上表 GpuComm 多出的大部分（每帧约 1,460 页 XXH3），新包实测见 7.6 节。
  - 单测：`tests/common/memory_tracker_concurrency_tests.cpp` 新增写预测用例（衰减开、关，以及衰减开且哈希），Pocket DS 上 62/0。
  - 负对照：把衰减关闭时的逻辑改回“只保留哈希判定为未变的页”，2 项失败。
  - 该测试由 `scripts/android/test-buffer-tracking-locks` 单独编译，不在 CMake 中；本轮给 `SnapshotForUpload` 加哈希回调后它已无法编译，这次一并修复。

### 7.2 guest 线程避开小核

- **手动对照**：用 `taskset` 把除 Guest-1 外的全部 66 个 `Guest-*` 线程限制在 cpu3–7（`f8`），与不限制（`ff`）交替。

| | 不限制 | cpu3–7 |
|---|---|---|
| FPS | 26.84 / 26.96 / 26.22 | 27.63 / 27.09 / 27.00 |
| 每帧进程 CPU（ms） | 184.6 | 165.9 |
| 全部 guest 线程 ms/帧 | 116.5 | 94.7 |
| Guest-17 ms/帧 | 12.3 | 9.3 |
| Guest-21..25 合计 ms/帧 | 30.0 | 25.8 |
| Guest-1 ms/帧 | 26.1 | 26.8 |
| VkRecorder ms/帧 | 10.1 | 12.7 |
| GpuComm ms/帧 | 26.9 | 26.9 |

- 每轮限制都高于相邻的不限制，均值 +2.1%。
- 同样的工作在中核上用的 CPU 时间更少（guest 线程合计 −22 ms/帧）；Guest-1 与 VkRecorder 因中大核更挤而略增。
- **实现**（`guest_affinity`）：
  - 新文件 `src/core/host_runtime/guest_cpu_placement.{h,cpp}`：guest 线程进入 guest 代码时（`GuestRuntime::Attach`）设置亲和性，排除算力最低的那一簇（按 `cpu_capacity`，这台设备是 cpu0–2）。
  - 较快的 CPU 少于 4 颗时不启用（例如 2 大 + 6 小的机型），避免把工作线程挤到太少的核上。
  - DebugBus `guest_affinity on|off|status`：切换时遍历 `/proc/self/task` 中名为 `Guest-*` 的线程，与 taskset 实验的选择相同，不保存线程 id，因此不会误设已退出线程复用的 id。
  - `debug.shadps4.guest_affinity=0` 关闭；默认开。
  - 读 `cpu_capacity` 的代码从唤醒代理移到 `Common::GetCpuCapacitySplit`，代理继续用其中算力最低的一簇。
- **与实验的差别**：实现也包括 Guest-1（实验没有限制它）。Guest-1 本来就几乎都在中大核上，影响预计很小，但未单独测。新包的开/关复测见 7.6 节。
- **未测**：能耗；长时间运行后中核簇的温度变化（每轮只有 45 s）。

### 7.3 写缺页跳过无关的纹理失效（`fault_textures`）

- **原因**：每次写缺页，`Rasterizer::InvalidateMemoryFromWriteFault` 在缓冲区释放该页之后都会调用 `TextureCache::InvalidateMemory`。后者先拿纹理缓存的独占锁再查该页上的图像，而 GPU 线程每次查纹理都要拿这把锁。环形缓冲的页上一般没有图像，这一步是白做，还可能要等 GPU 线程释放锁。
- **做法**：缓冲区释放后，若这些页在 `PageManager` 中已没有写关注者（计数为 0），说明没有任何图像跟踪它们，跳过纹理缓存。
  - 图像注册（`TrackImage`）先在同一把页锁下增加关注者计数并加保护，再读取内存上传。并发注册时，重试的写要么再次因图像的保护而缺页、走完整路径，要么发生在上传读取之前。
  - 未被跟踪的图像本来就收不到 CPU 写的通知（页未保护时写入不会缺页），所以跳过不改变纹理缓存依赖的任何保证。
- **计数与开关**：`upload_diag status` 新增一行 `fault_textures=… texture_invalidates=… texture_skipped=… texture_ns=…`（调用次数、跳过次数、调用耗时）；`upload_diag fault_textures skip|always`，默认 skip。
- **预期**：第一轮两项全开时的 simpleperf（衰减开，233 帧）中，全部 guest 线程的写缺页处理（`DispatchAccessViolation`）在 CPU 上约 6.66 ms/帧：
  - mprotect（`ProtectGpu`）4.72 ms/帧，约 28 µs/次缺页；
  - `Rasterizer::IsMapped` 0.64 ms/帧（7.7 节）；
  - `TextureCache::InvalidateMemory` 只有 0.17 ms/帧。采样看不到等锁的时间，跳过它能省多少要看新计数里的 `texture_ns`。
- **效果**：这个场景无可测差别，见 7.6 节。

### 7.4 跨区域延续预放（`watch_cross`，衰减开）

- **做法**：预放一直放到区域末尾都没遇到空洞或 GPU 页时，接着在下一个区域的第一个窗口继续（另取下一区域的锁）。
- **结果**：

| | 关 | 开 |
|---|---|---|
| FPS | 25.96 / 25.95 / 25.73 | 25.89 / 25.79 |
| 写缺页 /帧 | 166 | 179 |
| 流起点（其中窗口起点）/帧 | 84（9.5） | 64（1.8） |
| 冷页 /帧 | 80 | 97 |

- 延续本身生效：落在窗口起点的流起点缺页几乎消失，流起点总数也少了约 19 次。但多放出的页在衰减开启时很快降到零置信度，冷页反而多了约 17 次，总数更差，FPS 无差别。
- 电量保护在第 6 轮前停止，只有 2 轮“开”。
- 默认关。不衰减条件下的干净重测见 7.9 节：缺页少约 27%，但多出的上传与 mprotect 抵消了收益。

### 7.5 流式拷贝先进缓存（`stream_bounce`）

- **做法**：小只读缓冲先从 guest 内存拷到栈上的缓存缓冲，释放内存管理器读锁后再写入 stream buffer，避免锁释放等待对写合并内存的写入完成。
- **结果**：关 26.36 / 26.31 / 25.95，开 26.15 / 25.96 / 25.98，GpuComm 27.6 / 27.4 ms/帧，无收益，默认关。

### 7.6 新包复测（APK `1881c71d`）

- **条件**：设备熄屏充电约 1 小时后，06:22–06:32 进行，电量 12% → 7%，全程中核 4 核、X3 在线，Thermal Status 3。
  - 电量低于 8% 时脚本停止，所以不衰减条件下的 `watch_cross` 和 `watch_decay` 两组没有跑。
  - 设备经过降温，基线不能与第二轮直接比较（第二轮首窗 27.88 FPS、每帧进程 CPU 178 ms）。
- **基线**（全部新默认）：29.94 FPS，已到 30 帧上限；每帧进程 CPU 135 ms。
  - 写缺页 78 次/帧（冷页 0.1、前页已脏 8.8、流起点 75.7），与第二轮“不衰减 + 哈希”相同；哈希 0；保留置信度的预放页 1,388 页/帧。
  - 写缺页的纹理失效全部跳过，每帧约 85 次。
- **`guest_affinity`**（新实现，包括 Guest-1）：

| | 关 | 开 |
|---|---|---|
| FPS | 29.62 / 29.68 / 29.30 | 29.83 / 29.80 / 29.79 |
| 每帧进程 CPU（ms） | 155.0 / 159.4 / 165.5 | 135.8 / 139.9 / 142.0 |
| 全部 guest 线程 ms/帧 | 100.5 / 102.8 / 106.0 | 76.3 / 80.7 / 81.3 |
| Guest-1 ms/帧 | 23.8 / 23.8 / 24.1 | 25.3 / 24.9 / 24.7 |
| Guest-17 ms/帧 | 9.9 / 10.5 / 11.3 | 7.6 / 8.0 / 8.2 |

  - 帧率已接近上限，差别被压缩，但每轮“开”仍高于相邻的“关”；每帧进程 CPU 少约 13%。
  - Guest-1 自身多约 1 ms/帧（中大核更挤）。
- **`fault_textures`**：

| | 总是调用 | 跳过 |
|---|---|---|
| FPS | 29.57 / 29.55 / 29.26 | 29.54 / 29.40 / 29.30 |
| 每帧进程 CPU（ms） | 143.4 / 144.5 / 145.2 | 144.5 / 144.9 / 145.7 |
| 纹理失效 次/帧（耗时 µs/帧） | 83（97–148） | 0（约 1） |

  - 无可测差别：这个场景中锁没有争用，每次约 1.5 µs。启动以来真正命中被跟踪图像的少数调用平均约 150 µs（主要在加载阶段）。
  - 保留默认跳过：语义安全，并避免缺页线程等 GPU 线程持有的纹理缓存锁。
- **唤醒代理**：本轮 341 万次投递，队列从未满，最大深度 12。
- **采样与调度**（复测结束后立即采集；Perfetto 10 s 约 298 帧，simpleperf 10 s 320 帧）：
  - Guest-1 每帧运行 24.4 ms、可运行排队 2.6 ms、睡眠 5.75 ms（大多由代理线程唤醒）、D 0.56 ms，不再在小核上运行。
  - simpleperf 中 Guest-1 每帧 24.1 ms：JIT 19.6，宿主、libc、FEX、内核合计约 4.5；内核 1.1 ms，其中 futex 0.59、mprotect 0.23。
  - 全部 guest 线程的写缺页处理 2.17 ms/帧（第一轮两项全开、衰减开时 6.66）：mprotect 4.72 → 1.64，`IsMapped` 0.64 → 0.07（其中可重入锁登记 0.42 → 0.03），纹理失效 0.17 → 0。两次的温控状态不同，只用来看结构。
  - GpuComm 中内容哈希（`HashSparsePages`）为 0。
  - cpu3–7 忙碌率 77–81%，小核 41–45%。工作线程的可运行排队时间与运行时间相当（w32-36 排队多于运行）。帧率已到上限，暂不影响；场景更重时中大核会先饱和。

### 7.7 可重入锁登记不再分配内存

- **原因**：`Rasterizer::IsMapped` 每次缺页都要调用（`InvalidateMemoryFromWriteFault` 一次，`PageManager::UpdatePageWatchers` 的映射检查又一次）。它用的 `RecursiveSharedLock` 在 `thread_local std::unordered_map` 里登记每把锁：每次加锁插入、解锁删除，都伴随堆分配，而且发生在信号处理中。
  - 同一份 simpleperf 中，`IsMapped` 全进程 0.68 ms/帧，其中登记占 0.42 ms/帧，几乎全部在 guest 线程的缺页路径上。
- **做法**：`src/common/recursive_lock.cpp` 改为每线程最多 8 项的小数组线性查找，接口和断言不变。整个代码库里只有 `IsMapped` 和 `ForEachMappedRangeInRange` 两处在用。
- **效果**：预计每帧省零点几毫秒 guest 线程 CPU，太小，无法用 FPS 测出，没有单独 A/B；随新包复测确认运行无异常。

### 7.8 第四轮（受干扰，只保留计数）

- **经过**（07:08–07:32，APK `1881c71d`）：
  - 第一次启动时设备处于休眠，`am start` 到达 MainActivity 后它立即因休眠暂停；App 随后在后台启动会话服务，被 Android 拒绝（`BackgroundServiceStartNotAllowedException`），App 崩溃，这一次的各窗口全部无效。App 的这个缺陷已另行修复，见[休眠启动崩溃](android-sleep-launch-20261003.md)。
  - 05:44 用 TaskStop 停掉的一个等电量脚本（目标 15%）实际没有退出，07:19 电量达到后它自行启动游戏，从 07:22 起又跑了一遍第三轮的流程（`guest_affinity`、`fault_textures`）。与此同时第四轮在同一会话里切换 `watch_cross` 和 `watch_decay`。
  - 两边的开关互相叠加，两份采样脚本共用设备上的临时文件，所以两边的 FPS 和线程 CPU 都无效。脚本已改为按窗口命名临时文件。
- **仍有效**：写缺页相关计数只受 `watch_cross` / `watch_decay` 影响，而这两个开关只有第四轮在切。
- **`watch_cross`（不衰减）**，每帧：

| | 关 | 开 |
|---|---|---|
| 缺页（按分类合计） | 84.9 | 62.0 |
| 流起点（其中窗口起点） | 75.9（9.1） | 53.8（4.1） |
| mprotect | 136.5 | 142.7 |
| 保留置信度的预放页 | 1,407 | 1,905 |
| 跨区域预放页 | 0 | 1,085 |

  - 缺页少约 23 次/帧（−27%），但每次延续多一次 mprotect，每帧还多上传约 500 页。是否划算要看 CPU，尚无有效测量，默认保持关。
- **衰减开（带哈希）与关（不哈希）**，每帧：缺页（按分类合计）176.2 与 84.3，mprotect 229.3 与 136.7，哈希 1,190 与 0 页，保留置信度的预放页 0 与 1,416。与第二轮一致。
- 证据：[第四轮两份日志（受干扰）](evidence/bloodborne-android-sync-faults-20261003/ab-round4-contaminated.txt)。

### 7.9 第四轮重测（干净）

- **条件**：07:57–08:08，APK `1881c71d`，电量 13% → 9%，中核 4 核、X3 全程在线，Thermal Status 3。
  - 开始前确认电脑上没有其他实验进程；启动前确认设备已唤醒并解锁。
  - 采样脚本改为每个窗口使用自己的设备临时文件。
- **`watch_cross`（不衰减）**：

| | 关 | 开 |
|---|---|---|
| FPS | 29.91 / 29.99 / 29.58 | 29.80 / 29.81 / 29.81 |
| 每帧进程 CPU（ms） | 135.7 / 136.7 / 140.3 | 135.9 / 138.9 / 140.9 |
| 缺页（按分类合计）/帧 | 84.1 | 61.6 |
| mprotect /帧 | 137.0 | 143.6 |
| 保留置信度的预放页 /帧 | 1,423 | 1,956 |
| Guest-21..25 合计 ms/帧 | 18.1 | 17.5 |
| GpuComm ms/帧 | 21.8 | 22.5 |

  - 工作线程因缺页减少而少约 0.6 ms/帧，GpuComm 因多上传约 530 页而多约 0.7 ms/帧；每帧进程 CPU 成对比较还略高。结论：保持默认关。
- **衰减开（带哈希）与关（不哈希，新默认）**：

| | 衰减 + 哈希 | 不衰减、不哈希 |
|---|---|---|
| FPS | 29.36 / 29.04 / 29.10 | 29.55 / 29.64 / 29.41 |
| 每帧进程 CPU（ms） | 144.8 / 146.3 / 147.1 | 142.8 / 143.7 / 144.8 |
| 写缺页 /帧（mprotect） | 172（230） | 78（137） |
| 哈希页 /帧 | 1,193 | 0 |
| Guest-21..25 合计 ms/帧 | 22.5 | 19.0 |
| GpuComm ms/帧 | 23.9 | 23.7 |

  - 每轮“不衰减”都优于相邻的“衰减”：FPS +1.2%（已接近上限），每帧进程 CPU −2.3 ms。
  - GpuComm 只少约 0.2 ms：省掉的哈希大致被多出的上传抵消（保留置信度的预放页约 1,420 页/帧，衰减时这些页会重新缺页）。主要收益仍在工作线程。
- 证据：[第四轮重测](evidence/bloodborne-android-sync-faults-20261003/ab-round4-clean.txt)。

## 8. 核心暂停（低电量限流）

- **现象**（03:04，连续运行约 75 分钟后）：
  - 帧率掉到 3.5–5 FPS，所有线程都跑在 cpu0–2；
  - `/sys/devices/system/cpu/cpu3/core_ctl/active_cpus` 和 `cpu7/core_ctl/active_cpus` 都是 0，`global_state` 显示 Cluster pause；
  - CPU7 温度 95 °C，机身 62 °C；进程仍在 `/top-app`，各线程的 `Cpus_allowed_list` 仍是 0–7。
- **复现**：停游戏约 10 分钟后重新进入世界，短时间内又进入同一状态。这时的 FPS 与线程时间都不可用，只有每帧计数可比。
- **原因**：03:34 检查 `dumpsys battery`，电量 2%、电压 3.66 V。
  - 设备接在电脑 USB 上（最大充电电流 900 mA），供电跟不上游戏负载，整个测试期间都在掉电；
  - thermalservice 中 BCL 电量类传感器 `socd`（类型 8）读数 98–99，状态 6，是所有传感器里最高的级别；
  - 推断核心暂停主要来自低电量限流，温度只是叠加因素。03:04 那次没有记录电量，无法排除两者的先后。
- **对测量的影响**：
  - 第 3、4 节的 A/B 都是在电量持续下降中做的，各轮帧率单调下滑，可能部分来自 BCL，所以只比较相邻轮次；
  - 今后 A/B 前后都要读 `core_ctl/active_cpus` 和 `dumpsys battery` 的电量，并使用足功率的充电器。

## 9. 代码与开关

- **写跟踪**（`src/video_core/buffer_cache/`）：
  - `region_manager.h`：`CleanForUpload`、`EndWriteCycle(cleaned, rewritten, kept)`、`ContentHashes`、`ReleaseAhead` / `ContinueReleaseAhead`、`MarkWriteFault` 缺页分类、`watch_gaps`；
  - `memory_tracker.h`：`SnapshotForUpload` 增加 `hash_pages`；写缺页时可选地把预放延续到下一个区域（`watch_cross`，另取下一区域的锁）；
  - `region_definitions.h`：开关与计数；
  - `buffer_cache.cpp`：哈希回调、`page_heat` 钩子；
  - `page_heat.{h,cpp}`：新增诊断。
- **内存**：`MemoryManager::HashSparsePages`（`src/core/memory.{h,cpp}`）。
- **写缺页与纹理缓存**：`Rasterizer::InvalidateMemoryFromWriteFault`（`vk_rasterizer.cpp`）、`PageManager::HasWriteWatchers`（`page_manager.{h,cpp}`），计数在 `Core::GpuWatchCounters`（`address_space.h`）。
- **线程放置**：`guest_cpu_placement.{h,cpp}`（新增）；`Common::GetCpuCapacitySplit` / `SetThreadAffinity`（`common/thread.{h,cpp}`），唤醒代理改用它们。
- **可重入锁**：`common/recursive_lock.cpp`。
- **单测**：`tests/common/memory_tracker_concurrency_tests.cpp`（写预测用例；随 `SnapshotForUpload` 新签名更新）。
- **同步**：
  - `guest_wake_proxy.{h,cpp}`（新增）；
  - `guest_kernel_semaphore.h`、`guest_mutex.h`、`guest_sync_waiters.h` 三处接入；
  - `guest_runtime.cpp` 读取 `debug.shadps4.wake_proxy`。
- **其他**：`common/performance_hint.{h,cpp}`（ADPF）。
- **DebugBus**：
  - `upload_diag watch_content|watch_gaps|watch_decay|watch_cross|stream_bounce on|off`、`upload_diag fault_textures skip|always`；`upload_diag status` 增加 `gpu_watch`、`write_faults`、`fault_textures` 三行，`write_faults` 含缺页分类（冷页及其中已知页、前页已脏、流起点及其中的窗口起点）、未变内容上传数、跨区域预放页数；
  - `guest_affinity on|off|status`；
  - `wake_proxy on|off|spin <us>|status`；
  - `page_heat on|off|reset|status`；
  - `perf_hint on guest|<tids> [ms] | off | status`。
- **默认值**：
  - `predict_from_contents` 仅 Android 开，桌面未测、保持关；`decay_unchanged` 默认 false，只在内容预测开启时起作用；
  - 唤醒代理、guest 线程放置是 Android 宿主运行时代码，桌面没有；
  - `fault_textures` 默认 skip，桌面与 Android 共用这段缺页路径。

## 10. 边界

- 只测了一台设备、一个静止场景，且全程温控。
- 内核内部原因（唤醒为何要 12–20 µs）是推断，没有 root 无法确认。
- **唤醒代理**：
  - 正确性依据是“状态先在等待方的锁下发布，代理只补做 notify”，加上约 1,000 万次真实代发中游戏未出现卡死；
  - 新增 `tests/host_runtime/guest_wake_proxy_tests`（宿主探针，链接 `libshadps4_host.so`）。多个生产者向多个等待者投递，投递间隔随机，覆盖代理的 spin 窗口与入睡切换（含 spin=0）。
  - 等待超时时若存在超过 50% stall 仍未消费的投递，即判为丢唤醒。
  - Pocket DS 上两个容量版本各 11/0 通过：三组共约 32 万次投递，丢唤醒 0，`notifies == posts`。
  - 其他 sync 单测（未链接代理）仍走直接唤醒路径。
- 未做帧时间分布（卡顿）统计，只比较平均帧率。
- 桌面仅编译检查（未运行）；内容预测在桌面保持默认关。
- 新默认值全开后，这个静止场景已到 30 帧上限，FPS 对照被压缩，第三轮主要看每帧 CPU；更重的场景（战斗、移动）没有测。
- “不衰减时不哈希”没有与“不衰减 + 哈希”直接对照；与“衰减 + 哈希”的对照见 7.9 节。
- 线程放置只在一台设备上测过，未测能耗和长时间运行后的温度；“较快的 CPU 至少 4 颗”这一门槛是保守选择，没有在其他拓扑上验证。

## 证据

`evidence/bloodborne-android-sync-faults-20261003/`：

- [page_heat](evidence/bloodborne-android-sync-faults-20261003/page-heat.txt)
- [内容预测 A/B](evidence/bloodborne-android-sync-faults-20261003/ab-watch-content.txt)、[GpuComm 差分](evidence/bloodborne-android-sync-faults-20261003/gpucomm-watch-content.txt)
- [wake_bench 结果](evidence/bloodborne-android-sync-faults-20261003/wake-bench.txt)、[源码](evidence/bloodborne-android-sync-faults-20261003/wake_bench.cpp)
- [hle_sync 计数](evidence/bloodborne-android-sync-faults-20261003/hle-sync.txt)、[Perfetto 唤醒统计](evidence/bloodborne-android-sync-faults-20261003/perfetto-wakes.txt)、[Guest-1 状态](evidence/bloodborne-android-sync-faults-20261003/perfetto-guest1-states.txt)
- [唤醒代理 A/B 与 spin](evidence/bloodborne-android-sync-faults-20261003/ab-wake-proxy.txt)
- [两项合计 A/B](evidence/bloodborne-android-sync-faults-20261003/ab-all.txt)、[GpuComm 差分与线程放置](evidence/bloodborne-android-sync-faults-20261003/gpucomm-all.txt)
- [ADPF A/B](evidence/bloodborne-android-sync-faults-20261003/ab-perf-hint.txt)
- [缺页分类与 watch_gaps](evidence/bloodborne-android-sync-faults-20261003/ab-fault-kinds-and-gaps.txt)、[watch_decay（温控暂停中）](evidence/bloodborne-android-sync-faults-20261003/ab-decay-paused.txt)
- [核心暂停现场与电量](evidence/bloodborne-android-sync-faults-20261003/thermal-pause.txt)
- [唤醒代理压力测试](evidence/bloodborne-android-sync-faults-20261003/wake-proxy-tests.txt)
- [第二轮：不衰减、线程放置、跨区域延续、流式拷贝](evidence/bloodborne-android-sync-faults-20261003/ab-round2-decay-affinity-bounce-cross.txt)
- [写预测单测与负对照](evidence/bloodborne-android-sync-faults-20261003/tracker-prediction-tests.txt)
- [第三轮：新默认值、guest_affinity、fault_textures](evidence/bloodborne-android-sync-faults-20261003/ab-round3-new-defaults.txt)、[新默认值下的 Perfetto 与 simpleperf](evidence/bloodborne-android-sync-faults-20261003/profile-r3-new-defaults.txt)
- [测量脚本](evidence/bloodborne-android-sync-faults-20261003/tools/README.md)（A/B、线程 CPU、亲和性、Perfetto/simpleperf 分析）
