# Swan 血源诊所：下一步优化 spec（2026-09-22）

状态：**待实施 spec**。基线是 [去 fulldump 后复测与快路径修复](../validation/android-native-host/swan-clinic-remeasure-20260922.md) 的 run S（APK a863de67 / host cf2df6be，锁频 1.90/1.50 GHz + 726 MHz，Render 0.5 / Texture medium，mainline Turnip 86ca472f）：诊所可操作场景 1484 draws/帧，**14.3 FPS，帧 69 ms**。规则不变：锁频只走 spatial-debug-tool profile、同指纹才比 FPS、不伪造数据、保留失败证据、实现走高性能路径、不做游戏专用 patch。

## 0. run S 的每帧账本（谁在限速）

| 车道 | 每帧 | 说明 |
|---|---|---|
| 帧长 | 69.0 ms（p90 75.1） | 289 帧 PROF，`gpu_timing detail` 开着 |
| `shadPS4:GpuComm` `PM4.Resume` | **67.9 ms（98%）** | 子 scope 只有 0.3 ms：PM4 解码 + Vulkan 录制本身在吃 CPU，不是在等 GPU |
| Guest GPU 命令 | 48.6 ms | RenderPass 33.4（2 个 ≥5 ms 的 pass = 14.2；180 个 <200 µs 的实例 = 7.5）、HostReadback 5.3、Dispatch 3.0、Transfer 2.8；另 Present 1.4 + Overlay 1.3 |
| Guest-19（帧 owner） | `GNM.SubmissionGate` 55.1 ms | 提交门等 GPU/GpuComm 消化上一批 |
| Guest-1（游戏主线程） | HLE 23.8 ms（862 次）+ 未注解 45.2 ms | CondWait 16.1 ms 是等 worker；非等待 HLE ≈ 7 ms：SetPs/Vs/Cs 243 次 2.5 ms、IsUserPaEnabled 204 次、Sema 73 次 3.1 ms |
| 全进程同步 HLE | 2420 次/帧 | rwlock 569+569、AddrWake 264、Sema.Wait 258、Signal 176、Mutex.Config 122、AddrWait 105、Cond 89+89、Init/Destroy 44+44 |

结论：CPU 侧 GpuComm 与 GPU 侧几乎同时到顶（68 vs ~51 ms），任何一边单独降下去都只能换来几毫秒；要到 20 FPS（50 ms）两边都得降。GpuComm 每 draw 约 46 µs（1484 draws + 27 dispatch + 255 次 pass begin + 42 次 HLE 拷贝 + 3 次融合回读），是首要目标。

## 1. 交付顺序与验收

### P0（本轮已做，需守住）：mutex 快路径窗口

- 已改：`shad_sync_window` 表由 host 在发布时填写；256 MiB `GuestSyncObjects` 预留在 `ServiceAllocationBase + 4 GiB`。
- 还要补的守卫：
  1. host 单测：`GuestSyncArena` 的每个 block 必须落在 `[sync_window_base, sync_window_limit)`，窗口为 0 时 `fast_path` 状态必须是 `installed_no_arena_window`（不是 `installed`）。
  2. `hle_sync status` 的 `fast_path` 值加进 Litep `collect_capture` 的身份检查：capture 前 `installed` 之外一律在报告里标红；`Sync.AddrWait/AddrWake` 行缺失 + `Mutex.Lock` 行存在即判定"快路径未生效"。
  3. 把"服务分配起点整体挪动 256 MiB 会让血源 10 s 内空指针崩溃"登记为待查缺陷（§5），不是靠 +4 GiB 绕开就算完。

### P1：GpuComm 每 draw CPU 成本（目标 67.9 → ≤ 35 ms/帧）

方法先于猜测：

