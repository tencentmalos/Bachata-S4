# Swan 血源诊所：合并版复测与 GpuComm 深入归因（2026-09-23）

合并后的 `malos/main` / `feature/malos/swan_performance` 3f0c47ea（APK 30733386 / host 93464ad4，mainline Turnip 86ca472f，Render 0.5 / Texture medium）在 Swan `PB3110PGL6240001G` 上重测诊所，并按 [下一步 spec](../../specs/swan-bloodborne-cpu-gpu-next-20260922.md) P1 第 1 步做同窗 PROF + sched + simpleperf 归因。证据在 [evidence/swan-bloodborne-baseline-20260921/](evidence/swan-bloodborne-baseline-20260921/) 的 `bb-clinic-T-merged/`、`bb-clinic-U-topapp/`。

## 1. 结论

1. **合并版 FPS 与 run S 持平**：同一重指纹（1487 draws/帧）锁频 top-app 状态 **14.21 / 15.03 / 14.74 FPS**（另一会话前段 14.86 / 14.48 / 14.91），run S 为 14.25 / 14.29 / 14.39。快路径 `installed`，无窗口外告警。
2. **帧长 = GpuComm 的 CPU 翻译时间**。逐帧：帧 69.7 ms，`PM4.Resume` 覆盖 68.9 ms，**r = 0.995**；帧 owner（本代为 Guest-20）等提交门 57.7 ms（r = 0.948）；Guest-1 活跃 52.4 ms（r = 0.38）；GPU `GuestCommands` 50.9 ms（r = 0.38）。
3. **GpuComm 不是被阻塞，是算不完**。独立 tracefs sched：GpuComm **92.8% 在 CPU 上**，被抢占可运行 5.4%，唤醒后可运行 1.0%，睡眠 + D 仅 0.8%。spec P1 要求先回答的"68 ms 里有多少是阻塞"= 不到 2%。
4. **GpuComm 每帧约 65 ms on-CPU 的去向**（simpleperf 2000 Hz fp，136,659 样本 0 丢失）：

   | 路径（inclusive，互有嵌套） | 占 GpuComm | ≈ ms/帧 |
   |---|---:|---:|
   | `PipelineCache::GetGraphicsPipeline` | 32.4% | 21.0 |
   | 　└ `Shader::Info::RefreshFlatBuf` → `PortableSrt::Run` → `TryReadSrtMemory` | 23.5% | **15.3** |
   | 　└ `StageSpecialization` 构造（其中 `ParseFetchShader` 57%） | 4.5% | 2.9 |
   | `BufferCache::ObtainBuffer`（`SynchronizeBuffer` 64%、`StreamBuffer::Copy` 29%） | 18.4% | 11.9 |
   | 　└ `PageManager::Protect` → `mprotect`（内核 `change_protection`） | 9.0% | 5.8 |
   | `ExecuteShaderHLE`（copy shader；76% 在 `ObtainBuffer`、20% Turnip `CmdCopyBuffer`） | 13.8% | 9.0 |
   | `Rasterizer::BindTextures` / `BindBuffers` | 8.5% / 8.0% | 5.5 / 5.2 |
   | Turnip 自身（`vulkan.ad07xx.so` self） | 14.3% | 9.3 |
   | PM4 解码（`Liverpool::ProcessGraphics` 等 self） | 8.7% | 5.6 |

   4 核状态（见 §2）下各桶占比与此相差不到 0.5 个百分点，是稳定的工作量结构。
