# Swan 血源诊所：下一步优化 spec（2026-09-22，2026-09-23 复核修订）

状态：**待实施 spec**。基线是 [去 fulldump 后复测与快路径修复](../validation/android-native-host/swan-clinic-remeasure-20260922.md) 的 run S（APK a863de67 / host cf2df6be，锁频 1.90/1.50 GHz + 726 MHz，Render 0.5 / Texture medium，mainline Turnip 86ca472f）：诊所可操作场景 1484 draws/帧，**14.3 FPS，帧 69 ms**。2026-09-23 只用已有 PROF/源码做了复核（报告"复核"一节），本版按复核结果改写了 §0 与 P1，其余按需修正。2026-09-23 在合并版 3f0c47ea 上实测（top-app 14.2–15.0 FPS，与 run S 持平）完成 P1 归因并改写 P1 为具体任务，§2 增加 cpuset 守卫；P2 的每帧计数本轮为约 1475 次/帧（rwlock 344+344、AddrWake 162、Sema.Wait 158、Sema.Signal 108），比 run S 的 2420 低，以新值为准。规则不变：锁频只走 spatial-debug-tool profile、同指纹才比 FPS、不伪造数据、保留失败证据、实现走高性能路径、不做游戏专用 patch。

## 0. 谁在限速（复核后的结论）

帧 = 串行的"guest 组帧 → GpuComm 翻译"。`GNM.SubmissionGate` 不是等 GPU：`sceGnmSubmitDone` 在 Liverpool 队列未空时置 `submission_lock`，下一帧的第一次 `sceGnmSubmit*` 就阻塞到 GpuComm 把队列翻译完（`liverpool.cpp` 排空后才 `Signal(GpuIdle)`）。Vulkan 侧 GPU 完成只通过 guest 自己轮询 label 参与。

| 车道 | run S 每帧 | 与帧长的逐帧相关性 | 判定 |
|---|---|---|---|
| GpuComm `PM4.Resume` 覆盖 | **67.9 ms（98%）** | **r = 0.92** | 临界路径。段间空隙 1.6%，子 scope 0.3 ms；但 PROF 无 sched，不能证明 68 ms 全是 on-CPU |
| Guest-1 活跃（帧长 − 各类等待） | 50.4 ms | r = 0.63 | 第二位；其中非等待 HLE ≈ 7 ms，其余 JIT/off-CPU |
| `GPU.GuestCommands`（+Present 1.4 +Overlay 1.3） | 48.8 ms | r = 0.35 | 第三位；GPU 利用率约 70% |
| Guest-19 `GNM.SubmissionGate` | 55.1 ms | r = 0.25 | 症状：每帧一次 ≥20 ms，等上一帧翻译完 |

对照 run R（快路径失效）：Guest-1 活跃 115.7 ms、r=0.84，GpuComm 70.5 ms、r=0.53——修快路径把限速者从 Guest-1 换成了 GpuComm。

两个必须写进 P1 的事实：

- **GpuComm 每帧时间与 draw 数不成比例**：R 1083 draws → 70.5 ms，S 1484 draws → 67.9 ms。"≈46 µs/draw"只是平均数。候选的每帧固定成本：`buffer_upload` 2.2 次、HLE 拷贝 42 次、pass begin 255 次、融合回读 5 次 39 MB、VMA 分配/退休 11 次 22 MB；随 draw 的成本在 `BindResources`/descriptor/barrier。
- **阶梯预期**：GpuComm 降到 ≤35 ms 后帧 ≈ max(Guest-1 50、GPU 49+3) ≈ 52 ms（≈19 FPS）；要过 20 FPS 必须同时压 Guest-1（P2/P3）和 GPU（P4）。任何单项都不要按"帧长减多少"承诺。

## 1. 交付顺序与验收

### P0（本轮已做，需守住）：mutex 快路径窗口