1. **采样归因**：锁频、同指纹下对 GpuComm 线程做 simpleperf on-CPU 采样（`-e cpu-clock -f 4000 -g`，20 s，`--call-graph fp`），配合 Litep PROF 同窗；用 `debug.shadps4.*` 现有开关关掉 `gpu_timing detail` 与 StatusLayer 再采一次，扣掉测量本身的成本（每帧 ~200 个 GPU zone、overlay 1.3 ms）。输出：按 self 时间的前 30 个符号，按调用链聚合到 `Rasterizer::Draw` / `BindResources` / `BufferCache::ObtainBuffer` / `TextureCache::FindImage` / `Image::Transit` / `Scheduler::BeginRendering` / `PipelineCache` / `Liverpool::ProcessGraphics` / `vkCmd*` 几个桶。
2. **可能的杠杆（按采样结果取舍，不预设结论）**：
   - 资源绑定：`Pipeline::BindResources` 每 draw 的 buffer/image 查找与 descriptor 写入（30 个 unified binding），改为按 shader 资源变化增量更新、绑定缓存按 (buffer id, offset) 命中。
   - BufferCache 区间查询：`ObtainBuffer` 的区间表与 256 KiB 分区锁，每 draw 多次；考虑 draw 级缓存与批量 `SynchronizeBuffer`。
   - 屏障与 pass 账本：`Image::Transit` / `RenderBreak` 计数与 `RecordAttachmentDraw` 在热路径上的成本；把诊断计数压到原子加法之外的采样。
   - PM4 解码：`Liverpool::ProcessGraphics` 对 SET_CONTEXT_REG/SET_SH_REG 的逐 dword 处理与 `sceGnmSetPs/VsShader` 写入的重复解析。
   - Turnip 侧 `vkCmd*` 成本：若 vkCmdBindDescriptorSets / vkCmdPushConstants 占比高，改 `VK_KHR_push_descriptor` 或 descriptor buffer（Turnip 已报 `VK_EXT_descriptor_buffer`）。
3. **不做的**：不先做"PM4 解码与 Vulkan 录制分线程"的大重构；先把单线程成本降到采样显示的可压缩部分再评估。
4. 验收：同指纹（~1490 draws/帧）锁频 3×10 s FPS；PROF 中 `PM4.Resume` 每帧均值 ≤ 35 ms 且 GpuComm 未注解时间不增加；`gpu_memory status` 的 pass/draw 计数不变（不能靠少画东西）。

### P2：剩余同步 HLE（2420 → ≤ 800 次/帧）

按 run S 的每帧计数排序：

1. **rwlock 读锁/解锁 1138 次/帧**：沿用 `guest/runtime/sync` 的方式给 `scePthreadRwlockRdlock/Unlock`（及 POSIX 同名）做 guest 快路径——读者计数字 + 写者位，无写者时 CAS 增减不出 guest；写锁、超时、有写者等待时回退现有 HLE。host `GuestRwlockDomain` 改为读写同一 guest 字（和 mutex 一样只保留 prefix 状态）。
2. **kernel semaphore 434 次/帧**：`sceKernelWaitSema` 计数 >0 时 guest CAS 递减、`SignalSema` 无等待者时 guest 递增；有等待者/超时/Poll 语义回退 HLE。需要先确认 Orbis 信号量对象在 host 侧的表示能放进 guest 可读前缀。
3. **mutex Init/Destroy/Config 210 次/帧**：游戏每帧建销 44 个 mutex。guest 侧建对象要在 arena 里分配，第一版不做；先把 `Mutex.Config`（attr settype/protocol 3 次/对象）合并进 Init 的一次 HLE。
4. **cond broadcast 178 次/帧**：Bionic seq 协议进 guest（Tier B 二期原计划）。
5. 验收：`hle_sync` 20 s 窗口按 presents 归一，rwlock 行 <50/帧、Sema 行 <150/帧；`guest_sync_fastpath_tests` 扩展到 rwlock/sema 的 host↔guest 交接、取消、销毁用例并在真机 29+N/0；血源同指纹 FPS 不降。

### P3：Guest-1 的 GNM 小 HLE（447 次/帧 ≈ 2.6 ms）

