# Odin3 KH3 性能剖析：内存池提交、平台频率与 AT9 解码失败（2026-10-11）

承接 [loading GPU hang 修复](odin3-kh3-compute-state-20261011.md)。修复后游戏可进入剧情与可操作场景，但帧率很低，CPU 与 GPU 负载都不高。本文记录剖析过程与结论。

## 环境

| 项 | 值 |
|---|---|
| 设备 | AYN Odin3 `6edb0593`，Adreno 830，boot ID `c322cd23-e6c8-46f8-8eb7-ff032d7162db` |
| 构建 | APK `6cdd9e54…`，Turnip `c5a0d6fd990`（驱动 `bfc3af8b…`），`TU_DEBUG=gmem_warmup`，`bind_skip on` |
| 会话 | PID 23372，run UUID `6e91aa93cd640e8c180fb92bf50e133f` |
| 平台状态 | Thermal Status 2，CPU0 约 90 °C；CPU0–5（中核，capacity 792）封顶 2.7456 GHz（标称 3.5328）；CPU6–7（大核，capacity 1024）封顶 3.072 GHz（标称 4.32），core_ctl `active_cpus=0`、`min_cpus=0`，大核基本被挂起；GPU 封顶 967 MHz（`thermal_pwrlevel=2`），实际停在 660–734 MHz（`min_pwrlevel=6`，governor `msm-adreno-tz`） |

## 采集

| 场景 | 文件（`build/validation/odin3-kh3-20261011b/perf/`） | 结果 |
|---|---|---|
| 剧情过场（棋盘） | `kh3-cutscene-10s.prof`（SHA-256 前缀 `34c4555f`），`kh3_sched.pftrace` | 229 帧/10 s，约 22.9 FPS；GPU 每 guest 帧约 25.8 ms |
| “觉醒之所”可操作场景 | `kh3-dive-10s.prof`（`c8f829fb…`），`kh3_sched2.pftrace` | 87 帧/10 s，约 8.7 FPS，帧长均值 115 ms；GPU 每 guest 帧约 23 ms |

LiteP 以 `profiler_capture file` 采集、`index_trace` 建索引、`analyze_frame_budget` 分析；调度用无 root 的 Perfetto（sched_switch/waking、cpu_frequency），trace_processor 查询。帧以 guest `sceGnmSubmitDone` 为界。

## 结论

### 1. 内存池提交占游戏线程半帧，并堵住 GPU 命令线程

“觉醒之所”中最忙的 guest 线程 Guest-57（tid 23714，调度追踪中 10 s 在 CPU 上 6.46 s）每帧：

| 项 | 次数/帧 | 耗时/帧 |
|---|---:|---:|
| `sceKernelMemoryPoolCommit` | 11.1 | 60.6 ms（含下列） |
| ├ `Memory.WriteHold.PoolCommit`（持有 MemoryManager 写锁） | 11.1 | 58.2 ms |
| └ `VM.DataMapping`（Android 宿主映射更新） | 494 | 56.1 ms |
| 条件变量/同步等待（`Sync.Park`） | 18.8 | 15.0 ms |
| `scePthreadGetspecific` | 2144 | 0.36 ms |

同一时段 GpuComm 每帧有 9.8 次 `Memory.LockWait`、共 10.6 ms，正是在等这把写锁。

原因：`MemoryManager::PoolCommit` 把一次提交按物理池里的空闲区逐块映射（PS4 允许稀疏物理映射），每块各做一次 `impl.Map` 与一次 `impl.Protect`。池在反复提交/释放后碎片化，一次提交被拆成约 22 块、约 45 次映射更新；Android 上每次更新约 0.11 ms，且全程持写锁。

修改（`src/core/memory.cpp`）：
- 优先选能容纳整次提交的最小空闲池区（best-fit），没有时才按原顺序拼接稀疏区；
- 请求权限正好是读写时不再紧跟 `Protect`（桌面与 Android 的 `Map` 本身就是读写，Android 对新映射的 `Protect` 不涉及 GPU 监视位）。