5. **最大单项是 ARM64 的 SRT 解释执行**：每个 draw、每个阶段都要重算扁平化 user data。`PortableSrt::Run` 每读一个 4/8 字节就调用一次 `MemoryManager::TryReadSrtMemory`，每次都拿 `MemoryManager` 的 `std::shared_mutex`（libc++ 实现是 mutex + 条件变量，热点里因此出现 `mutex::lock`、`notify_all`）、做 `IsValidMapping` / `GuestAddressSpace::OwnsRange`、查 VMA 与物理段，最后 memcpy 几个字节；每次 `Run` 还堆分配一个 `std::vector<Memo>`。`RefreshFlatBuf` 的叶子分布：`TryReadSrtMemory` 26%、`IsValidMapping` 13%、mutex lock/unlock 约 24%、`OwnsRange` 5%、Singleton 4%，真正的 memcpy 只有 1.8%。x86 桌面版这里是直接读内存的原生 walker。
6. **写跟踪的来回**：GpuComm 同步/HLE 拷贝时把页重新保护（`mprotect` 5.8 ms/帧），guest 随后写这些页触发 SIGSEGV → `Rasterizer::InvalidateMemory` → 再 `mprotect`（Guest-1 上 7.7% ≈ 2.9 ms/帧，另有信号投递的内核开销）。
7. **Guest-1 约 37 ms/帧 on-CPU**：JIT 72%，其中**一个游戏自旋循环占 Guest-1 的 21%（≈ 7.9 ms/帧）**；反汇编（`jit-64905ace00.dis`）是 guest 0x1021b9967 / 0x1021b99b0：`ldapur` 读 32 位计数器与目标比较，再读全局字节 0x1056a6bf1 作为退出标志，否则回跳——主线程在等 worker 任务计数。它不是此前定位的 `DLAdaptiveMutex`（+0x2037cb0），是新点位。它是"等 worker"的症状，但在 6 核被占满时也和 worker 抢 CPU。
8. **GPU 每帧 50.9 ms**（detail，0 丢弃）：render pass 35.3 ms / 203 个实例、HostReadback 4.8、Dispatch 3.3、HostTransfer 2.5、Present 1.3、Overlay 0.8，与 run S（48.8）一致。
9. **同步 HLE 合计约 1475 次/帧**（rwlock 读锁/解锁各 344、AddrWake 162、Sema.Wait 158、Sema.Signal 108、Mutex.Config 76、AddrWait 65、Cond.Wait/Broadcast 各 54、Usleep 38、Mutex.Init/Destroy 各 27）。

**阶梯**：GpuComm 若降到约 40 ms，下一位限速者是 GPU（51 + present/overlay ≈ 53 ms）与 Guest-1 活跃（约 50 ms，含自旋），帧约 52 ms（≈ 19 FPS）；再往上必须同时压 GPU 与 Guest-1/worker。

## 2. 测量陷阱：Pico launcher 被杀后游戏丢 top-app

第一组 FPS 窗口（14.5–14.9）结束时，设备时间 16:39:19 头显距离传感器抖动，16:39:25–26 Pico `ResManagerKillPolicy multiWindowLmkdHook` 在内存压力下杀掉 launcher（它自身 2.6 GB memtrack，系统 RAM 已用 14.8/15.4 GB），系统重建 home Activity 并把焦点给它；shadPS4 进程从 `/top-app`（CPU 0–5）降到 **`/foreground`（CPU 0–3）**。16:39:35 lmkd HyperHold 开始把本应用 memcg（uid 10152）写入 zram，RSS 2.49 → 1.44 GB、swap 971 → 1063 MB；lmkd 与 kswapd0 各占约 85% 的一个核。

后果（T 轮，锁频、同指纹）：FPS 降到 11.8–12.6；帧 81.2 ms、`PM4.Resume` 79.1 ms（r = 0.964）；sched 显示整个进程只在 CPU 0–3 上运行，这 4 核 96–97% 忙而 CPU4/5 空闲 63–67%，GpuComm 被抢占可运行 16.5%、Guest-1 可运行 33%。之后 swap-in 已停止（10 s 内 majflt +28，PSI 0），掉帧来自核数而非缺页。

**规则**：Swan 上每次采集前后都要记录 `/proc/<pid>/cpuset` 与 `mFocusedApp`，不是 `/top-app` 的窗口不能和 top-app 的比。T 轮的 PROF/sched/simpleperf 只作 4 核状态对照；§1 的数字全部来自 U 轮（cpuset 全程 `/top-app`，见 `cpuset-log.txt`）。

另一个操作教训：为恢复焦点用 `am start -a MAIN -c LAUNCHER -f 0x10200000` 并没有复用原 Activity，而是新建了 MainActivity 实例，旧 Surface 被销毁，会话按 Surface 丢失规则正常取消（`terminal outcome=Cancelled ... guest return=0`，非崩溃），随后重新进诊所做了 U 轮。不要用 `am start` 抢焦点。

