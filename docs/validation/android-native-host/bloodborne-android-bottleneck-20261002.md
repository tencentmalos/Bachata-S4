# 血源 Android 瓶颈深入分析（AYANEO Pocket DS，2026-10-02）

接[桌面瓶颈报告](bloodborne-desktop-bottleneck-20261002.md)第 10 节的首轮 Android 实测。本轮补齐调度、采样和缓冲区计数，回答 Android 上每帧的时间花在哪里，并实施了其中最直接的一项（ImGui 缓冲常驻映射）。

## 现场

- **设备**：AYANEO Pocket DS（`01108YHE01017563`，SG8275 / Adreno 740，Android 13 user 版，无 root）。
- **构建**：APK `118345b9…`（源码 `397c40551`，含 XR 影院贴图），host `590594e5…`，Turnip `01a3548f`。
- **会话**：PID 30356，generation 1，run `c6e085e3…`。
- **场景**：读档后的中央亚楠起点，角色静止，每帧约 930 draw，Internal Scale 0.5（默认），状态叠加层开。帧率 25.1–26.9 FPS。
- **采集**（同一会话、同一场景，各自 10 秒左右的窗口，彼此不同时）：
  - **Perfetto**（无 root）：`sched_switch` / `sched_waking` / `sched_blocked_reason` / `cpu_frequency`，9.82 s；
  - **simpleperf**：`cpu-cycles` 2000 Hz，帧指针调用栈，9.96 s，107,828 个样本，0 丢失；
  - **缓冲区计数**：`gpu_memory` 两次快照之差，11.2 s，285 帧；
  - **线程 CPU**：`/proc/<pid>/task/*/stat`，15 s 窗口。
- **方法注意**：
  - 绝对时间取自 sched 和 /proc，simpleperf 只用来给比例。simpleperf 按样本数折算的时间偏高，例如 Guest-21 按样本约 11 ms/帧，/proc 实际约 7 ms/帧。
  - 内核符号受 `kptr_restrict` 限制无法解析，内核内部的具体位置只能推断。
  - 原始 trace（64 MB）和 perf.data（16 MB）只保存在本地 `build/validation/bb-pds-20261002/`，设备副本核对 SHA 后已删除。

## 1. 结论

- **帧长与上限**：帧长 37.2 ms（26.9 FPS），距 30 帧上限差约 4 ms/帧。
- **关键路径**：游戏主线程 **Guest-1**。GpuComm 约 55% 忙，GPU 81% 忙（680 MHz，没有降频），都还有余量。
  - GPU 每帧约 30 ms，即使 CPU 变快，最多也只到 33 FPS 左右，60 帧不可行。
- **温控**：设备处于严重温控（Thermal Status 3，机身 65.4 °C）。
  - 大核（cpu3–6）全程顶在 1.786 GHz，只有标称 2.803 GHz 的 64%；超大核（cpu7）平均 1.27 GHz，上限 1.594 GHz。
  - 游戏是 CPU 瓶颈，这部分降频直接压低帧率，是当前最大的单一环境因素。
- **模拟器自身开销**，最大的两项都属于“系统调用 + 内核”路径，而不是 JIT 代码：
  - **GPU 写跟踪缺页**：全进程约 15 ms CPU/帧，平均每次缺页约 50 µs，其中 mprotect 每次约 22 µs；
  - **HLE 同步唤醒（futex）**：全进程约 13 ms CPU/帧，主线程每次唤醒约 17 µs。
- **主线程等 worker**：主线程每帧约 7.4 ms 在等 worker，其中一半以上落在“worker 在小核上跑”和“worker 自己也在处理缺页”这两件事上。

## 2. 主线程 Guest-1 的帧预算

数据为 Perfetto 9.82 s、约 263 帧的每帧平均值。

| 状态 | ms/帧 | 说明 |
|---|---|---|
| 运行 | 26.4 | 85% 在 cpu4（大核） |
| 可运行排队 | 2.3 | 约 105 次/帧，平均 22 µs；被抢占（R+）只有 109 次/9.8 s，可忽略 |
| 睡眠 | 7.4 | 约 50 次/帧，59% 不到 50 µs 就被叫醒 |
| 不可中断等待（D） | 1.1 | 约 21 次/帧，平均 54 µs |

