# GPU 确定性回放：shadPS4 实施设计（2026-10-04）

状态：实施设计。第 0 步（桌面 GPU 内部对象移入 Guest 内存）与捕获端从本轮开始实施，进度记在文末。

依据：
- [GCN 翻译层 v2](gcn-translation-layer-v2-20261003.md) §4（为什么先做回放）、§5 阶段 A。
- Citron 已实现的同类机制：`D:\workspace\citron\docs\plans\20261004-gpu-deterministic-replay.md`（§1 设计原则、§9 对本仓的评估）。
- 本轮对工作区（`feature/mac_fex_arm64`，含未提交改动）的逐项核对，行号以当前工作区为准。

本文只写本仓的做法；Citron 文档里已有的原则（回放内存状态而非读取记录、在 GPU 线程边界记录、从空 cache 开始）不再重复。

---

## 0. 结论

| 项 | 做法 |
|---|---|
| 平台 | 先做桌面经典模式（Windows，clang-cl），Android/FEX 的写故障入口不同，后续单独接入 |
| 第 0 步 | 桌面把 VideoOut label、三个内嵌 shader、init 序列放进固定 Guest 地址的一块内存（Android 已这样做 label 和 shader），PM4 流里不再有 host 指针 |
| 快照点 | 每帧提交结束：`Process()` 一轮处理结束且见到 `submit_done`，此时 Guest 被 `submission_lock` 挡在提交门外，没有存活的协程帧 |
| CPU 写跟踪 | 录制器自有的写保护位图，合并进 PageManager 的页保护计算；写故障时记脏页、去保护、放行；每个边界复制脏页后重新保护 |
| 边界 | 每次顶层 `task.resume()` 之前、每次等待求值之后、每次执行注入命令之前、每轮结束 |
| 回放 | 同一个桌面程序加 `--gpu-replay=<trace>`：重建 Guest 内存和 GPU 栈，不加载游戏；Liverpool 主循环改由回放器按记录的顺序驱动；每次 flip 读回超分前的 guest 图像并计算哈希 |
| 验收 | 同一份 trace 回放两次逐帧一致；画面与捕获时一致（允许第一帧因写回不完整而有差异） |

---

## 1. 现状核对

### 1.1 PM4 调度（`video_core/amdgpu/liverpool.cpp`）

- 57 个队列（1 个图形 + 7 pipe × 8 计算，`liverpool.h:57-63`），`Process()`（`:152-248`）严格轮转，每个队列每轮最多 resume 一次。顶层 `task.resume()` 只有 `:210` 一处。
- 等待只在协程里让出：WAIT_REG_MEM（`:962-982`、`:1307-1313`）、MEM_SEMAPHORE（`:936-947`、`:1295-1306`）、REWIND（`:952-961`、`:1203-1212`）。每次 resume 时重新求值，读的是 Guest 内存或寄存器。
- 例外：等 VideoOut label 的 WAIT_REG_MEM 不让出，而是在 `vo_cv` 上阻塞 GPU 线程，直到 present 线程在 vblank 复位 label（`:970-977`，`videoout/driver.cpp:332-340`）。
- `SendCommand` 的闭包（CPU flip、buffer readback）在每个 packet 之前执行（`:286`、`:399`、`:1077`），位置取决于时序。EOP flip 由 PatchedFlip NOP 在 GPU 线程上同步触发（`:438-444`），位置确定。
- 一轮的结束（`:235-246`）：见到 `submit_done` 时 `OnSubmit` + `Flush`，然后触发 GpuIdle 中断，中断回调重置 `submission_lock`（`gnmdriver.cpp:208-212`）。`sceGnmSubmitDone` 在 GPU 未空闲时设置该门（`:2511-2530`），之后的提交和 doorbell 都在 `WaitGpuIdle()` 处等待（`:214-220`、`:439-512`）。

### 1.2 PM4 里的 host 指针（桌面）

| 对象 | 现状 | 进入 PM4 的方式 |
|---|---|---|
| VideoOut flip label | host 堆上的 `VideoOutPort::local_labels`（`videoout/driver.h:30-31`），地址每次运行不同；`sceVideoOutGetBufferLabelAddress` 把它交给 Guest | `sceGnmInsertWaitFlipDone` 的 WAIT_REG_MEM（`gnmdriver.cpp:1307-1318`）、`PatchFlipRequest` 的 WRITE_DATA（`:2226-2242`） |
| 3 个内嵌 shader | exe `.rdata` 里的静态数组（`gnmdriver.cpp:80-185`） | `SPI_SHADER_PGM_LO/HI`（`:1602-1661`），管线缓存按这个地址读代码 |
| init 序列 | exe `.rdata`（`gnmdriver_init.h`） | 直接作为 DCB 提交（`:2317-2341`） |