## 3. 方法与身份

| 项 | 值 |
|---|---|
| 构建 | 3f0c47ea；APK `307333860ea793ce`；host `93464ad4ce986a6c`（Build ID d9618178…）；JNI Build ID 8569e070… |
| 设备 | Swan PB3110PGL6240001G，boot 0af47cd3；锁频 `swan-evt-legacy`（cpu0–5 1.90 GHz、cpu6–7 1.50 GHz、GPU 726 MHz），操作 6e4792ea…（T）、a2fa32fa…（U） |
| 场景 | 诊所可操作，1487 draws/帧、1237 scaled、257 pass 实例/帧（`break_delta.py`） |
| U 轮 | 会话 generation 2（同进程 pid 26213）；measure 3×10 s → 同窗 sched（独立 tracefs，256 MiB，25 s）+ PROF file 20 s（capture 4）+ simpleperf `-e cpu-clock -f 2000 --call-graph fp` 20 s → detail（`gpu_timing detail` + `hle_sync start 68`，PROF capture 5）→ 关闭后 2×10 s |
| T 轮 | 会话 generation 1；首组窗口后进入 4 核状态；sched 64 MiB（overrun 大，只作参考）与 256 MiB 各一次，PROF capture 1–3，simpleperf 一次 |

锁频收尾：T 轮第一次 stop 时 `gpu_max_clock` 读回 826（写 902），重试后 Cleaned；U 轮 stop 时 `policy0/scaling_max_freq` 已被系统改为 2745600（高于记录的基线 2611200），工具按设计报 Conflict 不覆盖，两次重试结果相同，属于外部改写，未手改 sysfs。两轮锁频记录的 `policy0/scaling_min_freq` 基线都是 1900800（早先锁频残留）。

## 4. 局限

- simpleperf 采样自身有开销（T 轮同状态下带采样 11.8 FPS、不带 12.3 FPS），占比可用、绝对 ms/帧是按 PROF 帧率折算的估计。
- JIT 代码无符号；自旋点靠热页反汇编与 FEX 块元数据里的 guest RIP 认定，未在 IDA 里标注函数。
- U 轮 sched 的 CPU0 有 overrun，分析窗口裁到所有 CPU 都有数据的 19.69 s。
- GPU 时间戳未与 CPU 校准，GPU 只用量级，不做逐事件因果。
- `hle_sync` 每帧计数按 detail 窗口 FPS 归一，窗口含开启后的前几秒。

## 5. 对 spec 的影响

P1 的归因已完成，改为按收益直接实施（见 spec 修订）：

1. **SRT 批量读**（预计 GpuComm −10 ms/帧量级）：`RefreshFlatBuf` 期间只拿一次 `MemoryManager` 共享锁，缓存最近命中的 VMA/物理段，同段内读取只做边界检查 + memcpy；`Run` 的 memo 改为复用缓冲；保持同样的映射/权限校验语义与越界读 0。
2. **写跟踪来回**：对每帧都被 guest 重写、又被 GPU 同步的区间做频率判定，改为不保护、每次按需上传（或延迟重保护），同时削掉 GpuComm 的 `mprotect` 与 Guest-1 的缺页。
3. **HLE copy**：`ExecuteShaderHLE` 的 `ObtainBuffer` 成本单列优化（源/目的区间已同步时跳过）。
4. **每 draw 的查找**：`ParseFetchShader` 按 fetch shader 地址 + 代码哈希缓存；`BindTextures/BindBuffers` 对不变 sharp 走缓存。
5. 测量守卫：采集脚本记录 cpuset/焦点，非 top-app 即判无效。

## 6. 证据文件