- 已改：`shad_sync_window` 表由 host 在发布时填写；256 MiB `GuestSyncObjects` 预留在 `ServiceAllocationBase + 4 GiB`。
- 还要补的守卫：
  1. host 单测：`GuestSyncArena` 的每个 block 必须落在 `[sync_window_base, sync_window_limit)`，窗口为 0 时 `fast_path` 状态必须是 `installed_no_arena_window`（不是 `installed`）。
  2. `hle_sync status` 的 `fast_path` 值加进 Litep `collect_capture` 的身份检查：capture 前 `installed` 之外一律在报告里标红；`Sync.AddrWait/AddrWake` 行缺失 + `Mutex.Lock` 行存在即判定"快路径未生效"。
  3. 把"服务分配起点整体挪动 256 MiB 会让血源 10 s 内空指针崩溃"登记为待查缺陷（§P5），不是靠 +4 GiB 绕开就算完。

### P1：GpuComm 翻译时间（目标 ≈ 65 ms on-CPU → ≤ 40 ms/帧）

**归因已完成（2026-09-23，[合并版深入归因](../validation/android-native-host/swan-clinic-merged-deep-20260923.md)）**：top-app 锁频同窗 sched + simpleperf + PROF。GpuComm 92.8% 在 CPU 上、阻塞 < 2%，逐帧 `PM4.Resume` 与帧长 r = 0.995；每帧约 65 ms on-CPU 中 SRT 扁平化 15.3、`ObtainBuffer` 11.9（其中 `mprotect` 5.8）、copy-shader HLE 9.0、`BindTextures/BindBuffers` 10.7、Turnip 自身 9.3、PM4 解码 5.6、`StageSpecialization` 2.9 ms（inclusive，互有嵌套）。原第 1–3 步（拆 elapsed、分桶）不再需要重做；第 2 步"扣测量成本"已由 top-app detail 轮给出：detail + hle_sync 开启 14.50 FPS、关闭 14.80–14.85 FPS。

按收益实施，每项单独 A/B（同指纹、top-app、锁频，simpleperf 同桶对比）：

1. **SRT 批量读**（最大单项，15.3 ms/帧）：`Shader::Info::RefreshFlatBuf` → `PortableSrt::Run` 当前每读 4/8 字节调一次 `MemoryManager::TryReadSrtMemory`（每次 `shared_mutex` + `IsValidMapping` + `OwnsRange` + VMA/物理段查找），真正 memcpy 只占 1.8%。改为一次 `RefreshFlatBuf`（最好一次 `RefreshGraphicsStages` 覆盖全部阶段）只拿一次共享锁的读取器对象，缓存最近命中的 VMA 与物理段，同段读只做边界检查 + memcpy；越界/无映射/无读权限仍返回 0，语义与现状逐字节一致（加单测：跨 VMA、跨物理段、无 backing、权限拒绝、并发 unmap 被锁挡住）。`Run` 里每次新建的 `std::vector<Memo>` 与 `flattened_ud_buf.assign` 改为复用。验收：`RefreshFlatBuf` ≤ 3 ms/帧、flat buffer 内容与旧实现在同一 PROF 窗口逐 draw 相同（诊断开关下比对）。
   - **已完成（2026-09-23 晚，[报告 §8](../validation/android-native-host/swan-clinic-merged-deep-20260923.md)）**：`MemoryManager::SrtReadBatch` + `Shader::SrtGuestReader`（参考 Azahar jobs decode 的"一段范围解析一次、一批一次锁、失败回退"），扁平化 13.7 → 3.5 ms/帧；`srt_batch verify` 在血源/TMNT 共比对约 2.9 亿次读取 0 不一致；同会话锁频 off/on 均值 14.64 → 15.19 FPS。锁按 `RefreshFlatBuf` 取，未扩到整个 `RefreshGraphicsStages`（中间可能编译 shader，不宜持锁）。
