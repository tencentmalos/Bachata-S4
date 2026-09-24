# Swan 血源：录制线程、pass 提前与 tile 流量（2026-09-24）

分支 `feature/malos/swan_performance`，代码提交 169a0b32（基于 e206e81e）。设备 Swan PB3110PGL6240001G，Turnip mainline（Mesa 26.3.0-devel 86ca472fc2），血源 CUSA03023，诊所内部视角（右摇杆转入，约 1500–1600 draws/帧）。证据在 [evidence/swan-bloodborne-20260924/](evidence/swan-bloodborne-20260924/)。

## 1. 结论

- **出厂频率下诊所是 GPU 受限**：不锁频时 `gpubusy` 97–99%（902 MHz），约 23 FPS；0.25 scale、锁频（GPU 726 MHz）时仍有约 92%。血源这个场景的 GPU 开销大部分与分辨率无关，切 0.25 解不开 GPU 瓶颈，所以 CPU 端改动在帧率上看不出来，需要用线程 on-CPU 时间衡量。
- **录制线程**把 GpuComm 从 44.2 降到 33.1 ms/帧（−25%），录制线程承担 10.8 ms/帧，总 CPU 基本不变。帧率不变（GPU 受限）。
- **GPU 时间分布**（0.5，不锁频，15 s PROF，`gpu_timing detail`）：GuestCommands 40.1 ms/帧，其中 render pass 32.6 ms（81%，272 个实例），dispatch 2.4，回读 2.1，上传/传输 0.9，其余约 2.1。pass 耗时分散：<50 µs 的 189 个/帧只占 2.6 ms；≥5 ms 的约 1.1 个/帧占 6.3 ms（19%）；0.05–0.5 ms 的 74 个/帧占 12.4 ms（38%）。
- **tile 流量**（新计数）：每帧 277 个 pass，load 448 个附件（157 MPix），clear 6，store 455（164 MPix）；177 个 pass（64%）只有 1 个 draw。模拟器强制切断后重开的 pass 约 38 个/帧，主要是 G-buffer（7 附件）等 960×540 目标，原因中 HLE 拷贝约 63%。
- **pass 提前（hoist）**：HLE 拷贝不再切断 pass，每帧 44–45 次提前、1.1 次因真实依赖保留切断；tile load 438–448 → 359–366（−19%），load 像素 159–163 → 107–108 MPix（−34%），pass 268–273 → 250–259。帧率 22.04/22.63 → 22.78/22.87（+1–3%），GPU 仍 98%：tile load/store 不是 GPU 时间的大头（Turnip 对小 pass 可能走 sysmem）。
- **HLE region 合并**：每帧 4206 → 496 个 region（字节不变，992 KB/帧），GPU 忙碌率约 −1.8 个百分点，帧率在噪声内。

## 2. 实现

### 2.1 录制线程（`vk_command_recorder.{h,cpp}`）

- Citron/yuzu 的 CommandChunk + worker 方案。`Scheduler::CommandBuffer()` 返回 `RecordingCommandBuffer`：方法与 `vk::CommandBuffer` 同名，关闭时直接转发；开启时把参数（数组、结构体指针深拷贝）写入 128 KiB 命令块（前段放命令、后段放数据），由 `shadPS4:VkRecord` 按顺序重放到真实 cmdbuf。
- `RawCommandBuffer()`（Presenter、GPU profiler、ASTC 编码器、host pass）先排空 worker，并在下一个 cmdbuf 之前让后续命令立即录制，保证与已延迟命令的相对顺序。每帧约 1.2 次（主要是 Presenter）。
- `SubmitExecution` 在 `end()` 前排空 worker；worker 与 GpuComm 不会同时使用 cmdbuf/command pool。worker 异常在下一次 Dispatch/Sync 转回 GpuComm。资源销毁均走 `DeferOperation`（等该 tick 的 GPU 完成），晚于录制与提交。
- 默认关闭，`vk_recorder on|off` 在下一次提交时生效；`vk_recorder status` 输出 chunks/commands/syncs/raw_syncs/held_passes/hold_overflows。

### 2.2 pass 提前（frame graph 第一步）