`bb-clinic-U-topapp/`：`fps.txt`、`fps-detail-window.txt`、`fps-post-detail.txt`、`cpuset-log.txt`、`snap0/1.txt`、`frame-limiter-results.txt`、`sched-U-states.txt`、`hle-sync-detail-start/end.txt`、`hle-sync-per-frame.txt`、`gpu-timing-status-detail.txt`、`perf-light/perf-buckets.txt`、`subtree-gpucomm.txt`、`subtree-guest1.txt`、`hot-jit-guest1.txt`、`jit-64905ace00.dis`、`threads.txt`。
`bb-clinic-T-merged/`：同名 FPS/快照文件、`sched-T2-states.txt`、`perf-light/perf-buckets.txt`、`subtree-gpucomm.txt`、`launcher-kill-logcat.txt`、`session-end-by-am-start-logcat.txt`、DumpLayer 截图。
PROF（capture 1–5）、`perf.data`、sched 原始 trace 只记 SHA，不入库；工具 `perf_record.sh`、`perf_buckets.py`、`perf_subtree.py`、`perf_hot_ips.py`、`sched_states.py`、`sched-only-capture.sh` 在 `tools/`。

## 7. 复测 V（2026-09-23 下午，含 compute 清屏修复与重传计数）

构建 APK cd795a87 / host 781b9693（3f0c47ea + TMNT 那轮的 compute 填充改清图 + StatusLayer 重传计数，本地未提交），pid 3882 generation 1。`upload_diag status` 显示 `compute_fill: off=0`，即清图默认开启；血源不命中这个内核（`cleared=0`）。用户在头显里操作进入诊所，场景指纹与 T/U 相同（**1487 draws/帧**、245 pass/帧）。锁频 `swan-evt-legacy`，采集前后均 `/top-app`、焦点在游戏；进程有 916 MB 在 swap。

- **FPS**：**13.75 / 13.94 / 13.93**（U 为 14.21 / 15.03 / 14.74；同指纹略低，swap 与场景小差异均可能，未单独归因）。detail 插桩下 13.16。
- **逐帧卡点**（轻量 PROF 280 帧）：帧长 71.3 ms；GpuComm `PM4.Resume` **67.5 ms，r=0.917**；Guest-1 活跃 50.5 ms，r=0.859（U 为 0.379）；owner 等待 33.9 ms。仍然是 GpuComm 翻译算不完，Guest-1 紧随其后。
- **GPU**（detail PROF，203 帧，0 丢块）：GuestCommands 48.7 ms/帧（render pass 32.9、HostReadback 4.95、dispatch 3.0、HostTransfer 2.74、Present 1.23），与 U 的 50.9 ms 相当，约占帧长的 66%。锁频下 `gpu_busy_percentage` 99%，但它统计整颗 GPU，含 Pico 合成器，不能单独说明本进程 GPU 受限。
- **GpuComm on-CPU**（simpleperf 20 s，132,027 样本 0 丢失，约 278 帧）：18.4 s，折合 66.3 ms/帧，各桶占比与 U 几乎相同：`RefreshFlatBuf` 23.5%（15.6 ms，逐次 `TryReadSrtMemory` 的 `IsValidMapping`/mutex/`OwnsRange`，memcpy 1.7%）、`ObtainBuffer` 19.4%（12.9 ms，`SynchronizeBuffer` 的 `mprotect` 与 stream buffer 拷贝）、copy-shader HLE 13.7%（9.1 ms，其中 77% 是 `ObtainBuffer`、20% 是 Turnip `vkCmdCopyBuffer`）、`BindResources` 18.8%。
- **Guest-1 on-CPU**：10.3 s，折合 37.1 ms/帧；JIT 72.9%，两段 JIT 页合计 20.4%（约 7.5 ms/帧），即上一轮定位的游戏自旋循环。
- **纹理重传（StatusLayer 显示为橙色）**：每帧 **8.0 次、15.7 MB**，全部是 GPU storage 写后整张重传，写入者两个，都是现有 compute HLE 差一点没接住的情况：
  - `0x8b355b5a`：16 字节常量清成纯色（上游 `IsComputeImageClear` 的形态）。1080p 整张清除已被接住，但 R16G16F 6 层立方体图（32²/64²/256²）是**逐层清**，buffer 只覆盖单层、起点是图起点加层偏移，不满足"大小与起点等于整张图"而照常 dispatch，整张重传。
  - `0x3d5ebf4e`：逐 dword 拷贝（`IsComputeImageCopy` 的形态）。两张 960×540 RGBA8 来回拷贝时 buffer 比图大 0x1800 字节（尾部补齐）；1080p R32F 的源是 D32S8 深度+模板整块（0xb70000 字节）。两种都因大小不等被拒绝。
  - 代价：`RefreshImage` 只占 GpuComm 的 0.8%（约 0.5 ms/帧），GPU 侧计入 HostTransfer 2.74 ms/帧中的一部分。**不是当前卡点**，但修法明确（逐层清除按子资源 `Image::Clear`；拷贝允许覆盖范围大于图、且超出部分没有其它缓存图），成本低。
