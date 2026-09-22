# Swan 血源诊所：下一步优化 spec（2026-09-22，2026-09-23 复核修订）

状态：**待实施 spec**。基线是 [去 fulldump 后复测与快路径修复](../validation/android-native-host/swan-clinic-remeasure-20260922.md) 的 run S（APK a863de67 / host cf2df6be，锁频 1.90/1.50 GHz + 726 MHz，Render 0.5 / Texture medium，mainline Turnip 86ca472f）：诊所可操作场景 1484 draws/帧，**14.3 FPS，帧 69 ms**。2026-09-23 只用已有 PROF/源码做了复核（报告"复核"一节），本版按复核结果改写了 §0 与 P1，其余按需修正。规则不变：锁频只走 spatial-debug-tool profile、同指纹才比 FPS、不伪造数据、保留失败证据、实现走高性能路径、不做游戏专用 patch。

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

### P1：GpuComm 翻译时间（目标 67.9 → ≤ 35 ms/帧）

先归因，再动手；采样前不要预设是 per-draw 成本。

1. **拆 elapsed**：同一窗口对 GpuComm 做 (a) simpleperf on-CPU 采样（`-e cpu-clock -f 4000 --call-graph fp`，20 s，锁频），(b) 只开 `sched_switch/sched_wakeup` 的独立 tracefs instance（不采 KGSL，绕开 1.5 GiB 守卫）拆出 on-CPU / runnable / sleep，(c) Litep PROF 同窗。先回答：68 ms 里有多少是被阻塞（BufferCache 256 KiB 区锁、TextureCache mutex、staging 满、`vkQueueSubmit` 锁、fence）。若阻塞 ≥ 15%，先解锁再谈 per-draw。
2. **扣掉测量成本**：`gpu_timing detail`（每帧 ~200 个 zone、每个两次 timestamp）和 StatusLayer overlay 各关一次再采，得到 GpuComm 的净成本；`hle_sync` 也关。
3. **固定项 vs 随 draw 项**：用 (a) 的调用链把 self 时间聚到这些桶——`Liverpool::ProcessGraphics`（PM4 解码/寄存器写）、`Rasterizer::Draw/BeginRendering`、`Pipeline::BindResources`（buffer/image 查找、descriptor 写）、`BufferCache::ObtainBuffer/SynchronizeBuffer`（含 dirty memcpy）、`TextureCache::FindImage/UpdateImage`、`Image::Transit`/barrier 账本、`ExecuteCopyShaderHLE`、`TileManager` 回读、`PipelineCache` 查找/编译、Turnip `vkCmd*`。另按 R/S 两个场景比较：draw 数 +37% 而时间不变，要么多出的阴影 draw 极便宜，要么固定项占大头。
4. **杠杆（按采样结果取舍）**：
   - 绑定：`BindResources` 每 draw 的 30 个 unified binding 查找与 descriptor 写入改增量更新、按 (buffer id, offset) 缓存命中；Turnip 已报 `VK_EXT_descriptor_buffer`，若 `vkCmdPushDescriptorSet`/`vkUpdateDescriptorSets` 占比高再评估。
   - BufferCache：`ObtainBuffer` 区间表/分区锁每 draw 多次，考虑 draw 级缓存与批量同步；dirty 上传的 memcpy 走 staging 时不要持区锁。
   - 屏障/pass 账本：`Image::Transit`、`RenderBreak` 计数、`RecordAttachmentDraw` 在热路径的成本压成采样式；默认关闭的诊断不能留常开原子。
   - PM4 解码：`SET_CONTEXT_REG/SET_SH_REG` 逐 dword 与 `sceGnmSet*Shader` 的重复解析（这部分也对应 P3 的 Guest-1 HLE）。
   - 每帧固定项：HLE 拷贝 42 次、回读 5 次、上传 2 次的 CPU 侧（不是 GPU 侧）成本单列。
5. **P1b：提交门与流水**（P1 主体见效后再做）：门的语义是"上一帧翻译完才能提交下一帧"，等 GpuComm ≈ Guest-1 时它会让两者串行相加。评估允许 guest 领先一个提交 epoch（`sceGnmSubmitDone` 不置锁、`sceGnmAreSubmitsAllowed` 语义、label 轮询与 VideoOut flip 顺序、内存回压与死锁）；只在 P1 主体后 A/B，不与 P1 同批。
6. **不做的**：不先做"PM4 解码与 Vulkan 录制分线程"的大重构。
7. 验收：同指纹（~1490 draws/帧）锁频 3×10 s FPS；`frame_limiter.sql` 逐帧相关性里 GpuComm 不再是 r 最高的车道，或 `PM4.Resume` 覆盖 ≤ 35 ms/帧且段间空隙上升；`gpu_memory status` 的 draw/pass 计数不变（不能靠少画东西）。

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

- 每轮先跑 `evidence/.../tools/frame_limiter.sql`（Litep `query_sql`，按 tid 替换）给出 GpuComm / Guest-1 活跃 / GPU / owner 等待四条车道的每帧均值与相关性，用它回答"这轮谁在限速"，不再只看单车道均值。
- 每轮都记录 `hle_sync status` 的 `fast_path`、`gpu_memory status` 的 draws/帧指纹（~1490 重 / ~665 轻 / ~1083 桌前）、锁频操作 id 与 baseline；`device_frequency_stop` 出现 Conflict/Failed 要重试并记录。
- GpuComm 的 on-CPU/阻塞拆分需要 sched sidecar：Swan 上 KGSL 采集受 1.5 GiB MemAvailable 守卫限制，改用只含 sched 事件的独立 tracefs instance；simpleperf、PROF、sched 三者同窗。
- 有存档后用 `tools/launch_bb.sh`（卡片 → Launch）+ 圈键 intro 循环；圈键在游戏内是翻滚，到达诊所后立即停止循环（`present` 速率在 15 FPS 以上时旧的"<15 就停"判据失效，改用 StatusLayer `Guest flip` 或 `guest_flip` 计数差分）。
- 屏幕核对用 `pico_capture_dump_layer` + `tools/panel_crop.py`；先确认 `debug.spruntime.etfr.subsample=0`。