- **访问集**：`BufferCache::ObtainBuffer`（非 stream 路径）、纹理绑定（采样读，存储写）、颜色/深度附件（写）调用 `Scheduler::StageAccess`；按 draw 暂存，`NoteDraw` 时并入该 draw 所在 pass（`Draw` 是先 `BindResources` 后 `BeginRendering`，所以不能直接记到当时打开的 pass 上）。stream 路径的小 UBO 是录制时的 CPU 快照，之后的 GPU 写影响不到它，不计入。
- **持有 pass**：`BeginRendering` 时录制器先把 pass 之前的命令交出，然后持有 pass 内命令（最多 8 块，超出则释放并不再提前）。
- **pass 前插槽**：`Scheduler::BeginHoist(reads, writes, reads_written)` 判定操作写的范围与 pass 已有读写不相交、读的范围与 pass 已有写不相交（若源需要先上传，读也按写算），通过则后续命令进入插槽，`EndRendering` 在此期间为空操作，`EndHoist` 结束。pass 结束时按"插槽 → pass"顺序交给 worker。
- **中断**：操作执行中遇到提交或借出真实 cmdbuf，先结束持有的 pass：已录部分在 pass 前、剩余在 pass 后，二者对一个与 pass 无依赖的操作都合法，操作内部顺序不变（`interrupted` 计数）。插槽录制期间命令块溢出不释放持有。
- 首个接入点：HLE copy shader，逐 region 检查。只在录制器开启时生效；`vk_recorder hoist on|off`；`gpu_memory status` 的 `pass_hoist` 行与 StatusLayer `Hoisted/frame`。

### 2.3 统计与小修

- `render_pass_stats`：每个 pass 的 load/clear/store 附件数与像素、按 draw 数分桶（0/1/2–7/8+）、hoist 计数；`gpu_memory status` 的 `tile_traffic`/`pass_hoist` 行，StatusLayer `Tiles/frame`、`Passes/frame`（单 draw pass 过半时橙色）。像素为 width×height×layers，计的是请求的 load/store，不等于 GMEM 实际流量（驱动可能选 sysmem）。
- HLE copy：源、目标都首尾相接的 region 合并（`pm4_stats hle_merge on|off`）。
- PM4 进度诊断改为每 64 包或协程挂起时发布一次（原每包一次 `clock_gettime`，约占 GpuComm 3.4%）。
- `StreamBuffer::Copy` 合并两次映射校验（`MemoryManager::TryCopySparseMemory`），`WaitPendingOperations` 对已完成 tick 不再调用 `Scheduler::Wait`。
- `pm4_stats`（默认关闭）：DCB 结构（每帧约 12 次提交、74 个 DCB，单次提交最多 42 个；DCB 开头自带 120–190 个 context 寄存器，符合多线程各持 context 的结构）、stream 拷贝（每帧 7887 次/6.4 MB，同帧内容不变的重复 49%）、HLE 拷贝统计。

## 3. 实测

| 项目 | 条件 | 关 | 开 |
|---|---|---|---|
| 录制线程 | 0.25，锁频，3 轮 | GpuComm 44.2 ms/帧，18.73–18.98 FPS | GpuComm 33.1 + 录制 10.8 ms/帧，18.86–18.99 FPS |
| 录制线程 | 0.25，锁频，8 轮 FPS | 均值 18.90 | 均值 19.24 |
| HLE region 合并 | 0.5，锁频，4 轮 | 4206 region/帧，18.59 FPS，GPU 97.7% | 496 region/帧，18.51 FPS，GPU 95.9% |
| pass 提前（录制器开） | 0.5，不锁频，2 轮 | 438–448 load/帧，159–163 MPix，22.04/22.63 FPS | 359–366 load/帧，107–108 MPix，22.78/22.87 FPS |

- **锁频与不锁频的数字不可直接比较**：18–19 FPS 的组是锁频（GPU 726 MHz），22–23 FPS 的组是不锁频（GPU 902 MHz）。GPU 忙碌约 98% 时帧率约与 GPU 频率成正比（902/726≈1.24），这部分差距不是代码带来的；本轮代码的帧率收益只有 hoist 同会话 +1–3%。
- 画面：录制器开/关、hoist 开的截图与旧版（run Z4）一致；角色背后亮区是灯光照射衬衫，旧版同样存在。
- 测量陷阱：一次 A/B 中外部启动了 `org.xr3ds.xr3ds.relWithDebInfo`（Citra VR）抢走前台，游戏降到 `/foreground`→`/background`、会话被取消，该组数据作废；经用户同意强停后重测。锁频最后一次操作因系统温控把 `policy0/scaling_max_freq` 限在 2227200/2611200 而停在 CleanupPending，需系统放开后再收尾。
- 设置文件：测试期间临时改 0.25，已逐字节恢复（SHA 47d1d96c，341 B）。

## 4. 局限与下一步

- 录制线程与 hoist 默认关闭，只在血源诊所验证过；TMNT 等其他场景未验证。
- hoist 目前只接入 HLE 拷贝；dispatch、图像布局转换（sampled_image，约 16% 的切断）、buffer 上传还未接入。
- GPU 端下一步：用 RenderDoc 抓帧，按 pass/draw 看 0.05–0.5 ms 这批中等 pass 的 shader 与片元负载，判断瓶颈是像素、顶点还是个别昂贵 shader。