效果见下文“修改验证”。

### 2. 平台：大核被挂起、频率受限

两份调度追踪里游戏所有线程几乎只在 CPU0–5 上运行（过场 10 s 中 CPU6/7 无游戏线程；可操作场景 CPU6/7 各约 0.9 s 忙）。线程亲和性为 0–7、top-app cpuset 为 0–7，限制来自 core_ctl：大核簇 `min_cpus=0`，单线程利用率不到 70%，core_ctl 判断不需要大核。GPU 忙碌 65–78% 时 governor 仍停在 660 MHz。这部分属设备性能策略，可用已安装的 ClusterTune 调整（例如提高大核簇 `min_cpus`）后再做对照；本文未改动设备设置。

### 3. 唤醒代理在无小核的芯片上占用游戏核

`wake_proxy` 设计为放在最低容量簇（如 A510 小核）上自旋。Odin3 没有小核，最低容量簇就是承载游戏的 6 个中核（`placement: cpus 0,1,2,3,4,5`），Waker 线程在过场中占 CPU 约 40%，会话累计自旋约 222 s。曾做开/关交替测量，但测量期间画面切到下一段 loading，数据跨场景作废，需在稳定场景重测后再决定是否对这类拓扑默认关闭或缩短自旋。

### 4. 线程间高频唤醒

可操作场景 10 s 内 Guest-1/44/51/57 两两互相唤醒约 7,000–12,000 次（平均每次只睡 65–155 µs），是游戏任务系统的锁/条件变量交接。Guest-57 每 500 ms 约 3.4 万次 HLE 调用，主要是 `scePthreadGetspecific`（约 2,100 次/帧）与 `scePthreadGetthreadid`（约 620 次/帧）；单次宿主侧开销很小，但每次都要退出、重入 JIT。可考虑像 mutex 快路径一样把 TLS 查询放进 guest 侧。

### 5. AT9 解码持续失败

宿主日志中 `Atrac9Decode failed ret = -0x7e000000`（`0x82000000`，`ERR_UNPACK_SUPERFRAME_FLAG_INVALID`）从 20:20:52 进入新游戏时开始：loading 期间约 5,300 次/秒，过场期间几乎没有，可操作场景约 2,000 次/秒；日志已写满 100 MB 轮转。该错误表示解码器句柄内的“帧在 superframe 中的序号”与码流的首帧标志不一致。游戏音频线程 Guest-51 被 AJM 线程唤醒约 14,000 次/10 s，即每次提交立即失败、再次提交。这会造成音频异常并消耗 CPU（AJM 线程、日志线程与音频线程），需单独定位输入与 superframe 进度。

### 6. 其它

- GpuComm 每帧 `Texture.Stage` 约 11 ms（12 次）、`PM4.IdleWait` 约 12.5 ms。
- 状态层重绘每次约 1.3–1.5 ms GPU。
- 本次 `hle_sync detail` 未产生等待链（通知边需在启动时打开 `profile_sync`）。

## 修改验证

APK SHA-256 `84d8c605cbf267b257b1d2437f3625a977ae1fc7258db87d3fb64074a36a87f6`（Turnip 仍为 `c5a0d6fd990`，只多了 `memory.cpp` 的修改）。

- 第一次启动（PID 28009）在开场片段遇到 B 类保护写错误（fault 52，`CP Protected mode error | WRITE | addr=0x00000`，IB2 `0x418F9F6E88`、页内偏移 `0xe88`、ctx 6 / ts 5593），签名与修改前多次记录的 B 类一致，与本修改无关；证据在 `perf-pool/fault52/`（snapshot SHA-256 前缀 `f3be327d`）。
- 第二次启动（PID 28627，run UUID `c866d20b723b5027e51df22b32cd82cc`）：新游戏 → loading → 跳过开场事件 → “觉醒之所”教学画面（与修改前同一画面、Sora 站立、提示“推動 L 可移動索拉”）。