exe 固定加载在 `0x700000000000`（`CMakeLists.txt:1694-1713`），同一个程序的两个进程里静态数组地址相同；但 A/B 对比要用不同版本的程序回放同一份 trace，地址不能依赖程序版本。所以这三样必须移入 Guest 内存（第 0 步）。Android 路径已把 label 和 shader 放进 Guest 内存（`host_runtime/guest_runtime.cpp:464-469`、`guest_graphics.cpp:111-118`），桌面有现成钩子（`VideoOutDriver` 的 `guest_labels` 参数，`BindEmbeddedShaders`），只是没用。

### 1.3 内存模型（桌面 Windows）

- Guest VA = host VA（`core/address_space.cpp:149-184`、`:237-296`）。物理后备是一个 8448 MB 的映射对象，另有一个整体视图 `BackingBase()`（`:199-215`），GPU 跟踪从不保护这个视图。
- 有物理后备的 VMA：Direct、Flexible、Pooled（`memory.h:451-454`；游戏模块的段是 Flexible，`module.cpp:228-232`）。栈、系统库、`ThrHeap` 是私有匿名内存。
- GPU 可达性只看 `VA + size < 2^40`（`memory.h:244-252`），不看 GpuRead/GpuWrite 位。
- `TryWriteBacking`（`memory.cpp:598-644`）经后备视图写入，不触发写故障：EOP/EOS label、buffer/image 写回、copy-shader HLE 的提交走这条路。

### 1.4 写故障路径（Windows）

- 向量化异常处理 → `DispatchAccessViolation`（`signals.cpp:595-602`）→ 优先级 0 的 `GuestFaultSignalHandler`（`page_manager.cpp:416-432`）→ `InvalidateMemoryFromWriteFault`。地址只要在 `mapped_ranges` 里就返回已处理（`vk_rasterizer.cpp:2689-2702`）。
- 页状态 `PageState` 是 u8：7 位写计数 + 1 位读计数（`page_manager.cpp:46-88`），保护按计数合成（`Perms()`），`UpdatePageWatchers` 把相同保护的连续页合并成一次 `Protect`（`:108-174`）。
- 文件读取先读到线程局部的中转缓冲，再 `memcpy` 进 Guest（`file_system.cpp:350-365`），所以 HLE 对 Guest 内存的写也会触发写故障，不会因为页受保护而让系统调用失败。

### 1.5 GPU 独有的结果（readbacks 默认关闭）

- buffer：GPU 写过的区间记在 `gpu_modified_ranges` 和页的 GPU 位里，只在 arena 中（`buffer_cache.cpp:474-518`）。没有“全部写回”的入口。readbacks 关闭时一页可以同时是 CPU 脏和 GPU 修改过，写回前要先把 CPU 的字节合进 arena（GpuByteKeeper），否则会用旧数据覆盖 CPU 新写的字节。
- image：`GpuModified` 一旦置位就不清除（`image.h:30-39`）；`RecordImageDownload`（`texture_cache.cpp:463-520`）能重新 tile 并写回。限制：内部缩放过的图像写回是插值近似；模板平面从不写回；MSAA 没有有效的写回路径；CMask/FMask/HTile 只存在于 host 的 clear 掩码里。
- GDS：64 KiB 的 host 可见 Vulkan buffer（`buffer_cache.cpp:33,156`），不在 Guest 内存里。
- CE RAM：静态 48 KiB `constants_heap`（`liverpool.cpp:79`）。

---

## 2. 设计

### 2.1 第 0 步：桌面 GPU 内部对象移入 Guest 内存

- GnmDriver 桌面 `RegisterLib` 在创建 Liverpool 之后，用 `MapMemory(Fixed | NoOverwrite)` 在系统保留窗口的固定地址 `0xFE0000000` 分配 64 KiB，命名 `GpuDriverObjects`：
  - 第 0 页：16 个 VideoOut label（与 Android 布局相同）；
  - 第 1–3 页：三个内嵌 shader；
  - 第 4 页起：全部 init 序列变体。
