# Swan TMNT 屋顶基线与瓶颈归因（2026-09-23）

血源在 Swan 上内存压力过大（launcher 被杀、游戏丢 top-app、lmkd 把进程写进 zram，见 [血源合并版归因](swan-clinic-merged-deep-20260923.md) §2），用户决定改用 TMNT 做 Swan 性能基准。构建与血源一轮相同：3f0c47ea，APK 30733386 / host 93464ad4，mainline Turnip 86ca472f，Render 0.5 / Texture medium。证据在 [evidence/swan-tmnt-roof-20260923/](evidence/swan-tmnt-roof-20260923/)。

## 1. 结论

1. **基线**：TMNT CUSA50828 v1.11.0 教程屋顶起点，Leonardo 原地待机，锁频 `swan-evt-legacy`（CPU 1.90/1.50 GHz、GPU 726 MHz）、cpuset 全程 `/top-app`：**19.66 / 19.66 / 19.50 FPS**（帧约 51 ms）；detail 插桩 19.24 FPS，关闭后 19.76 / 19.68。场景指纹 419 draws/帧、53 个 pass 实例/帧、28 次 dispatch 打断/帧。
2. **TMNT 是 GPU 瓶颈，和血源正相反**。整个进程只用 0.85 个核（GpuComm 运行 31%、Guest-1 20.5%，其余在睡）；锁频结束后 GPU 在 902 MHz 下忙碌 98–99%，解锁后约 25 FPS，与 GPU 频率比 1.24 吻合。
3. **GPU 每帧 40.5 ms 的游戏命令里，`HostTransfer` 占 24.2 ms（每帧 75.5 次）**，游戏自身的 render pass 只有 9.6 ms（44.5 个实例）、dispatch 6.2 ms；另有 present 1.5、PostProcess 0.6、HostPrepare 0.6 ms。帧长与这些打点之间还有约 9 ms，可能是 Pico 合成器等其它 GPU 上下文或未打点的工作（`gpu_busy_percentage` 统计的是整颗 GPU）。
4. **HostTransfer 的来源是每帧重传的缩放纹理**：`gpu_memory` 差分为每帧新建 26 张 `image/upload-source`、共 **115 MB**（全部跟踪分配每帧新建并释放 58 个、240 MB）。路径是 `TextureCache::RefreshImage` → `ObtainBufferForImage`（整段 guest 内存上传）→ `TileManager::DetileImage` → `Image::Upload`：对 scaled 图像每次新建一张原尺寸临时 image（VMA 分配）、`copyBufferToImage`，再 `BlitBacking` 缩到 0.5 倍后退役。`RefreshImage` 只对"GPU 写过"的图先比内容哈希跳过未变化的 mip；普通资产一旦被判 CPU 脏就整张重传。§6–§8 的定向诊断确认：不是页级写跟踪误判，而是游戏每帧用 Gnmx 工具库的 compute 填充内核把 32 张渲染目标**清成纯色**，模拟器把这次清除当作"buffer 内容变了"，整张走上传链路重传一张纯色图——这笔开销没有意义。§8 的修复把它换成直接清图：锁频 **19.7–20.6 → 59.6 FPS（60 帧封顶）**。
5. **等待链都是 GPU 积压的表现**：
   - Present 线程 `Present.DriverCall` 每帧 50.6 ms。off-CPU 调用栈是 `Swapchain::Present` → `Surface::queueBuffer` → `BufferQueueProducer::queueBuffer` → `Fence::waitForever` → `sync_wait`：Android BufferQueue 对 EGL 连接的生产者（Vulkan loader 也按 EGL 连接）做节流，要等上一帧 buffer 的 fence。唤醒它的是 `kgsl_hwsched`，也就是 GPU 完成。
   - 这段等待期间 Present 持有 `QueueMutex`，VkSubmit worker 在 `Scheduler::SubmitExecution` 上等这把锁，每帧 35.9 ms（off-CPU 的 60%）。
   - GpuComm 在 `PM4.WaitVideoOutLabel` 上每帧等 35.4 ms，Guest-1 在 `GNM.SubmissionGate` 上等 40.2 ms。
   - GPU 忙碌率 98–99%，说明锁并没有让 GPU 空转；GPU 负载降下来之后，这把锁会变成下一个问题。