| 同一画面 | 修改前（PID 23372） | 修改后（PID 28627） |
|---|---|---|
| 状态层 FPS | 8.6 | 28.0–31.0 |
| guest flip 速率 | — | 28.9–29.3/s |
| GPU 忙碌 / 频率 | 31% / 660 MHz | 71–77% / 734–900 MHz |
| CPU（状态层） | 27% | 46–52% |

GPU 计时：修改后每 guest 帧 GPU 约 22–24 ms，GPU 忙碌升到 70% 以上，瓶颈开始转向 GPU 与平台频率。

### LiteP 分解（修改后）

`perf-pool/kh3-dive-poolfix-10s.prof`（PROF SHA-256 前缀 `d1a84fa4`，索引 `dcec544f689841909af9de0ce414c4f2`）：9.99 s 内 294 个 guest 提交周期（293 个完整），即 29.4 /s，帧长均值 34.05 ms（p90 36.0 ms）；0 个丢弃 chunk，有若干线程边界未闭合 scope 的诊断（保留）。

Guest-57（tid 28937）每帧：

| 项 | 修改前 | 修改后 |
|---|---:|---:|
| `sceKernelMemoryPoolCommit` 次数 / 耗时 | 11.1 / 60.6 ms | 11.0 / 4.41 ms |
| ├ `Memory.WriteHold.PoolCommit` | 58.2 ms | 2.50 ms |
| ├ `VM.DataMapping` 次数 / 耗时 | 494 / 56.1 ms | 11 / 1.28 ms |
| └ `Memory.LockWait` | — | 0.94 ms |
| `pthread_cond_wait` | — | 2.71 ms（9.0 次） |
| `shadSyncWait` | — | 1.80 ms（3.8 次） |
| HLE 合计 / 次数 | — | 6.88 ms / 3,568 次（`scePthreadGetspecific` 2,550 次 0.41 ms，`scePthreadGetthreadid` 662 次，rwlock 读锁/解锁各 120 次 0.91 ms） |

GpuComm（tid 28789）每帧有标注的时间 23.6 ms（帧长 34.05 ms）：

| 项 | 次数/帧 | 耗时/帧 |
|---|---:|---:|
| `Texture.Stage`（纹理重传时 `ObtainBufferForImage`，含从 guest 内存拷贝） | 12 | 10.26 ms（独占） |
| `Rasterizer.Draw` | 226 | 13.49 ms（含子项；独占 6.26 ms） |
| `Rasterizer.Dispatch` | 51 | 6.02 ms（含子项） |
| `Memory.LockWait` | 13.6 | 2.53 ms |
| `PM4.IdleWait` | 5.1 | 2.41 ms |

池提交不再是瓶颈；GpuComm 最大的单项变成每帧 12 次纹理重传。

### 纹理重传诊断（`upload_diag`，同一会话）

39 帧内 35 张图像共 461 次上传（11.8 次/帧），按内容哈希：395 次与上次相同、31 次变化、35 次首次。

- 两张 32×32×32 层 A2B10G10R10 体纹理（128 KiB，`0x2007680000`、`0x2007700000`）约每帧各一次，75/76 次内容未变。
- 约 25 张 1600×900 的 R16G16B16A16Sfloat（12.8 MB）/ B10G11R11（6.4 MB）tile14 目标（地址 `0x202c000000`–`0x2077000000`），每张约 3 帧重传一次，大多内容未变，`last_writer` 为空。

每帧约 10 张 6.4–12.8 MB 的整图从 guest 内存重传，正对应 `Texture.Stage` 的 10 ms。这些 1600×900 图像看起来是渲染目标，内容由 GPU 产生，guest 内存一直没被 CPU 改写，所以哈希不变。推测：游戏每帧约 11 次 pool commit/decommit 覆盖了这些目标所在的池内存，`PoolCommit`/`PoolDecommit` 都会调用 `TextureCache::UnmapMemory` 释放区内图像，下次绑定时重建并从 guest 内存整图上传——上传既浪费，也可能用旧的 guest 内容覆盖 GPU 渲染结果。地址是否落在池提交区尚未核实（准备用 `memory_describe` 查时会话已崩溃，见下节），这是下一项要确认和优化的点。

