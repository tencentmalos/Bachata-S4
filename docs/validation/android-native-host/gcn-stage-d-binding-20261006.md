# GCN v2 阶段 D：Android 回放测量、stream 读锁合并、纹理绑定快路径（2026-10-06）

设备 AYN Thor（`9c2841a4`，Turnip），血源 30 帧回放 `bb_thor30`（Android 抓取，11220 个事件）。所有数据都是
GpuComm（命令处理线程）的 CPU 时间：`steady_command_cpu_ms` 为每帧 flip 到 flip 的线程 CPU 减去应用录制写入的时间，
跳过前四分之一的帧（22 帧）。每轮都是新进程。回放是确定的：同一个包的计数器每轮完全相同。

## 1. 测量工具

- **回放报告**：`setprop debug.shadps4.gpu_replay_report "<DebugBus 命令;...>"`。第 8 次 flip
  （`debug.shadps4.gpu_replay_report_flip`）时写 `replay_report_start.txt`，最后一个事件之后写
  `replay_report.txt`；两份相减即稳态每帧计数。命令在命令处理线程上执行，耗时计入“应用写入”，不进命令时间。
  结束时先由命令处理线程取一次 `gpu_memory` 快照（之后不再有 draw 发布快照）。
- `Player::SetFlipCallback`：每次录制的 flip 回调（上面的报告用它）。

## 2. 稳态每帧负载（bb_thor30）

| 项 | 每帧 |
|---|---|
| draw（attachment_draws） | 1531 |
| render pass | 189 |
| stream 小拷贝 | 9734 次，7.4 MB（≤1K 的占多数） |
| 常驻 buffer 上传（arena_uploads） | 24 次，5.3 MB |
| 写缺页（回放模拟） | 约 50 |
| 纹理上传 | 0（剖析里的临时图创建全在前几帧） |
| 缓存命中的采样纹理绑定 | 7920 |

## 3. 不改默认值的实验

| 实验 | 结果 |
|---|---|
| `upload_diag stream_max` 16K / 0（全部常驻）/ 1K，各 3 轮 | 每轮 16K 都最低：均值约 42.3 / 44.2 / 44.7 ms |
| `upload_diag watch_stream on`，3 轮 | 写缺页 49.6 → 49.0 次/帧，提升 164 页，命令时间在噪声内 |
| stream buffer 用 host-cached 内存（`debug.shadps4.stream_memory cached`），3 轮 | 与写合并内存无差别 |
| SRT 走表门控（只测量，`srt_gate on`） | 见第 4 节 |

## 4. SRT 走表门控：不做

测量版仍然每次都走表，同时模拟门控：同一 permutation、同一 user data 之前走过，且那次读到的页之后没被写过，
就算“可跳过”，并用真实输出核对。

| 稳态每帧 | 值 |
|---|---|
| 走表次数 | 3378（约 2.2 次/draw） |
| （permutation，user data）之前出现过 | 9.9% |
| 可跳过 | 7.7%，错误跳过 0 |
| user data 与该 permutation 上一次相同 | 4.8% |
| 走表读到的 Guest 页 | 777，其中被 CPU 写的 7.7 |

表内容基本不变，变的是 user data：几乎每个 draw 指向不同的表。走表约占 GpuComm 采样的 4.5%，门控上限
7.7% × 4.5% ≈ 0.35%，不实现。测量代码（`src/shader_recompiler/srt_gate_diag.*`，DebugBus `srt_gate on|off|status|reset`）
保留，默认关。

## 5. stream 小拷贝合并读锁（`upload_diag stream_lease`，默认开）

每次 stream 拷贝原先各自取放一次 MemoryManager 读锁（每 draw 约 6.4 次）。现在一个 draw 从 `BindResources`
到 `BindIndexBuffer`、以及每次 `BindResources`（dispatch、间接 draw）共用一次读锁（`BufferCache::BeginStreamReads`，
计数嵌套；`MemoryManager::LockReads`）。绑定过程中不会取 VM 写锁（写锁只在 Guest 的内存系统调用里），
读锁对读者可重入，不引入新的死锁形式；写者每个 draw 多等一段绑定时间。

4 轮交替：关 42.96 / 42.12 / 43.07 / 43.42，开 40.58 / 41.90 / 41.31 / 43.18 ms，均值 42.89 → 41.74（−2.7%），
4 对都是开更低。

## 6. 纹理绑定快路径（`upload_diag texture_bind_fast`，默认开）

剖析（同一回放，行级）中 `BindTextures` 的主要开销：`FindView` 读 backing 的缓存未命中、`UpdateImage`
（加锁、跟踪、LRU、刷新检查）、每次都算一遍屏障的 `Transit`，以及 T# 缓存的 robin map 存放整份
`ImageBindingInfo`。改动：

- T# 缓存的 map 只存下标，条目放在旁边的 vector（`texture_bind_entries`），查找走小桶；清空时 epoch 加一，
  本次绑定中途被清空则第二遍不用快路径。