6. **CPU 侧与血源同一类热点，但不在关键路径上**：GpuComm 每帧约 16 ms on-CPU，其中 `PipelineCache::GetGraphicsPipeline` 31%、SRT 扁平化（`RefreshFlatBuf`）20.5%。另外 off-CPU 采样看到 GpuComm 线程读 `/proc/meminfo`（内核 `dma_buf_total_size`），是状态层"System RAM"读数，放在渲染线程上不合适。
7. **内存**：TMNT 在屋顶总 PSS 约 5.1 GB（Graphics 2.9 GB、Native 0.73 GB），系统 RAM 已用 13.6–13.7/15.4 GB，MemAvailable 1.6–1.9 GB。比血源轻，但同样处在 lmkd 的压力区间。

## 2. 下一步（TMNT）

按预期收益：

1. **定向诊断每帧 26 次缩放重传**：已完成，见 §6–§7——来源是 GPU 清屏内核，不是误判。
2. **compute 填充改直接清图**：已完成，见 §8，屋顶从 GPU 瓶颈变成 60 帧封顶。
3. ~~内容哈希拦截~~：清屏写在 GPU 侧 buffer，guest 内存哈希用不上；§8 已从源头去掉这些重传，不再需要。
4. **融合上传**降为次要：本场景已无重传；其它场景里真正由 CPU 或非清除着色器写出的纹理仍走原尺寸临时图 + blit，留作后续（[Internal scale 内存诊断](internal-scale-memory-20260919.md) 已登记）。
5. **Present 缩短持锁**：进入 `QueueMutex` 前先在锁外等上一帧的 `present_done`，让 `queueBuffer` 的节流等待不在锁内发生；保留提交与 present 的同步语义，不另开 present 线程。修复后 GPU 仍约 90% 忙碌，需在未封顶的场景复测是否还有收益。
6. 状态层的 `/proc/meminfo` 采样移出 GpuComm，或降低采样频率。
7. 血源 spec 里的 SRT 批量读同样适用于 TMNT 的 CPU 侧；屋顶现在封顶 60 帧，需换更重的场景（战斗、多角色）才能看出 CPU 侧收益。
8. 同一 Gnmx 填充内核在血源等其它游戏里是否出现、命中率多少，用 `upload_diag status` 的 `compute_fill` 行看。

验收仍按同一口径：屋顶起点、角色不动、锁频、top-app，3×10 s FPS，同时看 `scale_upload_created_count` 和 `GPU.HostTransfer` 每帧值；屋顶已到 60 帧上限，之后需要选不封顶的场景做基准。

## 3. 过程与身份

| 项 | 值 |
|---|---|
| 进入方式 | Library 卡片（中心约 1195,553）→ Launch（1271,618）→ 使用条款（用户在头显内确认）→ 主菜单"开始游戏"→"跳过教程？"选"开始游戏"→ 屋顶；输入走 DebugBus 手柄（`tools/pad.sh`，[协议](../../debugbus-pad.md)） |
| 会话 | pid 31682，generation 1，run_uuid 1a071bdd…；进屋顶时 Pico 设置面板（关于本机）拿走焦点，游戏弹"控制器断开"并降到 `/foreground`，用户关掉面板后恢复，之后全程 `/top-app`（`cpuset-log.txt`） |
| 锁频 | 操作 1d3957f5（先对上一轮 a2fa32fa 再收尾一次，Cleaned）；这次记录的 `policy0/scaling_max_freq` 基线已是 1900800，所以解锁后 policy0 仍是 min=max=1.9 GHz，未手改 sysfs |
| 采集 | 3×10 s FPS → 同窗 sched（独立 tracefs 256 MiB 22 s，0 overrun）+ PROF capture 1 + simpleperf 2000 Hz 20 s（33,521 样本 0 丢失）→ detail（`gpu_timing detail` + `hle_sync start 1`，PROF capture 2，GPU zone 60,342 个、0 丢弃）→ 关闭后 2×10 s → off-CPU simpleperf 10 s（Present/VkSubmit/GpuComm）→ 解锁后 GPU 忙碌率 10×1 s |

## 4. 局限

- GPU 忙碌率是在解锁后（902 MHz）采的，锁频状态下没有单独采；锁频与不锁频的 FPS 比例与频率比一致，支持"GPU 瓶颈"的判断，但不是同一窗口。
- GPU 时间戳未与 CPU 校准；GPU 忙碌率包含 Pico 合成器等其它上下文。
- 26 张/帧的身份与来源已由 §6 确认；§6 的诊断轮未锁频，只用于归因，不作性能对比。
- 屋顶起点只是教程场景的一个固定视角；战斗或移动时的负载另测。