**运行 26.4 ms 的组成**（simpleperf 比例 × 26.4）：

- **游戏 JIT 代码**：65%，约 17.2 ms。
  - 其中两段热点（各在一个 4 KiB JIT 页内）约占 15%，约 2.6 ms。是否为游戏自己的自旋等待未确认：目前没有把 JIT 地址映射回 guest 地址的工具。
- **同步唤醒的 futex 系统调用**：12.5%，约 3.3 ms。
  - 主线程每帧发起约 195 次唤醒，平均约 17 µs/次。
  - 来源：`sceKernelSignalSema` 约 53 次/帧，`pthread_cond_broadcast` 约 49 次/帧。PROF 中两者分别为每次 35 µs 和 24 µs，那是含唤醒在内的整个调用。
  - 17 µs 一次的唤醒远高于常见的几 µs。内核符号不可见，无法区分是 WALT 选核、IPI，还是在等被唤醒线程完成切换。
- **写保护缺页**：9.6%，约 2.5 ms，其中 mprotect 系统调用占 4.9%。
- **其余 HLE / FEX 调度**：12.9%，约 3.4 ms。

**睡眠 7.4 ms 在等谁**（按唤醒它的线程分组）：

- **Guest-54..59**：3.2 ms，6 次/帧，平均 0.54 ms。这组线程 25% 的 CPU 花在 rwlock HLE 的 futex 上。
- **Guest-32..36**：2.8 ms，26 次/帧，平均 0.11 ms。信号量任务。
- **其余**：约 1.4 ms。

**D 状态 1.1 ms**：93% 由 Guest-21..25 释放。这五个线程正是 mprotect 调用最多的线程，所以推断为 `mmap_lock` 争用（mprotect 持写锁，缺页持读锁）。没有 root，无法从内核函数上确认。

## 3. 全进程开销

**线程 CPU**（/proc，旧版 APK，25.1 FPS）：

| 线程 | ms/帧 |
|---|---|
| Guest-1 | 27.2 |
| GpuComm | 21.7 |
| Guest-17 | 13.1 |
| VkRecorder | 9.2 |
| Guest-20 | 8.0 |
| Guest-21..25 | 各约 7.0 |
| Present | 4.5 |
| Guest-32..36 | 各 3.1–3.7 |
| Guest-54..59 | 各 2.4–3.1 |

进程合计 4.45 个核。

### 3.1 GPU 写跟踪缺页

- **每帧计数**：
  - 写缺页处理 296.5 次，解除保护 937.5 页（其中 `watch_predict` 预放 634.5 页）；
  - 重新保护 56.8 次 / 1,188 页；
  - mprotect 353 次；
  - 脏页上传到 arena 27.7 次 / 3.8 MB。
- **CPU 开销**：
  - Guest-21..25 约 31% 的 CPU，约 10.8 ms/帧，其中 mprotect 内核约 5.4 ms；
  - 主线程约 2.5 ms，GpuComm 的 `ProtectGpu` 0.8 ms，其余约 0.5 ms；
  - 全进程合计约 15 ms/帧，相当于每次缺页约 50 µs、每次 mprotect 约 22 µs。
- **调用链**：`SignalDispatch::DispatchAccessViolation` → `Rasterizer::InvalidateMemoryFromWriteFault` → `BufferCache` → `RegionManager::MarkWriteFault` → `PageManager::UpdatePageWatchers` → `MemoryManager::ProtectGpu` → `mprotect`。
- **对帧长的影响**：主线程直接承担约 2.5 ms，加上 D 状态 1.1 ms，还要等因此变慢的 worker。

### 3.2 HLE 同步唤醒

- **每个线程发起的唤醒次数**（9.82 s）：
  - Guest-1：51,394 次，约 195 次/帧；
  - GpuComm：11,656 次；
  - Guest-21..25：各约 7.8k–8.2k 次；
  - Guest-32..36：各约 3.8k–4.3k 次。