2. **写跟踪来回**：GpuComm 同步/HLE 拷贝后重新保护的页被 guest 每帧改写，造成 GpuComm `mprotect` 5.8 ms/帧与 Guest-1 缺页处理 2.9 ms/帧。对 256 KiB 分区统计"保护后多快被写"，高频区改为不保护、每次使用时按需上传（或延迟到下一帧再保护），保持 CPU/GPU 脏标记语义；不影响只读或冷数据。验收：两处 `mprotect` 合计降一半以上，BufferCache 定向测试不回退。
3. **copy-shader HLE**：`ExecuteShaderHLE` 76% 在 `ObtainBuffer`，源/目的区间已同步且无新写时跳过同步；`vkCmdCopyBuffer` 在 Turnip 走 2D blit（`r2d_setup_common`），小拷贝可合并。
4. **每 draw 查找**：`ParseFetchShader` 按 fetch shader 地址 + 代码哈希缓存（1.7 ms/帧）；`BindTextures`（`FindImage`、`ImageInfo` 构造、`Transit`）与 `BindBuffers` 对与上一 draw 相同的 sharp 走缓存；descriptor 写入仍按需评估 `VK_EXT_descriptor_buffer`。
5. 屏障/pass 账本与 PM4 解码的原条目保留，但排在 1–4 之后（合计约 6 ms/帧）。
6. **P1b：提交门与流水**（P1 主体见效后再做）：门的语义是"上一帧翻译完才能提交下一帧"，等 GpuComm ≈ Guest-1 时它会让两者串行相加。评估允许 guest 领先一个提交 epoch（`sceGnmSubmitDone` 不置锁、`sceGnmAreSubmitsAllowed` 语义、label 轮询与 VideoOut flip 顺序、内存回压与死锁）；只在 P1 主体后 A/B，不与 P1 同批。
   - 2026-09-23 run X：SRT 批量读后 GpuComm `PM4.Resume` 56.4 ms、Guest-1 活跃 50.6 ms，两条线相关性都约 0.88，已进入"两者串行相加"的情形，P1b 的评估前提成立；仍排在 2（写跟踪来回）之后。
7. **不做的**：不先做"PM4 解码与 Vulkan 录制分线程"的大重构。
8. 验收：同指纹（~1490 draws/帧）锁频 3×10 s FPS；`frame_limiter.sql` 逐帧相关性里 GpuComm 不再是 r 最高的车道，或 `PM4.Resume` 覆盖 ≤ 35 ms/帧且段间空隙上升；`gpu_memory status` 的 draw/pass 计数不变（不能靠少画东西）。

### P2：剩余同步 HLE（2420 → ≤ 800 次/帧）

按 run S 的每帧计数排序：

1. **rwlock 读锁/解锁 1138 次/帧**：沿用 `guest/runtime/sync` 的方式给 `scePthreadRwlockRdlock/Unlock`（及 POSIX 同名）做 guest 快路径——读者计数字 + 写者位，无写者时 CAS 增减不出 guest；写锁、超时、有写者等待时回退现有 HLE。host `GuestRwlockDomain` 改为读写同一 guest 字（和 mutex 一样只保留 prefix 状态）。
2. **kernel semaphore 434 次/帧**：`sceKernelWaitSema` 计数 >0 时 guest CAS 递减、`SignalSema` 无等待者时 guest 递增；有等待者/超时/Poll 语义回退 HLE。需要先确认 Orbis 信号量对象在 host 侧的表示能放进 guest 可读前缀。
3. **mutex Init/Destroy/Config 210 次/帧**：游戏每帧建销 44 个 mutex。guest 侧建对象要在 arena 里分配，第一版不做；先把 `Mutex.Config`（attr settype/protocol 3 次/对象）合并进 Init 的一次 HLE。
4. **cond broadcast 178 次/帧**：Bionic seq 协议进 guest（Tier B 二期原计划）。
5. 验收：`hle_sync` 20 s 窗口按 presents 归一，rwlock 行 <50/帧、Sema 行 <150/帧；`guest_sync_fastpath_tests` 扩展到 rwlock/sema 的 host↔guest 交接、取消、销毁用例并在真机 29+N/0；血源同指纹 FPS 不降。注意这些 HLE 大半发生在 worker（Guest-20…24、Guest-47）而非 Guest-1，对帧长的直接贡献要用 `frame_limiter.sql` 的 Guest-1 活跃列验证，不按次数×单价估。

### P3：Guest-1 的 GNM 小 HLE（447 次/帧 ≈ 2.6 ms）

- `sceGnmSetPsShader/VsShader/CsShader/LsShader/HsShader`（249 次/帧）是纯 PM4 写入；`sceGnmIsUserPaEnabled`（204 次/帧）是常量返回。用与 mutex 相同的 app-shipped guest payload 机制加一个 `guest/runtime/gnm` payload：guest 内直接写 dcb（复用桌面 `gnmdriver.cpp` 的包格式，含 `sceGnmSetPsShader350` 的 shader modifier 分支），常量查询直接返回。
- 验收：Guest-1 这些 HLE 行消失；`gpu_memory`/PM4 消费计数与画面不变；不改任何游戏专用逻辑。