## 5. 证据文件

`tmnt-roof-A/`：`fps.txt`、`fps-detail-window.txt`、`fps-post-detail.txt`、`cpuset-log.txt`、`snap0/1.txt`、`prof-results.txt`、`sched-states.txt`、`hle-sync-detail-start/end.txt`、`gpu-timing-status-detail.txt`、`perf-light/perf-buckets.txt`、`subtree-transfer.txt`、`offcpu-stacks.txt`、`offcpu-present-stack.txt`、`threads.txt`，以及屋顶/菜单截图。PROF、`perf.data`、`offcpu.data`、sched 原始 trace 只记 SHA。新工具 `pad.sh`（DebugBus 手柄）与 `panel_auto.py`（不受头显朝向影响的面板裁剪）在 `tools/`。

## 6. T1 定向诊断结果（2026-09-23）

新增默认关闭的 `upload_diag`（`src/video_core/texture_cache/upload_diagnostics.{h,cpp}`，DebugBus `upload_diag start [行数] | status | stop`）。开启后在四个脏标记入口记录来源——CPU 写缺页（1/8 字节探针）、HLE 宿主写（精确范围）、单页共享（交给哈希判定）、GPU storage buffer 写、CP/DMA 拷贝——并在 `RefreshImage` 重传前对要上传的每个 mip 求 guest 内容哈希、与该图上一次上传比较；如果 buffer cache 里的字节比 guest 内存新（`gpu_resident`），就不做哈希，只记来源。关闭时每条路径只多一次 relaxed 原子读。逐次日志有行数上限，另有按图汇总。

构建 APK bdc171b1 / host 9c5feea7（3f0c47ea + 本诊断），同一屋顶起点（pid 15819，cpuset `/top-app`，未锁频，诊断本身不做性能结论）。10 秒、274 帧：

- **8,768 次重传 = 每帧 32 次、32 张图，全部来源是 `gpu_storage_write`，全部 `gpu_resident`**；逐次日志 544/544 条的写入范围都从图起点开始并覆盖整张图，标志为 `GR`（既作为渲染目标写过，又被 storage buffer 写过）。
- 每帧 guest 字节 125 MB：缩放图 26 次/116.8 MB（与第 1 节 `scale_upload` 的 115 MB 一致），原尺寸 6 次/8.5 MB。
- 大头是 1920×1080 目标：R8G8B8A8Unorm 每帧 6 次（50.1 MB，其中 0x246248000 一张每帧 3 次）、R16G16B16A16Sfloat 2 次（33.4 MB）、R32Sfloat/R16Sfloat/R8Unorm 各 1 次，另有 1 张 R8G8B8A8Srgb tile10（三张显示缓冲轮流，每帧 1 次）。其余是 2048² R8、960×540 与 512²/128² 的 R8 小图。

结论（当时）：**不是 CPU 页级写跟踪的误判**。着色器（`Rasterizer` 绑定时 `desc.is_written && desc.is_formatted`）把这些渲染目标的内存当作格式化 storage buffer 整张写，`InvalidateMemoryFromGPU` 置 `GpuDirty`；下一次按纹理采样时只能从 buffer cache 整张重传：解平铺 dispatch → 新建原尺寸临时图 → `copyBufferToImage` → `BlitBacking` 缩到 0.5 倍。PS4 上两者是同一块内存，没有这笔开销。

当时据此把"融合上传"列为主杠杆，并写了"GPU 每帧真实产生的内容"。这一句下得太早：写入是真实的，但写的是什么没有查。用户指出这种重传没有意义、内容来自原资源本身，§7 的追查证实写入的只是清屏纯色，上面的判断由 §7–§8 取代。

证据：`tmnt-roof-B-uploaddiag/` 下 `upload-diag-status.txt`（汇总）、`upload-diag-log.txt`（前 600 条逐次记录）、`property.txt`、屋顶截图。

## 7. 写入者与写入内容（2026-09-23）

**只跳过脏标记会花屏**。诊断加了一个仅供 A/B 的开关 `upload_diag ignore_storage_dirty on`：已渲染过的图在被 storage buffer 写时不置 `GpuDirty`。同一会话锁频（APK 7b21d9f4 / host 4e7c5415，pid 18160）：关 21.66、开 **52.72**、再关 21.74 FPS，开启时每帧重传 0 次，但左上出现一块边缘生硬的亮色方块（`tmnt-roof-C-ab/B-panel.png`、`A-over-B.png`）。写入的内容确实被画面用到，不能直接丢。