- **futex 内核时间**（ms/帧）：
  - Guest-54..59 4.2（rwlock HLE）；
  - 主线程 3.3；
  - Guest-32..36 2.3（`GuestKernelSemaphore::DeferredWakes`）；
  - Guest-21..25 1.5；
  - GpuComm 0.6；
  - 其余 0.6；
  - 合计约 12.6。
- **worker 睡眠很短**：不到 50 µs 就被叫醒的比例为 Guest-21..25 30%、Guest-32..36 17%、Guest-54..59 49%。
- **worker 在小核上运行的比例**：Guest-21..25 23%，Guest-32..36 50%，Guest-54..59 43%。主线程等得最久的正是后两组。

### 3.3 GpuComm（约 21 ms/帧，约 55% 忙，目前不在关键路径）

- **总体**：`Rasterizer::Draw` 74%，`DispatchDirect` 17%。
- **`BufferCache::ObtainBuffer` 26%，约 5.2 ms**：
  - **stream 小拷贝**：4,895 次/帧，共 4.0 MB，其中 82% 小于 1 KiB。
    - memcpy 约 0.84 ms。
    - `CopySparseMemory` 释放 MemoryManager 读锁的那次原子操作约 1.0 ms（`__aarch64_ldadd4_acq_rel`）。
    - 推断原因：stream buffer 用的是 uncached 内存类型 type0；读锁释放带 release 语义，要等前面写入 uncached 内存的数据排空。
    - Turnip 还提供 HOST_CACHED|HOST_COHERENT 内存（type1），目前没用于 stream buffer。
  - `SynchronizeMemory` 约 2.3 ms。
- **管线查找** `GetGraphicsPipeline` 20%，约 4.2 ms：`RefreshGraphicsKey` → `GetProgram` → `RefreshFlatBuf`。
- **纹理绑定** `BindTextures` 11%，约 2.3 ms。
- **CopyShader HLE** `ExecuteShaderHLE` 13%，约 2.6 ms。

### 3.4 其他线程

- **VkRecorder**（9.2 ms/帧）：Turnip 的 push descriptor（`tu_push_descriptor_set` / `tu_update_descriptor_sets`）占 26%。
- **Guest-17**（13 ms/帧）：几乎全是 JIT，含 AJM 音频解码 0.75 ms，不在关键路径。
- **Present**（4.5 ms/帧）：见第 4 节，已修。

## 4. 已实施：ImGui 顶点/索引缓冲常驻映射

**问题**：
- ImGui Vulkan 后端每帧对顶点/索引缓冲各做一次 `vkMapMemory`、拷贝、`VK_WHOLE_SIZE` flush、`vkUnmapMemory`。
- 在 Turnip 上：
  - 每次 map 是一次 mmap，新映射的页被碰到时都要缺页填充；
  - `vkFlushMappedMemoryRanges` 在 aarch64 上不看内存是否 coherent，一律逐缓存行清（`tu_device.cc` `sync_cache` → `tu_bo_sync_cache` → `util_flush_range`）；
  - 所选内存（type0）本来就是 HOST_COHERENT，按 Vulkan 规范不需要 flush。
- 合计约 2 ms/帧的 Present 线程 CPU。

**修改**：
- 缓冲创建/扩容时映射一次，销毁或扩容前解除映射；
- 优先选 HOST_COHERENT 内存，只有非 coherent 内存才 flush；
- 映射失败时本帧不画 ImGui。
- 两份后端同步修改：
  - 主仓 `src/imgui/renderer/imgui_impl_vulkan.cpp`，用于 2D 画面的 HUD、通知、HLE 对话框；
  - Foundation `third_party/imgui/backends/imgui_impl_vulkan.cpp`，上游 1.92.2b，用于 XR 状态层和错误层，改动处标有 `Foundation:`。