- 条目记下上次完整绑定后的 image、view、layout，以及三个代际：纹理缓存的 `ContentGeneration`（任何
  image 被失效或取消跟踪时加一）、image 的 `backing_epoch`（backing 被替换时加一）、`state_version`
  （`GetBarriers` 实际改变布局状态或产生屏障时加一）。
- 第二遍里，采样（非 storage、非渲染目标反馈/general）绑定在三者都没变时直接用缓存的 view 和 layout，
  跳过 `FindTexture`（刷新、view 查找）和 `Transit`。每 tick 一次的 `TouchFoundTexture`（LRU、使用观察）不变，
  `StageAccess` 与缺失内容跟踪照常。

正确性：Android 回放逐事件图像哈希（7648 行）与帧哈希，关、开、开三次全同；桌面（RX 7600M XT）回放
bb-final 开、关两次与 10-04/10-05 的基线全同（帧与 2210 行逐事件哈希）。

命中：稳态每帧 7920 次缓存命中中 7814 次走快路径（98.7%）。4 轮交替：关 41.82 / 42.01 / 42.52 / 41.48，
开 40.62 / 40.04 / 40.57 / 40.39 ms，均值 41.96 → 40.40（−3.7%），4 对都是开更低。`BindTextures` 采样
约 522 → 381；剩下的主要是 map 查找的桶未命中和每 tick 一次的 `TouchFoundTexture`。

## 7. stream buffer 映射预缺页（默认开，`debug.shadps4.stream_prefault=0` 关）

`simpleperf -e page-faults` 显示回放稳态 GpuComm 每帧约 3300 次缺页，几乎全在读写 stream 拷贝与上传的
`memcpy` 里：驱动按页懒映射 buffer 内存，128 MiB 的 stream 环第一圈每 4 KiB 写一次就缺页一次。
`StreamBuffer` 构造时用 `MADV_POPULATE_WRITE`（失败则逐页写一次）把映射填满，stream 拷贝的缺页消失
（窗口内 72.7k → 30.9k）。4 轮交替（同一包，属性切换）：关 38.86 / 40.04 / 41.12 / 42.45，开 37.40 / 39.12 /
38.25 / 40.97 ms，均值 40.62 → 38.93（−4.2%），4 对都是开更低。

实际游戏里环每约 17 帧绕一圈，之后同一页不再缺页，所以这项主要省开局（和环增长时）的缺页；回放只有
30 帧，正好落在第一圈里，收益比实际游戏大。剩下的缺页在上传 `memcpy` 里，源（Guest 页）与目的
（暂存环块，构造时已预缺页）各约一半；回放时 kswapd 扫描很重（`pgscan_kswapd` 约 86k/s），推测是页老化
后的重新缺页，与回放每帧写约 300 MB 增量页有关。回放写页后对 Guest 地址 `MADV_POPULATE_READ` 不减少
这些缺页，已撤回；实际游戏中是否同样存在未测。

## 8. 其他

- ARM 上 `RefreshFlatBuf` 不再先把整个 flat buffer 清零（与 x86 一致，只 `resize`）：用户数据之后的每个
  槽都由走表（读失败写 0）或动态图像表写入。回放逐事件哈希与帧哈希不变；计时无可测差别。

## 9. 实际游戏（Thor，APK 2b6442a3，血源大教堂区站立不动，30 FPS 上限，内部分辨率 0.5）

每窗口 10 s，按 /proc 线程 CPU 时间除以 guest flip 数：

| | FPS | GpuComm ms/帧 | Guest-1 ms/帧 | 进程 CPU ms/帧 |
|---|---|---|---|---|
| 本页两项开（3 窗口） | 27.89 / 28.10 / 27.49 | 25.79 / 26.70 / 27.00 | 23.42 / 22.74 / 23.90 | 136.9 / 137.1 / 143.5 |
| 两项关（`upload_diag stream_lease off`、`texture_bind_fast off`，3 窗口） | 27.59 / 27.92 / 27.19 | 28.67 / 29.14 / 29.44 | 23.94 / 23.91 / 24.32 | 143.1 / 147.1 / 149.4 |

开关交替进行，每对都是开更低：GpuComm 均值 29.08 → 26.50 ms/帧（−2.6 ms，−9%），进程 CPU 146.5 → 139.2 ms/帧。
GPU 在 680 MHz 下 99% 忙，这个场景受 GPU 限制（约 28 FPS，未到 30 上限），所以 FPS 只高约 0.3。
实际游戏中 GpuComm 每帧缺页不到 6 次，回放里上传 `memcpy` 的缺页确是回放特有的；stream 预缺页在实际游戏中
只影响开局，未单独测。其余线程：唤醒代理 `shadPS4:Waker` 约 11.5 ms/帧，`VkRecord` 约 8 ms/帧。

## 10. 未做

- 全局描述符 heap + push 索引（spec 3.3 第 2 项完整版）：需要改写 SPIR-V 后端全部图像/缓冲访问；
  GpuComm 上能省的 view 查找与描述符记录已大部分由第 6 节拿到，push descriptor 本身在录制线程上执行。
- 桌面帧率未测；stream 预缺页在实际游戏中的收益未单独测。