**唯一的写入者是一个 19 条指令的 compute 内核**（`pgm_hash 0x934eeac`，`upload_diag` 按写入着色器汇总 3483/3483）。用 `debug.shadps4.shader_dump=1` 取出 GCN 二进制和 SPIR-V（`tmnt-roof-C-ab/shaders/`），按 CI 编码逐条核对：

```
i = tgid.x * 64 + tid.x
if (i < ctl[0] && i < dst.num_records)      // ctl 由 s_buffer_load 读 [0]=count, [1]=mask
    dst[i] = src[ctl[1] & i]                // buffer_load_format_x / buffer_store_format_x，32 位 Uint
                                            // 越界的 src 读返回 0
```

形态与 Sony Gnmx 工具库清渲染目标/清 buffer 的填充内核一致（据指令和参数推断，未对照 SDK 源码）：`src` 是 1–2 个 dword 的图案，`mask = 图案长度 − 1`。

**每次 dispatch 写的是什么**：诊断再加一条默认关闭、有上限的逐 dispatch 记录（每个 buffer 绑定的地址/大小/格式、前 4 个 dword、重叠的缓存图）。APK 373b8f22 / host caaaa8bd（pid 19235）记录 400 次，全部是这个内核，全部是"整张图纯色填充"：

| 目标 | 图案（mask） | 含义 |
|---|---|---|
| R8G8B8A8Unorm 1920×1080（多张） | `00000000`（0）或 `ff000000`（0） | 透明黑 / 不透明黑 |
| R8G8B8A8Srgb 1920×1080 tile10（显示缓冲） | `ff000000`（0） | 不透明黑 |
| R16G16B16A16Sfloat 1920×1080 ×2 | `00000000 3c000000`（1） | (0, 0, 0, 1.0) |
| R16Sfloat / R8Unorm 1920×1080、R8G8B8A8 960×540 | `00000000`（0） | 0 |
| R32Sfloat 1920×1080 | 源为空（records=0），mask `ffffffff` | 越界读 → 0 |
| R8Unorm 2048²/512²/256² | `ffffffff`（0） | 1.0 |
| R8Unorm 960×540、128²、64²、32²、16² | `00000000`（0） | 0 |

`count × 4` 恰好等于图的 `guest_size`。所以每帧 32 次重传传的是 32 张纯色图，共约 127 MB：先由内核把颜色写进 buffer，再 buffer → 解平铺 → 原尺寸临时图 → blit 到 0.5 倍 backing。一次 `vkCmdClearColorImage` 就能得到同样的结果。

## 8. 修复：compute 填充改为直接清图（2026-09-23）

实现（`Rasterizer::TryComputeImageFill`、`TextureCache::ClearImagesForFill`）：