**验证**：
- **构建**：Android host 两次 `HOST_LINK_PASS`，最终 host 为 `678d57ca…`；桌面 clang-cl 构建通过。
- **装机**：Pocket DS 装 APK `0bf9ec92…`（host `99b464f1…`，只含主仓改动）。读档回到同一场景，HUD 绘制正常。
- **Present 线程 CPU**：4.50 → 2.30 ms/帧（11.3% → 6.3% 个核），两次均为 Thermal Status 3、大核 1.786 GHz。
- **simpleperf**（只采 Present 线程，10 s，1,353 个样本）：`util_flush_range`、`MapMemory`、`UnmapMemory`、`mmap`、`munmap` 均为 0。

**边界**：
- Present 线程不在关键路径。两次 FPS（25.1 与 27.6）来自不同会话、不同运行时长，不能算作这项改动的收益。
- Foundation 那份只做了编译检查。Pocket DS 没有 XR，XR 路径未运行；桌面也未做运行检查。
- 两份 ImGui 后端随后已合并为 Foundation 的统一渲染器，见 [ImGui 渲染统一](imgui-renderer-unify-20261003.md)。

## 5. 建议的优化顺序（10-02 时均未实施）

第 1–3 项的实施与实测见 [10-03 写跟踪缺页预测与同步唤醒代理](bloodborne-android-sync-faults-20261003.md)：
- 第 1 项改为按内容哈希维持预放置信度；
- 第 2 项改为唤醒代理线程；
- 第 3 项 ADPF 实测无收益。

1. **GPU 写跟踪缺页**（全进程约 15 ms/帧，主线程直接 + 间接 3–4 ms）。
   - 先补每页的缺页频率统计，确认有多少页每帧都被写。
   - 对“每帧都被 CPU 写”的页停止重新保护，改为 GPU 使用时按内容直接上传：拷 4 KiB 约 1–2 µs，一次缺页约 50 µs。
   - 也要减少 mprotect 次数，它同时引发 `mmap_lock` 争用。
2. **HLE 同步唤醒**（全进程约 13 ms/帧，主线程约 3.3 ms）。
   - 参考 mutex 的 Tier B 做法，给内核信号量 / cond / rwlock 加 guest 侧快路径。
   - 或者等待方先短暂自旋再睡，唤醒方在等待方还没睡下时跳过 futex。
   - 主线程 59% 的睡眠不到 50 µs，worker 有 17–49% 不到 50 µs。
3. **线程放置**：主线程每帧约 6 ms 在等的两组 worker，有 43–50% 的时间跑在小核上。可以评估 ADPF（`APerformanceHint`，API 33 可用）为关键线程提供提示。是否真能改变 WALT 的选核，需要实测。
4. **GpuComm 的 stream 拷贝**：stream buffer 改用 cached coherent 内存（type1），或按 draw 合并 MemoryManager 读锁。预计 1–2 ms/帧，但 GpuComm 目前不在关键路径。
5. **温控**：设备凉的时候和温控后的帧率差，还需要冷启动对照（要先停游戏让设备降温）。AYANEO 的性能模式、风扇设置会改变温控行为，改动前需征得用户同意。

## 6. 边界

- 只测了一个场景（中央亚楠起点，静止）、一台设备，而且全程处于温控状态。
- 各项采集不同时，交叉换算（例如每次唤醒的耗时）是近似值。
- 内核内部原因（`mmap_lock`、唤醒为何慢）是推断，没有 root 无法确认。
- JIT 热点未映射回 guest 代码。
- 游戏在两次装机之间被覆盖安装中断，回到标题后读档。存档按游戏正常流程读写，没有外部修改。

## 证据

- [Perfetto 调度汇总](evidence/bloodborne-android-bottleneck-20261002/perfetto-sched-summary.txt)（唤醒次数、睡眠分布、主线程等待对象、频率）
- [simpleperf 各线程拆分](evidence/bloodborne-android-bottleneck-20261002/simpleperf-threads.txt)
- [缓冲区计数与温控状态](evidence/bloodborne-android-bottleneck-20261002/buffer-cache-and-thermal.txt)
- [/proc 线程 CPU：修改前](evidence/bloodborne-android-bottleneck-20261002/proc-threads-imgui-before.txt)、[修改后](evidence/bloodborne-android-bottleneck-20261002/proc-threads-imgui-after.txt)