- **新代码开销**：`TryComputeImageFill` 在 GpuComm 上只采到 2 个样本。

结论：卡点与 U 轮一致，spec P1 顺序不变——SRT 批量读（约 15.6 ms/帧）→ 写跟踪来回与 stream 拷贝（约 12.9 ms/帧，含 copy HLE 的大部分）→ copy HLE 里的 Turnip 拷贝 → 每 draw 查找缓存；Guest-1 的自旋循环（约 7.5 ms/帧）其次。重传泛化可以顺手做，但对 FPS 的直接收益有限。

证据：`evidence/swan-bloodborne-baseline-20260921/bb-clinic-V/`（`fps.txt`、快照、`cpuset-pre/post.txt`、`gpu-busy-locked.txt`、`gpu-timing-status-detail.txt`、`frame-limiter-results.txt`、`perf/perf-buckets.txt`、`perf/subtree-gpucomm.txt`、`perf/hot-jit-guest1.txt`、`uploads/upload-diag.txt`、`uploads/run-lines.txt`）；PROF capture 1/2 与 `perf.data` 只记 SHA。锁频操作 6f3a5ecc 收尾时 `policy0/scaling_max_freq` 被系统改为 2112000，工具判冲突未覆盖。

## 8. SRT 批量读与重传修正（2026-09-23 晚，run W/X）

### 实现

**SRT 批量读**（参考 Azahar 的 PICA jobs decode：每段范围只解析一次映射、拿 host 指针后本地读；一批只加一次锁；有疑问就回退旧路径；去掉每次调用的堆分配。它把小粒度工作交给工作线程的尝试在其自身测量里没有收益，所以这里不开线程）：

- `MemoryManager::SrtReadBatch`：生命周期内持有映射锁（`SharedFirstMutex` 共享）一次；每张被读的表第一次读取时解析一次，得到以读取地址为中心、64 KiB 对齐、限定在同一 VMA 与同一物理段内的 host 后备窗口（`OwnsGuestRange` 对整窗成立才扩窗，否则只缓存这次读取的范围），最多缓存 4 个窗口、先查最近命中的那个；命中后读取就是范围比较 + memcpy。没有物理后备、跨物理段的读取走原 `TryReadSrtMemory` 路径（在同一把锁下）。
- `Shader::SrtGuestReader` 把这个批量对象放在栈上（不分配），作为 `PortableSrt::Run` 的读取函数在 `Info::RefreshFlatBuf` 里使用；锁只覆盖扁平化本身，不跨编译或任何可能阻塞的工作。第一版用 `thread_local` 找批量对象，每次读取都要走动态 TLS 解析（`tlsdesc_resolver_dynamic` 占扁平化的 19%），已改为直接传入。
- `PortableSrt::Run` 的 memo 改为每线程复用缓冲、epoch 跨运行递增，不再每次 `std::vector` 分配。
- DebugBus `srt_batch status | on | off | verify on|off`：`off` 在同一会话切回逐次读取；`verify on` 对每次批量读取再走一遍旧路径比对，不一致时计数、限量告警并返回旧路径结果。统计每批结束时汇总一次，不在每次读取上做原子操作。

**重传修正**（§7 的两类写入者，均扩展既有的 compute 清图/拷图 HLE，而不是新加特判）：

- `IsComputeImageClear`：整张图条件不满足时，用 `TextureCache::FindImageSlice` 查"范围恰好是某张缓存图的一个（mip, layer）子资源、且没有别的缓存图重叠"，是则只清这一个子资源；图上若有未上传的脏数据先 `UpdateImage` 再清，避免丢掉其它层的内容。立方体贴图的逐面清除因此不再触发整图重传。
- `IsComputeImageCopy`：允许 buffer 比两张同大小的图更长（尾部补齐、深度平面后面的模板平面），图照旧做图对图拷贝，超出部分按内存用 `vkCmdCopyBuffer` 拷贝；前提是目的端的尾部没有任何缓存图（`HasImageInRange`），否则照旧 dispatch。