- `BindEmbeddedShaders` 指向第 1–3 页；`PerformSubmit` 改为提交 Guest 里的 init 序列副本；VideoOut 桌面驱动以第 0 页作为 `guest_labels`。
- 分配失败（地址被占）时记错误并退回原来的 host 对象，游戏照常运行，但该会话不能捕获 trace。
- 这样 `sceVideoOutGetBufferLabelAddress` 返回的也是 Guest 地址，与主机上的行为一致。

### 2.2 Trace 文件

沿用 Citron 的容器形式（`gpu_replay_format.h`）：80 字节文件头 + 带长度和类型的记录；大于 4 KiB 的负载压缩，压缩后更小才采用；后台写线程，队列超过 512 MiB 时写入方阻塞。文件扩展名 `.sgpurply`，magic `SGPURPLY`，版本 1。

文件头：标题号、SDK 版本、Neo 模式、内存配置（extra dmem/fmem，以及游戏加载时由进程参数决定的直接/灵活内存规模）、内部分辨率、捕获时间、页大小位数（12）。

初始状态记录：

| 记录 | 内容 |
|---|---|
| `Info` | `key=value` 文本：设置快照、程序版本、计数 |
| `Vmas` | 被记录的 VMA：基址、大小、类型、保护、名字、物理段（键、物理基址、大小）。回放按它重建映射，物理段保留别名关系 |
| `MemoryPages` | 页数、是否初始快照；`u64 VA[]`；内容。全零页只记 VA，不写内容 |
| `Liverpool` | `regs`（0xD000 dword）、57 个 `cs_state`、`indirect_args_addr`、`num_counter_pairs`、`pixel_counter`、`last_cb_extent`/`last_db_extent`、`num_mapped_queues`、CE 计数器与 48 KiB CE RAM、`flip_epoch` |
| `AscQueues` | 每个已映射计算队列：ring 地址、读指针地址、大小、pipe、`tmp_packet` 与 `tmp_dwords`、slot 序号；GnmDriver 的 `asc_next_offs_dw` |
| `GnmDriver` | `send_init_packet`、`sdk_version`、`frames_submitted`、tessellation ring 地址 |
| `VideoOut` | 端口：分辨率、16 个 buffer slot、4 个属性组、flip rate、`prev_index`、`is_open`、HDR |
| `Gds` | 64 KiB GDS 内容 |
| `BeginStream` | 初始状态结束 |

事件记录（GPU 线程顺序）：

| 事件 | 时机 | 回放动作 |
|---|---|---|
| `MemoryPages`（增量） | 每个边界，内容确实变了的脏页 | 写入 Guest 内存，并按 CPU 写的原路径通知 cache |
| `Submit` | 顶层 resume 之前，取走 Guest 线程上排队的提交描述（队列、DCB/CCB 或 ACB 的地址与长度、提交序号） | 调 `SubmitGfx`/`SubmitAsc` |
| `EopFlipArmed` | 同上，`sceVideoOutSubmitEopFlip` 注册的一次性中断（端口、buffer、flip_arg） | 注册同样的中断回调 |
| `Resume` | 每次顶层 `task.resume()` 之前（队列号、提交序号） | 恢复该队列队首任务；序号不符即报分歧 |
| `WaitPoll` | 每次等待求值之后（队列、种类、地址、结果） | 先写入之前的增量，再求值；记录为“未满足”时强制让出，记录为“满足”而回放未满足时报分歧 |
| `Command` | 执行注入命令之前（CPU flip：端口、buffer、flip_arg、is_eop；readback：地址、大小） | 在同一位置执行等价操作 |
| `Mapping` | 映射变化后的第一个边界：范围与范围内现在的 VMA 列表 | 解除范围内映射后按列表重建（仅保护变化时只改保护） |
| `BurstEnd` | 一轮结束（是否见到 `submit_done`） | `OnSubmit` + `Flush`，触发 GpuIdle |
| `Flip` | `SubmitFlipInternal`（校验与计帧） | 只计数 |
| `End` | 第 N 帧之后的第一个边界 | 结束 |

等待取“求值之后刷新增量”的顺序：原始运行看到的写入必然在刷新之前，回放在求值前写入全部增量，所以“原始满足”在回放中一定满足；刷新时可能多带进求值之后才落地的写，使回放把原本未满足的等待看成满足，这时按记录强制让出，没有副作用（MEM_SEMAPHORE 只在满足时递减）。

### 2.3 捕获