- `sceGnmSetPsShader/VsShader/CsShader/LsShader/HsShader`（249 次/帧）是纯 PM4 写入；`sceGnmIsUserPaEnabled`（204 次/帧）是常量返回。用与 mutex 相同的 app-shipped guest payload 机制加一个 `guest/runtime/gnm` payload：guest 内直接写 dcb（复用桌面 `gnmdriver.cpp` 的包格式，含 `sceGnmSetPsShader350` 的 shader modifier 分支），常量查询直接返回。
- 验收：Guest-1 这些 HLE 行消失；`gpu_memory`/PM4 消费计数与画面不变；不改任何游戏专用逻辑。

### P4：GPU 每帧 48.6 ms

1. 两个 ≥5 ms 的 pass（G-buffer、光照，各约 7 ms）在 960×540 上的 tiler 成本：用 `gpu_timing detail` + RenderDoc 单帧确认是 bin 数量还是 shader；候选是 `VK_QCOM_render_pass_shader_resolve`/`VK_EXT_rasterization_order_attachment_access` 让光照直接读 G-buffer（PS4 上本来也是同一帧的相邻 pass），减少 attachment load/store。
2. HLE 拷贝把大 pass 拆开（重开 23/帧，G-buffer 2→8、光照 6.5→15.8）：`ExecuteCopyShaderHLE` 把 `vkCmdCopyBuffer` 延后到当前 pass 自然结束，条件是后续 draw 不绑定与 dst 重叠、不写 src（BufferCache 已有每 draw 的绑定范围）；任何 dispatch/download/upload 触及范围就立即冲刷。
3. `sampled_image` 重开 6/帧：写完就被采样的目标在 `EndRendering` 处直接转到 `ShaderReadOnly`（按图像的"上帧被采样"历史决定），猜错的代价只是 pass 边界一次额外屏障。
4. HostReadback 5.3 ms/帧：融合回读的 dispatch 本身 ~1.7 ms，其余是依赖排空；只能通过让后处理直接读 image（需要 guest 侧改变，不做）或把三张回读合并到一次 dispatch 减少排空次数。
5. Overlay 1.3 ms + Present 1.4 ms：StatusLayer 关闭时应为 0，作为测量修正项记录。
6. 验收：`GPU.GuestCommands` ≤ 40 ms/帧、resumed 实例 ≤ 10/帧，画面用 DumpLayer 与 run S 对照。

### P5：待查缺陷（不阻塞，但要登记）

- 服务分配起点挪动 256 MiB → 血源启动空指针（`fex-fault-27056-1.txt`）：可能的 32 位截断（RSI = RDI 低 32 位）或对栈/TLS 地址上限的假设；用 guest debugger 在 `0x1020b6ac2` 下断，拿到出错前的最后一次 HLE 返回值。
- 标题菜单上施加 DumpLayer 预设与 `SEGV_ACCERR fault 0x24d3c0040`（Guest-19）同秒：GPU 写保护页与 surface 重建的竞争，需在 fulldump 关闭的机器上另想办法拿栈（`debuggerd -b` 或 host LLDB attach）。
- 血源 `Sema.Wait` 单次最长 8.9 s、`Cond.Wait` 1.6 s：加载期正常，但要确认不是 P2 改动后的新等待。

## 2. 测量与工具要求

- 每轮都记录 `hle_sync status` 的 `fast_path`、`gpu_memory status` 的 draws/帧指纹（~1490 重 / ~665 轻 / ~1083 桌前）、锁频操作 id 与 baseline；`device_frequency_stop` 出现 Conflict/Failed 要重试并记录。
- 有存档后用 `tools/launch_bb.sh`（卡片 → Launch）+ 圈键 intro 循环；圈键在游戏内是翻滚，到达诊所后立即停止循环（`present` 速率在 15 FPS 以上时旧的"<15 就停"判据失效，改用 StatusLayer `Guest flip` 或 `guest_flip` 计数差分）。
- 屏幕核对用 `pico_capture_dump_layer` + `tools/panel_crop.py`；先确认 `debug.spruntime.etfr.subsample=0`。
- simpleperf、PROF、hle_sync 三者同窗；KGSL 采集在 Swan 仍受 1.5 GiB MemAvailable 守卫限制。