### 会话在 21:00:21 闪退（fault 53）

同一会话（PID 28627）运行约 19 分钟后闪退，证据在 `perf-pool/crash-arena/`（宿主日志、crash buffer、`fault53/` 下 dmesg 与 snapshot，snapshot SHA-256 前缀 `7b2653a1`）：

1. KGSL 先报 GPU 故障（dmesg 171360.077 s，fault 52 → 53）：`CP DDE BR opcode error | opcode=0x00000000`，ctx 6 / ts 195670，`status 00880005`，BR `ib1 0x425F88B000` 剩余 0x4b56、`ib2 0x426BA05644` 剩余 0x317，BV 空闲。
2. 宿主随后在稀疏绑定提交上收到 DeviceLost：Turnip `tu_knl_kgsl.cc:1797: bind submit failed: Protocol error`，`BufferCache::SubmitPendingArenaBinds` 断言 `vkQueueBindSparse failed: ErrorDeviceLost`，Present 线程 `PrepareLastFrame` 断言 “Device lost during waiting for a frame”，进程 abort。

所以闪退的起因是 GPU 故障，两处宿主断言是 DeviceLost 之后的后果（按此前决定不修 DeviceLost 后的宿主崩溃）。

snapshot 中 BR 的 IB2（2118 dword，已取 1327）在第 1301 dword 处是一条 11 组的 `CP_SET_DRAW_STATE`（组 0 起为管线 draw state，地址 `0x433182ae80` 等），前一条是 `CP_DRAW_INDX_OFFSET`。这些 draw-state 载荷不在 snapshot 冻结的内存范围内，无法核对 DDE 实际读到的内容。

snapshot 解析的边界：容器完整（1205 个 section，有 END），但 gpu-snapshot 工具对该芯片（`0x44050000`）没有已验证的 PM4 组合解码 profile，MVC 游标寄存器映射为 semantic-only，ROQ 补偿未验证（BR ROQ IB2 原始值 `0x02000080`），工具本身不给出根因。能确定的只有：故障类别来自 dmesg；MVC 的 BR IB1/IB2 地址与剩余量和 dmesg 逐项一致；IB2 已取到第 1327 dword。由于 ROQ 预取，真正执行的包可能早于 1301 处的 `CP_SET_DRAW_STATE`；本次没有用固件 PC 定位 DDE 实际执行点（上一次的固件反汇编证据在 `build/validation/odin3-kh3-gpu-hang-20261010/`，不在本机）。故障环（第三条抢占记录，rptr 7048 = dmesg 的 `0x1b88`）的 `info=0x80000091`，其它环为 `0x80000000`，含义未核实；Swan 上三次同类故障都发生在 RB2 抢占恢复后 80–115 µs，这是下一步要对照的线索。**根因未定位。** snapshot、dmesg、宿主日志与解码结果（连同 fault 52）已整理到 [`bugs/odin3-kh3-cp-dde-opcode-20261011/`](../../../bugs/odin3-kh3-cp-dde-opcode-20261011/README.md)，附待确认清单。签名与 Swan 上未解决的 B 类（BR 停在 `SET_DRAW_STATE` 等 DDE，DDE 读到全零包头）以及 Odin3 10-08 记录的一次 `CP DDE BR opcode=0` 相同，不是本轮池提交修改引入的新问题；当时 `upload_diag` 正在记录，它只做统计。B 类故障未修复，修改前后都会出现。

本轮会话日志中没有出现 AT9 解码失败（修改前那轮从 loading 起每秒数千次）。只有一个样本，不能断定与本修改有关；若属实，可能是音频流数据放在池提交内存中、旧的稀疏映射使读到的内容不一致。需多轮复现确认。