### P4：GPU 每帧 48.8 ms

1. 两个 ≥5 ms 的 pass（G-buffer、光照，各约 7 ms）在 960×540 上的 tiler 成本：用 `gpu_timing detail` + RenderDoc 单帧确认是 bin 数量还是 shader；候选是 `VK_QCOM_render_pass_shader_resolve`/`VK_EXT_rasterization_order_attachment_access` 让光照直接读 G-buffer（PS4 上本来也是同一帧的相邻 pass），减少 attachment load/store。
2. HLE 拷贝把大 pass 拆开（重开 23/帧，G-buffer 2→8、光照 6.5→15.8）：`ExecuteCopyShaderHLE` 把 `vkCmdCopyBuffer` 延后到当前 pass 自然结束，条件是后续 draw 不绑定与 dst 重叠、不写 src（BufferCache 已有每 draw 的绑定范围）；任何 dispatch/download/upload 触及范围就立即冲刷。
3. `sampled_image` 重开 6/帧：写完就被采样的目标在 `EndRendering` 处直接转到 `ShaderReadOnly`（按图像的"上帧被采样"历史决定），猜错的代价只是 pass 边界一次额外屏障。
4. HostReadback 5.3 ms/帧（S 里 5 张/帧、39 MB）：融合回读的 dispatch 本身约 1/3，其余是依赖排空；只能通过让后处理直接读 image（需要 guest 侧改变，不做）或把多张回读合并到一次 dispatch 减少排空次数。
5. Overlay 1.3 ms + Present 1.4 ms：StatusLayer 关闭时应为 0，作为测量修正项记录。
6. 验收：`GPU.GuestCommands` ≤ 40 ms/帧、resumed 实例 ≤ 10/帧，画面用 DumpLayer 与 run S 对照。

### P5：待查缺陷（不阻塞，但要登记）

- 服务分配起点挪动 256 MiB → 血源启动空指针（`fex-fault-27056-1.txt`）：可能的 32 位截断（RSI = RDI 低 32 位）或对栈/TLS 地址上限的假设；用 guest debugger 在 `0x1020b6ac2` 下断，拿到出错前的最后一次 HLE 返回值。
- 标题菜单上施加 DumpLayer 预设与 `SEGV_ACCERR fault 0x24d3c0040`（Guest-19）同秒：GPU 写保护页与 surface 重建的竞争，需在 fulldump 关闭的机器上另想办法拿栈（`debuggerd -b` 或 host LLDB attach）。
- 血源 `Sema.Wait` 单次最长 8.9 s、`Cond.Wait` 1.6 s：加载期正常，但要确认不是 P2 改动后的新等待。

## 2. 测量与工具要求

- **cpuset 守卫（2026-09-23 新增）**：Swan 上 Pico launcher 在内存压力下被杀并重建时会抢走焦点，游戏从 `/top-app`（CPU 0–5）降到 `/foreground`（CPU 0–3），同指纹 FPS 从约 14.7 掉到约 12.3。每次采集前后记录 `/proc/<pid>/cpuset` 与 `dumpsys window` 的 `mFocusedApp`，非 top-app 的窗口作废；不要用 `am start` 抢回焦点（会新建 Activity、销毁 Surface、结束会话）。
- 每轮先跑 `evidence/.../tools/frame_limiter.sql`（Litep `query_sql`，按 tid 替换）给出 GpuComm / Guest-1 活跃 / GPU / owner 等待四条车道的每帧均值与相关性，用它回答"这轮谁在限速"，不再只看单车道均值。
- 每轮都记录 `hle_sync status` 的 `fast_path`、`gpu_memory status` 的 draws/帧指纹（~1490 重 / ~665 轻 / ~1083 桌前）、锁频操作 id 与 baseline；`device_frequency_stop` 出现 Conflict/Failed 要重试并记录。
- GpuComm 的 on-CPU/阻塞拆分需要 sched sidecar：Swan 上 KGSL 采集受 1.5 GiB MemAvailable 守卫限制，改用只含 sched 事件的独立 tracefs instance；simpleperf、PROF、sched 三者同窗。
- 有存档后用 `tools/launch_bb.sh`（卡片 → Launch）+ 圈键 intro 循环；圈键在游戏内是翻滚，到达诊所后立即停止循环（`present` 速率在 15 FPS 以上时旧的"<15 就停"判据失效，改用 StatusLayer `Guest flip` 或 `guest_flip` 计数差分）。
- 屏幕核对用 `pico_capture_dump_layer` + `tools/panel_crop.py`；先确认 `debug.spruntime.etfr.subsample=0`。