### 验证

构建 APK 5b170ae1 / host edf0e10d（最终为只改格式的 16a5c32d / 6ed63a7a），pid 28012，诊所约 1500 draws/帧，全程 `/top-app`，锁频 `swan-evt-legacy`。

- **正确性**：血源加载与诊所中 `verify` 比对 **88,350,696 次读取，0 次不一致**；TMNT 屋顶比对 **204,769,592 次，0 次不一致**，60 FPS、画面正常。诊所命中率 97.3%，回退 0。
- **重传**：各轮均为 **0.0 次/帧**（V 轮 8.0 次/帧、15.7 MB），状态层同样显示 0。
- **同会话 A/B**（10 s/轮，交替 8 轮）：off 14.74 / 14.40 / 14.77 / 14.65（均值 **14.64**），on 14.72 / 15.62 / 15.28 / 15.13（均值 **15.19**，+3.7%），轮间波动约 ±0.5 FPS。V 轮（旧 SRT、有重传）为 13.75–13.94。
- **CPU**（simpleperf 15 s、0 丢失，按窗口内 present 数折算）：SRT 扁平化 **13.7 → 3.5 ms/帧**（GpuComm 的 25.0% → 7.7%），GpuComm 合计 54.6 → 45.3 ms/帧。
- **逐帧卡点**（PROF 15 s）：

| | 帧长 | GpuComm `PM4.Resume` | Guest-1 活跃 | owner 等待 |
|---|---|---|---|---|
| off | 68.3 ms | 67.6 ms（r=0.998） | 47.9 ms（r=0.13） | 56.8 ms |
| on | 64.2 ms | 56.4 ms（r=0.88） | 50.6 ms（r=0.88） | 28.9 ms |

GpuComm 每帧少了约 11 ms，但帧长只少约 4 ms：批量读关闭时帧长几乎完全跟随 GpuComm；打开后 GpuComm 与 Guest-1（游戏主线程约 50 ms 活跃）一起决定帧长，两者没有完全并行。

### 结论与下一步

- SRT 批量读本身达到目标（扁平化 -75%），FPS 受益有限是因为瓶颈已变成 GpuComm 与 Guest-1 双线。
- GpuComm 剩余大头：`ObtainBuffer` 23%（写跟踪 `mprotect` 与 stream 拷贝，copy HLE 的主要成本也在这里）、`BindResources` 27%、`GetGraphicsPipeline` 其余部分。
- Guest-1：约 50 ms/帧活跃，其中游戏自旋约 7.5 ms/帧（§7），其余是 JIT 代码与同步 HLE。
- 要继续提帧需要两条线一起压：GpuComm 的写跟踪来回（spec P1 下一项），以及 Guest-1 的自旋与同步 HLE（spec P2）。

### 过程记录

- W 轮：加载中内存压力（系统 RAM 约 14.9/15.4 GB）让 Pico launcher 被杀重启并抢焦点，游戏降到 `/foreground`。这时第一组 A/B 因 `verify off` 命令未生效而作废；`verify off` 生效后的 A/B 为 off 16.36/16.65、on 16.87/17.33（未锁频、`/foreground`，只作参考）。
- X 轮第一次 A/B 最后一轮是 off，紧接的 PROF capture 1 与 simpleperf 因此是 off 状态，已改作 off 对照（证据目录 `bb-clinic-X/perf-off/` 附说明）；随后重新锁频采了 on 的一组。
- 锁频操作 38d5507c、47c235d3 第一次收尾时系统已把 `policy0/scaling_max_freq` 改为 2112000，工具判冲突未覆盖；稍后再次收尾均读回基线、状态 Cleaned（policy0 上限 2611200）。

证据：`evidence/swan-bloodborne-baseline-20260921/bb-clinic-X/`（`results.txt`、`ab-fps.txt`、各轮 `*-snap1.txt` 与 `*-srt-*.txt`、`perf-off/`、`perf-on/`、`perf-fg-on/`）。
