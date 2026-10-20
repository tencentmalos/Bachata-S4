# GpuComm 架构参考：Azahar gpu_async 与 bbport 两阶段流水线（2026-10-20 整理）

用途：KH3 在 Odin3 上 GpuComm 占满一个核、成为帧长的决定项（[KH3 记录](validation/android-native-host/kh3-odin3-10fps-20261020.md)）。
用户认为按 draw 数提前提交（streaming submit）不可靠，要求参考血源 PC 版（bbport）与 Azahar 的 gpu-async，
找更合适的结构。本文只整理源码与既有文档，没有新的测量。Azahar 部分来自只读调研（未运行），bbport 部分的
细节已在 [两阶段流水线评估](two-stage-pipeline-evaluation-20261012.md) 与 [bbport 参考](bbport-reference-20261012.md)。

## 1. Azahar gpu_async

源码 `C:\workspace\emulations\3ds\azahar`，性能文档在 `docs\manual\self\Performance\`（中文）。
**分支**：09-08～09-11 的关键提交（`8e75623b7`、`c29d6cc4a`、`e037d740d`、`b2f7346ef`、`bbbd5974f` 与 MK7 文档）
只在 `codex/turnip-mesh-prototype` 上，`feature/malos/gpu_async_plus`（`6f46f7175`）不包含。

### 1.1 线程

| 线程 | 职责 |
| --- | --- |
| guest 提交线程 | `QueueSubmitCmdList`（`src\video_core\gpu.cpp:1870-2097`）同步完成：把命令列表复制成不可变的 `PicaCommandListPacket`；`PicaCommandListAccessAnalyzer` 算出 index/vertex/texture/RT 的精确读写范围；识别重放、把解码任务投到 job 池、入队 |
| 解码（job 池，固定 4 个） | `WORK_JOB`（`src\core\core.cpp:1361`）；PICA 线程等 `PicaDecodeTicket`（`pica_command_replay.h:35`） |
| PICA GPU 线程 | `GpuCommandExecutor`（`gpu_thread.cpp:955`），唯一持有 PicaCore 与光栅器；等解码后持 `pica_mutex` 执行 `ProcessCmdList`，把 Vulkan 命令录成 32 KiB `CommandChunk`，present 作为 FIFO 中的 `FrameBoundary` |
| VulkanWorker | 执行 chunk、`vkQueueSubmit`（`vk_scheduler.cpp:198,420`） |
| VulkanCompletion | 把完成发布给 guest（`vk_scheduler.cpp:266`） |

队列：执行器 MPSC（容量 64，单调 fence，满时阻塞）、chunk 队列、`GuestCompletionQueue`、GSP 中断邮箱。
`gpu_async=0` 时同一个工作线程仍在，但提交阻塞到 fence，同时关掉读范围、包和阶段重叠，所以同步/异步对比不是单变量。

### 1.2 guest 可见的同步

- 普通提交从不阻塞 CPU。CPU 只在下面几处等：
  - 缓存页访问与 DMA 走 `ExecuteSerialized`（`gpu_thread.cpp:311`），与最近 128 条提交按精确范围比对：不重叠当场执行，
    部分重叠在依赖 fence 之后的命令边界执行，无法判断才整队列排空。
  - 回读 GPU 写过的内存（`FlushRegion` → `DownloadSurface` → `Finish`），唯一的真实 GPU 往返。
  - 读寄存器取 `pica_mutex`（最多等一个列表），传输状态按代际发布（`gpu_completion_status.h`）。
- 完成：每个 `irq_request` 与加速 fill/transfer 经 `QueueGpuCompletion` → `FlushWithGpuCompletion` 强制提交并登记
  `{tick, wait_for_retirement}`；完成线程在提交被接受（`QueueSubmitted`）或 tick 退役（`FenceRetired`，默认）时发布；
  `GSP::SignalInterrupt` 写入邮箱，在安全时间点才送给 guest（`gsp_gpu.cpp:581,639`）。
- 可恢复调度（`e037d740d`）：guest 的 GPU 请求持有 `GuestGpuProgress::Work` 租约；所有 guest 核都空闲且有未完成租约时，
  调度器按代际计数等完成、下一个定时器或停止，中断在本帧送达而不是等帧限速。MK7 单核：发布 18.0 → 59.8 次/s，
  GSP 最老条目延迟 p50 10141 → 32 µs。
- 文档要求的不变式：不丢唤醒（先取代际再读事件计划）、持 GSP/事件/会合锁时不等待、不推进时钟、不打乱 FIFO、
  完成回调不取 `pica_mutex`、按标题 epoch 隔离迟到回调、设备丢失不发布成功。

### 1.3 内存一致性

- 只有表面所在页标记为 `RasterizerCachedMemory`，其余页没有慢路径。
- `dirty_regions` 记 GPU 写过的区间，`FlushRegion` 对干净范围直接返回。
- CPU 写两阶段失效（`ff7699bcb`）：`OnCPUWrite` 先登记、写入后在 `cpu_write_mutex` 下收集，每条工作线程命令前与 VBlank 时排空，
  `FlushRegion` 扣除待处理的 CPU 写，避免旧 GPU 副本覆盖它们。
- 精确读范围（每列表最多 512 段，`92358744d`）：invalidate 等待 13.2 → 0.45 ms/帧，帧 p50 −22%。
- GPU 的输入全部由宿主持有：命令列表提交时复制（约 1 MB/帧），vertex/index/uniform 复制进宿主缓冲，纹理解码进 staging。
  所以 guest 复用自己的内存不需要等 GPU 退役。

### 1.4 批与提交

- chunk 写满只交给工作线程，不是提交。真正提交在：P3D/PSC/PPF 完成、present、`Finish`；按命令列表切分提交（Qualcomm/ARM）
  默认关闭。
- “超过 20 个 draw 就提交”的规则保留过一段（关掉会使 PICA 同步等待 +36.7%），`7c50b0305` 删除；每个命令列表结束都提交
  （约 4.7 次/帧、每次约 265 draw）使 PICA 路径几乎翻倍，被否决。present 合并（`da55df2eb`）已撤回。

### 1.5 每 draw 降本

- 寄存器组脏标记在解码侧计算（`400a93174`、`3aaf418ce`；`pica_register_groups.h:16`）。VS/GS 程序、片元配置、光栅状态、
  顶点布局、帧缓冲组都干净时跳过管线查找（`vk_rasterizer.cpp:1667`）；纹理描述符复用以 Texturing/Framebuffer 组与注册表、
  失效、tick 代际为门（`:2395`）；顶点布局与帧缓冲表面查找同样复用（`6164598b3`、`d715ce35a`）。
- uniform：VS uniform 作为解码侧 patch 上传（上传 −47%，重放块 −39%）；每帧只追加的 uniform arena，按类别脏标记驱动，不哈希
  （`a665a7ab8`）。
- 常驻 mesh 缓存（64 MiB，按采样哈希，连续两帧出现才收录）：顶点上传 12.5 MB → 0.8 KB/帧，顶点设置 4.0 → 0.48 µs/draw；
  只采样部分字节，guest 改写未采样字节时有正确性风险。
- 解码放 job 池、PICA 线程等结果（`8e75623b7`）：解码后的计划比逐字节解释快 40%，PICA 线程 14.2 → 6.8 ms/帧（只在 Windows
  测过）；把解码再切成并行块、等待前自旋都试过并放弃。
- 逐 draw profiler zone 占测量开销约 40%，已改为按需开启。

### 1.6 撤回与遗留

- record/replay 删除（调用缓存从未命中）：PICA 线程 10.27 → 8.14 ms。保留命令包、异步解码和重放识别（只用于共享解码）。
- MH3G 帧不均：先后的“完成改到 GPU 退役”“帧边界/限速”“lineage/epoch/present 对齐”都撤回；真正原因是游戏循环只等
  `nngxWaitCmdlistDone`，每个 VBlank 跑 2.8 轮，用 guest 补丁修复（1480 → 535 draw/VBlank）。
- `FenceRetired` 仍为默认（guest 等待 p50 约 9.37 ms）；改 `QueueSubmitted` 帧时 33.7 → 24 ms，但之后一次 SIM 由 40–42 降到 20–23，
  正确性未证明。
- Android 上解码默认“等待”模式未实测（Windows 数据决定，之前 Android 不等待 59.6 对 53.8 Hz），可能是回退。
- 未解决：`pica_mutex` 覆盖整个列表；present 与渲染共用一个 VkQueue 和 `submit_mutex`；宿主录制与 guest/PICA 的重叠未测；
  uniform arena 会等忙槽；约 65 个 render pass/帧未动；完成链没有逐请求身份；没有 Android 或多游戏长测。

## 2. bbport（血源 PC 版）要点

见 [评估](two-stage-pipeline-evaluation-20261012.md)。阶段 A（PM4 解码、寄存器文件、CE、管线选择）把每个 draw/dispatch
写成 16 MiB 环中的包（脏寄存器块、按 stage 的 user data 与扁平 user data 快照、draw 参数）；阶段 B（`bb:DrawRec`）持有
自己的寄存器副本，做纹理、缓冲、RT、屏障、描述符、动态状态和录制，包在途时独占纹理与缓冲缓存；`PipelinedOpcode` 之外的包
让阶段 A 等阶段 B 排空。8 个以上硬件线程才默认开。另有常量环（阶段 A 把小只读缓冲复制进 32 MiB 环，+11%）、
render-state memo（+9%）、texture-set memo（+5.6%）。RX 7800 XT 上拆分 +19–32%；阶段 B 85–99% 忙，阶段 A 约 25% 在等。
两个拆分缺陷：GPU idle 先于延迟 fence 发出导致堆损坏；阶段 B 在 CE 改写后才读 V# 表（DoF #44）。

## 3. 对照 shadPS4（2026-10-20）

| 参考做法 | shadPS4 现状 | 结论 |
| --- | --- | --- |
| 寄存器组脏标记，组干净跳过管线/描述符/布局重建（Azahar）；render-state、texture-set memo（bbport） | GCN v2 spec 阶段 C：寄存器脏位图（`RegTracker`，按值比较、按组代际）与渲染目标 / render state memo 已做（10-20，KH3 26 → 29.5 FPS，见 [KH3 记录](validation/android-native-host/kh3-odin3-10fps-20261020.md)）；管线 key 增量维护、动态状态按组重建未做 | 进行中 |
| 只追加的 uniform arena + 脏标记（Azahar）、常量环（bbport） | 10-20 的 stream 拷贝复用（`stream_reuse.h`）：同一窗口内复用拷贝，KH3 90 → 21 MB/帧、22.4 → 26.7 FPS | 已做 |
| 解码放 job 池、单执行线程（Azahar）；两阶段流水线（bbport） | GpuComm 一个线程做 PM4 解码、寄存器、管线选择、资源绑定、录制前处理；录制线程、提交线程、完成线程已拆出 | 脏位图之后评估；按 spec 阶段 B 启动条件（GpuComm > 80% 帧长且 r ≥ 0.8、GPU 忙 < 85%、有空闲快核、无温控） |
| 在 guest 可见的完成点提交 + 精确访问范围（Azahar） | streaming submit 按 draw 数提前提交，只能逐游戏开关 | 访问范围可在阶段 B 的包里一起产生；之后取代 streaming submit |
| 可恢复的 guest 调度（Azahar） | PS4 guest 线程是真实宿主线程，不是 fiber | 不适用 |

教训：Azahar 删掉了“每 N 个 draw 提交”的规则；MH3G 与 KH3（Guest-57 等查询结果空转）一样，最终是游戏自身的等待方式，
用 guest 补丁解决。拆线程只缩短串行链、不减少工作量，所以先降每 draw 成本（阶段 C），再看拆分。

## 4. “解码放 job 池、单执行线程”评估（10-20，阶段 C 第一项之后）

**数据**（KH3 Odin3 燃烧的街道，stream 拷贝复用 + 目标 memo 开，29.5 FPS，GpuComm 占满一个核，约 37.5 ms/帧、5.5 µs/draw；
`build/validation/stagec-20261020/scq_children.txt`）：

| 若照 bbport 切分 | 内容 | 占 GpuComm |
| --- | --- | ---: |
| 阶段 A | PM4 解码与寄存器文件（`ProcessGraphics` 自身 2.9%）、管线选择 `GetGraphicsPipeline` 22%（`GetProgram` 17.5%，其中 SRT 走表 7.5%、fetch shader 2.5%、排列匹配 2.2%） | 约 27% |
| 阶段 B | `BindResources` 29%（`ObtainBuffer` 13%、`BindBuffers` 11%、`BindTextures` 11%）、描述符推送 7%、`BindVertexBuffers` 9%、`BindIndexBuffer` 2.6%、目标与 render state 7%、动态状态 2%、录制与其余 draw 逻辑约 10% | 约 63–65% |

- **Azahar 式“解码放 job 池”在这里收益很小**：PM4 解码本身只占约 3%。Azahar 的解码重是因为 PICA 命令列表里的 uniform/寄存器
  写入要逐条解释；我们的重活在每 draw 的资源解析（读 guest 内存里的 sharp、SRT、缓冲/纹理缓存）。真正对应的是“把每 draw
  的资源解析做成 plan，执行线程只录制”，也就是 bbport 的阶段 A/B。
- **bbport 式两段**：按上表切分不均衡，关键路径变成阶段 B（约 65%），GpuComm 一侧最多约 1.5 倍；要更均衡需把一部分绑定解析
  （sharp 读取、SRT 展开、ObtainBuffer 的判定）挪到阶段 A，即 bbport 作者未做的方案（б）。
- **阶段 B 启动条件**（spec §6）：同窗实测见 4.1，几条都满足，但 GPU 一侧把收益上限压在约 1.4 倍，第二颗超大核被 Guest-57 的
  查询等待空转占着，温控余量也小。
- **主要风险**（bbport 经验与我们的特有路径）：纹理/缓冲缓存在包在途时的归属；写跟踪缺页与 `SendCommand` 回读；CE 改写 V# 表
  （bbport DoF #44：阶段 A 必须给包带 user data/扁平 user data 快照）；GPU idle 先于延迟 fence（bbport 堆损坏）；copy-shader HLE、
  异步管线跳过、pass 上提都要重新审计。

**建议顺序**：

1. ~~先补阶段 B 的启动前提~~：已做，见 4.1。
2. 继续单线程降本（风险低，阶段 B 也会受益）：SRT 走表 7.5%（按 user data 与读集合代际门控，Android 上比桌面估计的上限大得多）、
   `BindTextures` 自身 5.2%、剩余 stream/扁平 user data 拷贝 8.5%、管线 key 增量维护。
   10-20 夜进展（[KH3 记录](validation/android-native-host/kh3-odin3-10fps-20261020.md#srt-走表降本与管线-key-增量维护10-20-夜已提交)）：
   门控实测可省上限约 1%（UE4 在同一地址重写描述符表，(permutation, user data) 重复的走表有 46% 读到新内容），不做；改为 SRT
   预编译计划、内存锁持有统计默认关、管线 key 按 `Targets`/`KeyState` 代际 memo 与同 key 复用管线、fetch shader 缓存改 4 路组相联。
   `GetGraphicsPipeline` 占 GpuComm 25.0% → 18.1%，同会话 28.6 → 30.5 FPS（+6.8%，不含 fetch 缓存）。随后 sampler 前置缓存（GetSampler 1.1% → 0.4%）与宿主 DSO
   `-Bsymbolic-functions`（`@plt` 4.1% → 0.75%，跨会话 +3%）。下一批候选：`BindResources` 30%（`ObtainBuffer`、`BindTextures`）、
   descriptor 录制 3%、`Matches` 2.4%。
3. 然后按 bbport 包格式做阶段 A/B 原型：先只移录制侧（目标、描述符推送、动态状态、录制），阶段 A 保留资源解析，用确定性回放
   做逐包校验（对应 `BB_PIPE_VERIFY`），按硬件线程数决定是否启用。

### 4.1 阶段 B 启动条件实测（10-20 晚，LiteP 同窗采集）

Odin3，KH3 燃烧的街道（同一存档点，站立），APK `32770151`（stream 拷贝复用、目标 memo 都开，KH3 profile `stream_submit` 开，
`guest_patches` 为空即未开 `kh3_cpu_waits`）。两次 12 s 同窗采集（PROF + KGSL/sched，`collect_evidence`，256 MiB，无 overrun/丢块）：
`build/validation/stagec-20261020/litep1/cap-hot`（进场约 5 分钟后）、`litep2/cap-warm`（冷却后重新进场约 2 分钟）。逐帧统计用
`stagec-20261020/stageb.py`：sched_switch 求各线程在 CPU 上的时间与 runnable，KGSL ctx 15 的 start..retire 求 GPU 忙，帧为 SDK 帧
（KH3 每帧一次 `sceGnmSubmitDone`）；LiteP 自带的 `analyze_cpu_gpu_bottlenecks`（中间 2 s）结果一致。

| 每帧 | 第一次（353 帧，33.9 ms，29.5 FPS） | r | 第二次（361 帧，33.2 ms，30.1 FPS） | r |
| --- | ---: | ---: | ---: | ---: |
| GpuComm 在 CPU 上 | 33.2 ms（97.7%） | 0.90 | 32.5 ms（97.7%） | 0.81 |
| GpuComm `PM4.Resume` | 32.7 ms（96.4%） | 0.94 | 32.2 ms（96.9%） | 0.89 |
| GPU 忙（ctx 15） | 28.0 ms（82.4%） | 0.24 | 27.4 ms（82.3%） | 0.69 |
| Guest-57（UE4 渲染线程） | 26.9 ms（79.3%） | 0.69 | 27.1 ms（81.6%） | 0.60 |
| VkRecord | 17.7 ms（52.2%） | −0.01 | 17.4 ms（52.4%） | 0.17 |
| Waker | 10.3 ms | | 10.2 ms | |
| Guest-1 | 6.4 ms（18.7%） | 0.10 | 6.3 ms（18.9%） | 0.07 |

- **GpuComm 决定帧长**：每一帧 GpuComm 都超过帧长的 80%；最长与最短四分之一帧相差 3.4 / 2.3 ms，其中 GpuComm 3.0 / 1.8 ms，
  GPU 0.8 / 2.0 ms（第二次 GPU 与帧长的相关也升到 0.69，GPU 已接近一起限速）。GpuComm 的时间是真实工作：`Rasterizer.Draw`
  29.6 ms/帧（6859 draw，约 4.3 µs/draw），PM4 自身 2.3 ms，等 VideoOut label 0.09 ms；帧率落在 30 附近是巧合，不是节拍等待。
  GpuComm 几乎不排队（runnable 0.06 ms/帧），99.8% 跑在超大核 cpu6/7 上。
- **超大核没有空闲**：Guest-57 的 r≈0.6–0.7 来自查询等待空转（等的结果由 GpuComm 推进），它的在 CPU 时间有 67–79% 也在 cpu6/7
  上；两颗超大核同时忙的时间占 61% / 68%，cpu6/7 空闲 17%/23%、24%/9%。
- **中核有空闲**：cpu0–5 各空闲 47–50%，合计平均约 2.9 个核；至少一个核空闲的时间约 89%，至少两个约 73%。VkRecord、Waker、
  Guest-1 都在中核上。游戏进程外的 CPU 时间约 0.27 个核。
- **频率**：中核时间加权 2.70 GHz（上限 2.7456，硬件 3.53），超大核 3.07 GHz（上限 3.072，硬件 4.32），GPU 925 / 953 MHz（最高
  1100，governor 按 82% 忙给的频率）。CPU 上限与温度无关：停游戏后 90 s 内 Thermal Status 从 5 降到 0、空闲时上限仍是
  2.7456/3.072 GHz（`litep1/cooldown.txt`、`litep2/cpustate-prelaunch.txt`）；疑为 AYN 标准性能模式（`settings system
  performance_mode=0`），未在 UI 上确认。
- **温控**：内核的 CPU/GPU 冷却设备（`cpufreq-cpu0/6`、`cpu-cluster0/1`、`pause-cpu*`、`cpu-hotplug*`、`gpu`）全为 0，
  `thermal_pwrlevel=0`、`throttling=0`，core_ctl 两簇 6/6、2/2 全开：采集时没有降频或挂核。但这个场景进场约 2 分钟 Android
  Thermal Status 到 3、约 4 分钟到 5（来自 skin 61 °C；CPU 热点 105 °C），两次采集都在 3–5 之间。

**结论**：spec §6 的四条都满足（GpuComm 占帧 97.7%、r 0.81–0.94；GPU 82% < 85%；Guest-1 6 ms 远短于 GpuComm；中核平均约
2.9 个空闲、未降频未挂核）。但：

1. **GPU 上限**：每帧 GPU 忙约 27.7 ms@925–953 MHz，升到 1100 MHz 约 23.5 ms，GPU 受限的上限约 42 FPS（现在的 1.4 倍）。
2. **切分**：按 §4 的切分，阶段 B 约 65% ≈ 21 ms 应留在超大核，阶段 A 约 9 ms 可放中核。按 capacity × 当前上限/最高频率估算，
   中核约为超大核的 0.85（792×2.7456/3.5328 ≈ 615 对 1024×3.072/4.32 ≈ 728），A 在中核约 10.5 ms；流水线约 22 ms/帧，与 GPU
   上限接近，所以两段即使理想并行，最多也只到约 1.4 倍。
3. **第二颗超大核被 Guest-57 占着**：阶段 B 要与 `kh3_cpu_waits` 补丁一起评估（10-13 实测 Guest-57 10.7 → 3.4 s/10 s），否则新线程
   与 Guest-57 争 cpu6/7，或只能放到中核。
4. **温控余量小**：再加约一个核的持续负载，长时间运行可能触发真正的限频；验收时同窗记录 Thermal Status、冷却设备与实际频率。

因此建议顺序不变：先做单线程降本（同时降功耗，GPU 一侧也会跟着升频），阶段 B 原型放在其后，并以“开 `kh3_cpu_waits` + 同窗采集
无降频”为验收前提。