**DebugBus**：`gpu_replay_capture [frames 1-600] [name]`、`gpu_replay_status`、`gpu_replay_cancel`。命令在网络线程上执行，只负责布置；所有工作在 GPU 线程上进行。trace 写到 `user/captures/gpu_replay/<name>.sgpurply`，同名拒绝。

**开始**（布置后，第一个见到 `submit_done` 的一轮结束时，在 GPU 线程上触发 GpuIdle 中断之前）：

1. 检查前提：第 0 步对象已在 Guest 内存；没有挂起的 GfxFlip 一次性中断；内部分辨率 100%（否则记录警告，写回是近似）。不满足则推迟到下一帧，连续 600 帧不满足则失败。
2. `Finish()` 等 GPU 空闲。
3. 写回 GPU 独有结果：buffer 对每段 `gpu_modified_ranges` 先 `SynchronizeMemory`（合入 CPU 字节）再 `DownloadMemory`；image 对每个 `SafeToDownload()` 的 `GpuModified` 图像 `RecordImageDownload`；记下 GDS 内容。
4. 打开文件，启动写线程。
5. 先装写跟踪，再拍快照：对 §2.4 范围内的 VMA 置录制保护位；然后经后备视图（有后备的 VMA）或 VA（其余）复制全部页，记下每页哈希。
6. 写初始状态记录与 `BeginStream`。

**进行中**：钩子见 2.2 的事件表。另外两点：

- 注入命令只在顶层 resume 之前执行（捕获期间跳过 packet 间的 `ProcessCommands`），位置因此就是边界。readbacks 关闭时 readback 命令很少；CPU flip 不等待完成，延迟一次 resume 无影响。
- VO label 的阻塞等待：每次从 `vo_cv` 醒来求值后都按 `WaitPoll` 处理。回放不阻塞，按记录逐次写入增量再求值；回放进程的 present 线程不复位 label，label 只由增量驱动。

**结束**：第 N 次 flip 之后的第一个边界写 `End`，撤销录制保护位，关闭文件，状态记为 finished。`gpu_replay_cancel` 走同一路径，记为 failed/cancelled。

### 2.4 录制器的写跟踪

- **范围**：已映射的 Direct、Flexible、Pooled VMA，以及 `GpuDriverObjects`。不含栈（Windows 在发生异常时要向用户栈写异常记录，栈页不能写保护）、`ThrHeap`、系统库与蹦床、文件映射。
- **状态**：每页 1 位的录制保护位图，覆盖 2^40 VA（32 MiB，按需提交）。PageManager 计算页保护时把它合进来：位为 1 时去掉写权限。`UpdatePageWatchers` 的分段按合成后的保护进行，所以 cache 的计数从 1 变 0 时不会把录制器还要保护的页放开。
- **写故障**（`GuestFaultSignalHandler` 内，最先处理）：页的录制位为 1 → 在该页的锁下清位、把页号加入脏表；若页上还有 cache 的写关注者，照常走 `InvalidateMemoryFromWriteFault`，否则只按合成保护重新设置该页，返回已处理。
- **边界刷新**（GPU 线程）：取走脏表；在页锁下逐段置位并重新保护；然后复制页内容，与上次输出的哈希比较，只输出变了的页。先保护后复制，复制期间的新写入会再次故障，记入下一批。
- **不经过写故障的写入**：`TryWriteBacking` 与 `CopyGuestRegions` 都在 GPU 线程上，属于命令流的确定结果，回放会自己重做，不记录；在其他线程上的异步写回（copy-shader HLE 的延迟提交、image GC 写回）在捕获和回放时都固定为同步或关闭。
- **映射变化**：Windows 的部分 unmap 会按映射时的保护重新映射残余部分（`address_space.cpp:318-403`），录制保护会丢失。所以任何映射变化之后，把受影响区间及其所在区域在下一个边界整体当作脏页，重新保护并比较内容。

### 2.5 回放进程