## 3. 基准改用 TMNT 屋顶（2026-09-23）

血源在 Swan 上太容易触发内存压力（launcher 被杀 → 游戏丢 top-app → lmkd 把进程写入 zram），用户决定之后先用 TMNT 做 Swan 基准。[TMNT 屋顶基线](../validation/android-native-host/swan-tmnt-roof-baseline-20260923.md)：教程屋顶起点、角色不动、锁频、top-app 为 19.5–19.7 FPS，419 draws/帧。**TMNT 是 GPU 瓶颈**（GPU 忙碌 98–99%，进程只用 0.85 个核），GPU 每帧 40.5 ms 的游戏命令里 `HostTransfer` 占 24.2 ms，来源是每帧 26 张、115 MB 的缩放纹理整张重传（`RefreshImage` → 整段上传 → 解平铺 → 原尺寸临时图 → `BlitBacking`）。

TMNT 上的顺序：

- **T1 诊断重传（已完成，2026-09-23）**：新增默认关闭的 `upload_diag`。屋顶每帧 32 次重传全部由格式化 storage buffer 整张写入引起，不是页级误判。继续追查写入者（用户指出这种重传没有意义）：唯一写入者是 Gnmx 工具库的 compute 填充内核 `dst[i] = src[ctl[1] & i]`，每帧把 32 张渲染目标清成纯色；详见 [TMNT 报告 §6–§7](../validation/android-native-host/swan-tmnt-roof-baseline-20260923.md)。
- **T1b compute 填充改直接清图（已完成，2026-09-23）**：`Rasterizer::TryComputeImageFill` 按指令字节精确识别该内核，读出图案，若填充范围内的缓存图都整张覆盖且为均匀 texel，就 `Image::Clear` 并跳过 dispatch，否则照旧。锁频同会话 off/on 19.7–20.6 → **59.6 FPS**（60 帧封顶），重传 32.5/帧 → 0，画面一致；`upload_diag fill_clear off|on` 可回切。见报告 §8。
- **T2 拦截未变内容**：本场景已由 T1b 从源头消除，暂缓；保留给以后出现 CPU 侧误判的标题。
- **T3 融合上传**：降为次要。本场景已无重传；其它场景里真正由 CPU 或非清除着色器写出的纹理仍走原尺寸临时图 + blit，届时再做（一次 compute 从 guest 布局解平铺并按倍率写入 scaled backing，与 `TileImageFromScaled` 方向相反）。
- **基准场景需要换**：屋顶现在 60 帧封顶、GPU 约 90% 忙碌，不再能区分后续优化；下一轮先在 TMNT 选一个不封顶的场景（战斗或多角色），并用 `compute_fill` 计数确认填充内核在其它游戏（血源）里的命中情况。
- **T4 Present 缩短持锁**：进 `QueueMutex` 前在锁外等上一帧 `present_done`，让 Android `BufferQueueProducer::queueBuffer` 的 EGL 节流等待不再发生在锁内（修复前 VkSubmit 每帧在锁上等 35.9 ms）；保留提交/present 同步语义，在不封顶的场景复测是否仍有收益。
- P1 的 SRT 批量读仍然做（TMNT GpuComm 每帧约 16 ms on-CPU 中占 20%），但不在 TMNT 的关键路径上。

验收口径：屋顶起点、角色不动、锁频、top-app，3×10 s FPS，并对比 `scale_upload_created_count`/帧与 `GPU.HostTransfer` ms/帧。进入方式与 DebugBus 手柄脚本见报告 §3。