- **精确识别**：只对 3 个 buffer 绑定（控制块非格式化只读、源格式化只读、目标格式化可写）、无 image 绑定、`num_thread_x = 64`、`start_x = 0` 的直接 dispatch，再把程序地址处的 19 个 dword 与上面的内核逐字节比对。不看游戏、不看哈希，语义由指令本身确定；若它确是 SDK 工具库内核，其它用同版本工具库的游戏会同样命中（未验证）。间接 dispatch 不处理。
- **按语义取值**：目标与源都必须是 `Format32` 的 Uint/Sint、stride 4、无 swizzle/add_tid（逐位拷贝）；`ctl` 与图案用 `TryReadSrtMemory` 读，若 buffer cache 里这些字节是本次提交里 GPU 写的（`IsRegionGpuModified`）就不处理。元素数取 `min(count, dst.num_records, 实际线程数)`。图案周期为 `mask + 1`（须是 ≤16 的 2 的幂）；源为空时所有读返回 0，按零填充；其它情况（下标跟随 i，即拷贝）不处理。
- **清图条件**：填充范围内所有重叠的缓存图都必须整张落在范围内、起点对齐图案周期、非深度/非压缩/非多重采样/非 ASTC；每个 texel 起点都是 texel 大小的整数倍（任何分块方式都如此），所以图案周期与 texel 大小整除且各 texel 字节相同时，整张图就是同一个 texel。texel 按 backing 格式换成精确的清除值：UNORM8/16 用 k/255、k/65535，SFLOAT16 逐位展开，32 位格式直接拷位，8/16 位 UINT 取整数；sRGB 只接受颜色通道为 0 或 255（驱动会把线性值重新编码，只有这两个值往返精确）；其它格式只接受全零。任一图不满足就整次不处理，照原路径 dispatch。
- **结果**：`Image::Clear` 清整张（所有 mip/layer，含 scaled backing），标 `GpuModified`、去掉脏标记，跳过 dispatch——和渲染目标被 draw 写过后的状态一致。guest 内存与 buffer 里的这段不再同步写入，这与渲染目标一贯的处理相同：之后以格式化只读 buffer 读这段内存时，`SynchronizeBufferFromImage` 找到这张 `GpuModified` 且不脏的图并从图回读；非格式化 storage 读或 CPU 读看不到清除值，与 draw 写过的渲染目标是同一限制。
- **诊断与开关**：`upload_diag status` 常驻输出 `compute_fill:` 行（清掉的图数、字节、各种回退原因计数，每次填充 dispatch 几个 relaxed 原子加）；`upload_diag fill_clear off|on` 可在同一会话里切回旧路径做 A/B。
- **StatusLayer 常驻显示**：`ScaleCoverageCounters` 新增 `image_uploads / image_upload_bytes / fill_clears`（`RefreshImage` 每次真正上传加一次、字节为所传 mip 的 guest 大小；清图路径按清掉的图数累加，均为 relaxed 原子），状态层按 500 ms 窗口折算成每 guest 帧：`Re-uploads N/frame (X MB)  fill clears M`，每帧 ≥4 次重传显示为橙色；没有 guest flip 时改为每秒。`gpu_memory status` 同步输出 `texture_uploads count= bytes= fill_clears=`。APK cd795a87 / host 781b9693（pid 32548）屋顶实拍：开启 `Re-uploads 0.0/frame (0.0 MB)  fill clears 32.8`，FPS 59；切到 off 后橙色 `Re-uploads 30.5/frame (115.1 MB)  fill clears 0.0`，FPS 23（未锁频），截图 `tmnt-roof-F-statuslayer/on-status.png`、`off-status.png`，已切回 on。

验证（APK c937a218 / host 11f4b27b，pid 25840，屋顶起点，锁频 `swan-evt-legacy`，全程 `/top-app`、焦点在游戏）：

| 轮次 | fill_clear | FPS（10 s） | 重传/帧 |
|---|---|---|---|
| A | off | 19.69 | 32.5 |
| B | on | **59.64** | 0 |
| C | off | 20.01 | 32.3 |
| D | on | **59.57** | 0 |
| 移动到攻击提示处：G | on | **59.60** | 0 |
| H | off | 20.59 | 32.5 |
| I | on | **59.66** | 0 |

- 开启时每帧约 32.6 张图被直接清除（B 轮 19,777 张/607 帧），约 2 次/帧是不落在缓存图上的普通 buffer 填充、照常 dispatch；加载阶段有 2 次"部分覆盖"回退。`pattern / format / gpu_resident` 回退为 0。
- 60 FPS 是呈现上限；解锁后同一位置 GPU 忙碌约 90%（修复前 98–99%，约 20 FPS）。
- 画面：开/关两张屋顶截图一致（差异只是雨和雾的动画，`on-vs-off.png`）；Leonardo 移动、镜头跟随、教程提示从"移动"切到"攻击"后画面正常（`F-on-moved-wide.png`）。只看了 DumpLayer 可见的左侧面板区域，没有做逐像素 GPU 读回对比。
- 局限：只验证了 TMNT 教程屋顶这一段；其它关卡、其它游戏（血源也用 Gnmx）未测。收尾第一次 stop 时 `gpu_max_clock` 写回 902 读到 826（工具判 Failed、未强写，疑温控），约 5 分钟后再次 stop 已读回 902、操作 Cleaned。

证据：`tmnt-roof-C-ab/`（ignore 开关 A/B 与截图、内核 GCN/SPIR-V/反编译 GLSL）、`tmnt-roof-D-dispatch/`（逐 dispatch 记录 `run-lines.txt`、汇总）、`tmnt-roof-E-fill/`（各轮 `ab-fps.txt`、`*-fill-before/after.txt`、`*-upload-diag.txt`、`*-gpu-memory.txt`、截图）。工具 `tools/ab_phase.sh`、`tools/ab_fill.sh`。