- 入口：`shadps4 --gpu-replay=<trace> [--gpu-replay-out=<dir>] [--gpu-replay-frames=N] [--gpu-replay-nopng]`，放在 `main.cpp` 检查游戏路径之前。
- 设置：从文件头和 `Info` 恢复标题号、SDK 版本、Neo 模式、内存配置；强制 §2.6 的设置，不回写配置文件。
- 内存：照常创建 AddressSpace 与 MemoryManager，按 `Vmas` 在原物理偏移分配 direct/pooled 内存并映射到原 VA，Flexible 与其余类型按原 VA 映射；写入初始页。
- GPU：照常创建 SDL 窗口与 Presenter（`Presenter` 需要窗口，暂不做离屏）；恢复 Liverpool、GnmDriver、VideoOut 状态与 GDS。
- 主循环：Liverpool 的 GPU 线程在回放模式下不轮转，而是逐个取事件执行（提交、resume、命令、增量、一轮结束），协程内的等待钩子从同一事件流取 `WaitPoll`。
- 出帧：`PrepareFrame` 在 FSR 之前把 guest 图像读回（复用 `screenshot game` 的读回），计算 XXH3，写 PNG（可关），汇总写 `replay_summary.txt`。

### 2.6 捕获与回放都固定的设置

| 设置 | 固定值 | 原因 |
|---|---|---|
| `pipeline_compile_mode` | `sync` | `async_graphics_skip` 会丢掉管线未就绪的 draw |
| 内部分辨率 | 100% | 缩放图像的写回是插值近似 |
| readbacks | 与捕获时相同（默认 Disabled） | 改变 GPU 结果何时进入 Guest 内存 |
| 写故障预测、按内容预测、GpuByteKeeper | 关闭 | cache 内容依赖故障时机，整页增量无法重现 |
| copy-shader HLE 延迟提交、image GC 写回 | 同步 / 关闭 | 在其他线程上按时序写 Guest 内存 |
| EOP/RELEASE_MEM 时间戳 | 回放时按 flip 序号推算 | 来自 host 时钟 |
| 遮挡计数 `pixel_counter` | 从快照恢复 | 本身是确定的合成值 |

---

## 3. 分步实施与验收

| 步 | 内容 | 验收 |
|---|---|---|
| R0 | 第 0 步：GPU 内部对象移入 Guest 内存 | 血源桌面正常进世界、flip 与 label 正常；日志显示对象地址 |
| R1 | trace 容器、录制器状态机、DebugBus 命令、开始时的写回与快照（不含增量）；Python 读取脚本 | 捕获血源一帧的初始状态，脚本列出 VMA、页数、零页比例、各段大小 |
| R2 | 写跟踪与边界事件（增量、Submit、Resume、WaitPoll、Command、BurstEnd、Flip、映射变化） | 捕获 N 帧，事件计数与 GPU 线程实际执行一致；捕获期间游戏不崩溃，结束后帧率恢复 |
| R3 | 回放进程 | 回放 N 帧，画面与捕获时一致；同一 trace 回放两次逐帧哈希一致 |
| R4 | 比较与诊断：帧差异脚本、按 draw 的目标哈希、RenderDoc 抓回放帧 | 能定位两次回放的第一处不同 |

R3 之后再做 GCN v2 阶段 A 的其余项（PM4 自有副本、以真实完成为准的 EOP/EOS），用回放验收。

## 4. 风险与未决

- **起始状态不完整**：模板、MSAA、压缩元数据、缩放图像无法无损写回，第一帧可能与原始画面不同；不影响“两次回放一致”。
- **捕获期间很慢**：全部可达页受写保护，每个脏页在每个边界间隔内故障一次。血源每帧的边界数和脏页数待实测。
- **快照体积**：血源的 direct 内存约 4–5 GB，零页不写内容；实际大小待实测。
- **渲染反馈环**：Citron 在 BOTW 上查明的不确定性来源（采样正在写的颜色目标）在本仓同样可能存在，回放前先用逐 draw 哈希确认。
- **Android**：写故障入口（FEX `gpu_pages`）和内存后端不同，桌面跑通后单独设计。

## 5. 进度

- 2026-10-04：本文；开始 R0。
- 2026-10-04：R0–R3 完成，R4 有逐事件/逐 draw 图像哈希。血源桌面回放画面正确、事件 0 分歧；两次回放每帧约 0.5% 像素不同，定位到 alpha-test 后按像素分歧的分支使隐式导数未定义（显式 LOD 诊断版本完全一致）。实现、用法、数据与修法建议见 [验证记录](../validation/android-native-host/gpu-replay-20261004.md)。之后按用户确认实施着色器修复（kill 处 demote 被杀像素），bb-r3a 三次、bb-final 两次回放逐帧哈希与逐事件图像哈希全部一致，R3 验收达到。与本文的差异：映射变化统一为 `Mapping` 记录；`MemoryPages` 负载为“头、数据页、数据页地址、零页地址”；抓取暂未固定 §2.6 的设置，回放固定为 sync。
