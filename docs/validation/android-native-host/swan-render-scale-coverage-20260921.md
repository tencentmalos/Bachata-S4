# Swan 血源诊所：Render Scale 覆盖率根因与迭代（2026-09-21/22）

分支 `feature/malos/swan_performance`（自 `malos/main` 63b6d560 切出，2026-09-22 已 rebase 到 605053a6）。设备 Swan `PB3110PGL6240001G`（Pico B3110，Android 16 / API 36，4 KiB 页，Adreno 840v2，内核 6.12.58，`adb root` 可用），mainline Turnip `86ca472f`（driver sha `ea4853bf…`，`budget_reported=1`）。游戏 Bloodborne CUSA03023 诊所，Render 0.5 / Texture Medium。所有 FPS 均在 spatial-debug-tool `swan-evt-legacy` 锁频下测（cpu0–5 1.90 GHz、cpu6–7 1.50 GHz、GPU 726 MHz，600 s watchdog，每轮重锁）。证据目录 `build/validation/swan-baseline-20260921/`（小文件归档到 `evidence/swan-bloodborne-baseline-20260921/`，大文件只记 SHA）。

## 结论

1. **覆盖率问题已解决**：诊所 scaled draws 11.5% → **83.7%**、scaled passes 18.5% → **62.5%**；一次性 native 提升 47 → 7（剩余全是 1×1 D16 小图和一张 2048×2 R32F storage LUT），`mixed-pass` 级联 38 → 0；4096² D32 阴影图从永久 native 变为随渲染倍率缩放（2048²）。
2. **FPS 没有变化**：E 8.60/8.79/8.89 → F 8.74/8.96/8.88 → G 9.65/9.49/9.24 → H 9.36/9.44/9.58 → I 9.28/9.37/9.33（3×10 s 窗口，新进程或新 generation）。新会话的 GPU busy 仍是 97–99% @726 MHz，`GuestCommands` 每帧 ≈100 ms；同时 Guest-1 每帧 107 ms 全占——CPU 与 GPU 同时顶到帧时长（run G）。GPU 这 100 ms 的分解见 run I：**70% 在 render pass 内，但每帧 313 个 render pass 实例中只有 26 个是天然的附件切换，198 个是 barrier 硬切出来的**；tiler 每个实例的 bin + 附件 load/store 是与分辩率无关的固定开销，这才是 0.5 倍率省下像素后 GPU 不降的原因。
3. 根因不是 GC 内存压力（run D/E/F 均 `gc downloads=0 pressured_ticks=0`，used ≈ 3.1 GiB 远低于 pressure 6.6 GiB），而是一次**整图拷贝引发的一次性 native 提升级联**（下节）。
4. `VK_EXT_memory_budget` 无需改 Turnip：`tu_device.cc` 已宣告并实现（heapBudget = 90% × heap，heap = 75% 总 RAM ≈ 10.4 GB），shadPS4 侧 `budget_reported=1`；日志里的 "unavailable" 来自 Qualcomm 系统驱动实例。
5. 启机 2 GB 的来源见"启动内存"一节：guest 后备 memfd ≈ 1.2 GB（游戏加载写入）+ host 大块 `malloc` ≈ 640 MB（Session 初始化一步到位）；另有 ≈ 1 GiB 固定 Vulkan 池不计入 RSS。

## 根因（run E 逐次提升日志）

新增有界日志 `Internal scale: promote <reason> …`（每次一次性 native 提升一条，≤256 条）与 `Internal scale: native pass … native_attachment=<reason>[<index>] …`（每个被迫 native 的 pipeline 一条）。run E（`bb-clinic-E-promolog/scale-log.txt`）中级联的起点：

```
copy alias  1920x1080 R8G8B8A8Unorm 0x272300000  scaled=false history=Texture|copy-dst origin=Upload mask=Alias|InsufficientMips
copy alias  1920x1080 R8G8B8A8Unorm 0x26e090000  scaled=true  gpu_modified history=Texture|RT|copy-src origin=Render
native pass fs=0xbb10e440 attachments=7 native_attachment=alias[0] 0x26e090000   ← G-buffer MRT0
native pass fs=0x2390c887 attachments=8 native_attachment=alias[0] 0x26e090000
native pass fs=0xa291c498/… attachments=2–3 native_attachment=mixed-pass[depth] 0x24f3f0000   ×6
```

游戏用 compute 整块拷贝（`Rasterizer::IsComputeImageCopy` → `Image::CopyImage`）把 G-buffer MRT0 拷进一张 1920×1080 RGBA8 纹理；这张纹理首次被缓存看到时是 **1-mip 采样资产**（`InsufficientMips` → 按资产规则分配 native）。`Image::InheritCopyPlan` 因目标"已被采样 / 来源为上传"拒绝重规划 → 两端 `ForceNative("copy alias")` → MRT0 变 native → 混合 pass 规则把整个 7/8 附件的 G-buffer pass 提升 native（`reasons=80 = MixedPass|Alias` 的 55 万次 draw）→ 深度 0x24f3f0000 变 native → 共用该深度的 6 个后续 pass 依次提升。run E 共 38 次 `mixed attachment pass` 提升，其中 17 张 1920×1080。

另一个独立的 native 大户：4096² D32 阴影图首次以纹理身份出现，`Image` 构造对"资产域深度"直接 `RequireNative(SemanticNative)` 锁死 domain，之后作为深度目标也无法重规划（run D/E `reasons=4 scaled=0 4096x4096` 7.5 万次 draw / 5.1 万个 pass）。

## 实现（本轮改动，均在 `src/video_core`，`src/imgui`）

- `image.cpp` `InheritCopyPlan(source, whole_image)`：整图拷贝（`CopyImage/CopyImageWithBuffer` 传 `whole_image=true`）允许目标继承 **scaled** 计划，不再受 `sampled`/`origin` 限制（被拷贝的 level 全部被覆盖；目标 mip 数多于源时仍先 blit 保留旧内容）。反方向（已 scaled 且非全新的目标 ← native 源）显式拒绝，避免"重规划到 native → 下次作为附件又缩回"的 ping-pong；`DepthStencilCopy`/`Resolve` 是子范围拷贝，保持旧内容 blit。
- `image.cpp` `Image::BlitCopy`：同格式、同尺寸/类型/层数、单采样、非压缩、无 mip-drop/ASTC、格式支持 BlitSrc|BlitDst、aspect 一致（与 `CopyImage` 一样不拷 stencil）的跨倍率拷贝改走 `vkCmdBlitImage`，两端计划都不变；深度/整数格式 nearest，其余 linear（用 `instance->IsFormatSupported(format, SampledImageFilterLinear)` 判断，`format_features` 是 usage 要求掩码不能用）。计数 `scaled_blit_copies`。
- `image.cpp` 构造：资产域深度图只记 `reason=SemanticNative`，不锁 domain；首次深度目标使用经 `ObserveUsage` 按渲染倍率重规划。若 shader 用精确 texel 访问，`exact shader data access` 仍一次性提回 native。
- `image.cpp` `Image::Download`（上一轮）：scaled 后备通过临时 native 副本（linear/深度 nearest）回读，不再 `ForceNative("readback")`；`DownloadImageMemory` 同步去掉提升。GC 只在确实进入 pressure 时才回读 GpuModified 图像，run D/E/F 均未触发（`gc downloads=0`），保留原逻辑并加状态转换日志。
- `vk_rasterizer.cpp` `BindResources`：混合 pass 规则只在 `any_scaled` 时提升；全 native 的 pass 不再把 SizeProtect 小图标成 MixedPass。原因位 `NativePass{SideEffects,Msaa,Attachment,Mismatch}` 计数；storage-*buffer* 写入是否强制 native 由诊断开关 `debug.shadps4.scale_side_effect_passes=1` 控制（默认不变；run D/E/F `side_effects=0`，血源不受影响）。
- `scale_coverage.h` / `texture_cache.*`：relaxed 原子覆盖率计数器（draws/passes/scaled/promotions/upscaled_readbacks/scaled_blit_copies/native_pass_*/gc_*），`gpu_memory status` 新增 `native_pass_causes …` 与 `gc downloads= frees= pressured_ticks= used_mib= trigger_mib= pressure_mib= critical_mib= budget_reported=` 两行。
- `status_layer.*`：`Scale x0.5  draws n/m  passes n/m`（500 ms 窗口，n/m 为 scaled/total；不足一半橙色）与 `native promotions +N | upscaled readbacks +N`。draw/pass 均按次数计，不按像素加权；"pass"是 `Scheduler::BeginRendering()` 真正开始的 dynamic rendering 实例，barrier/拷贝/compute 打断后重开会重复计数。
- 热路径：每 draw 只有 relaxed 原子加与既有的 `attachment_groups` 分组查找；日志/集合查找只在一次性提升或 pass 首次被迫 native 时发生。

## 实测

| run | APK / host | 条件 | FPS（3×10 s） | scaled draws | scaled passes | 提升 | 备注 |
|---|---|---|---|---|---|---|---|
| A 基线 | 上一轮 | PROF 47f25f86… | **6.8**（mean 147 ms，p90 376） | ≈9% | — | — | GPU busy 99% @726；DrawBatch 73 ms；Guest-19 每帧等 111 ms |
| D | 0be27d89 / 3a1f730f | 迭代 1（回读副本、计数、StatusLayer） | 9.35 / 8.97 / 8.86 | 275,527/2,088,641 = 13.2% | 134,267/540,347 = 24.8% | 62（alias 8、mixed 53） | `attachment=9 mismatch=9`，readbacks 3567；RSS 1.65 GB + swap 0.91 |
| E | dffb1fcb / a6d12c67 | 提升日志（rebase 后） | 8.60 / 8.79 / 8.89 | 191,122/1,658,978 = 11.5% | 62,723/338,929 = 18.5% | 47 | 种子定位；RSS 1.62 + swap 0.89，MemAvailable 0.62 GB |
| F-interrupted | 47561b84 / 3c04a67c | 修复，被 Citra VR 抢前台中断 | — | — | — | 7（alias 6 = 1×1 D16，storage 1） | 打断前 0 条 native pass，与 F 同结论 |
| **F** | 5bd0982e / 38d51a5f | 修复 + 复核修正 | **8.74 / 8.96 / 8.88** | **1,376,876/1,645,062 = 83.7%** | **208,930/334,368 = 62.5%** | **7**（mixed-pass 0） | readbacks 5833，blit_copies 0；4096² 深度 `scaled=1`；RSS 2.57 + swap 0.82，MemAvailable 0.95 GB |

F 剩余 native draw 组：32×32/64×64 SizeProtect（`reasons=2`，约 22 万次 draw，像素量可忽略）与几十次小图；无 `mixed-pass`/`alias` 1920×1080 组。

F 结束 3.5 h 后对同一进程采样（`gpubusy-clinic-stale-3h5-swapped.txt`）：GPU busy 76–82% @726 MHz、present 4.5/s，系统 swap 已用 7.9 GB、free 530 MB——只用于说明 GPU 不再饱和，不作 FPS 结论。

## 中断与失败记录（保留）

- run F 第 1 次：`org.xr3ds`（Citra VR）被 `adb shell`（uid 0）拉起并进入 VrActivity，shadPS4 容器转后台、Session 结束。日志/截图在 `bb-clinic-F-copyinherit-interrupted/`。
- run F 第 2 次：外部再次 `am start … open_last_game`（`startActivity … (has extras)` 16:48:06 设备时间）后进程 16409 `signal 9` 自杀重启；系统重启的 19492 在 critical memory pressure（Citra VR 同时常驻）下撞到 `Buffer::Create` 的 VMA 分配失败断言（`buffer.cpp:90`，tombstone_05）。这是真实的内存余量问题：血源 + Pico 系统栈（≈3 GB）+ 第三个 app 即越界。
- 被 TaskStop 的脚本残留进程在 F 第 3 次的 console 文件末尾多写了 2 行（`8 avail_kb=677120` / `done`），F 目录内的 fps/gpu-memory/memory/截图 mtime 03:02:42–03:03:33 UTC 与 F 自身 console 一致，未被污染。
- Litep `collect_evidence`/`capture_kgsl` 在 Swan 上被硬编码的 `MemAvailable−buffers ≥ 1.5 GiB` 守卫拒绝（血源常驻时只有 ≈1.1 GB），同窗 KGSL 无法采集；已列入 [Litep 工具缺口](../../specs/litep-perf-tooling-gaps-20260920.md) 待改为可配置。

## 启动内存归因（run F 启动采样，`bb-clinic-F-copyinherit/startup-mem/`）

进程出现后 0/1/2/4/8/15/30/60/120 s 采 `/proc/pid/status`、`dumpsys meminfo` 与按映射名聚合的 `smaps` RSS：

| t | VmRSS | 主要映射 |
|---|---|---|
| 8 s | 374 MB | APK/ART/系统库 |
| 15 s | 1,805 MB | `/memfd:shadps4-guest-backing` 623 MB；`[anon:scudo:secondary]` 623 MB |
| 30 s | 2,457 MB | guest-backing 1,202 MB；scudo:secondary 640 MB；kgsl 55 MB；FEX JIT/Allocator/Lookup 合计 ≈110 MB |
| 60 s | 2,555 MB | guest-backing 1,238 MB；scudo:secondary 650 MB；kgsl 96 MB |

`dumpsys meminfo`（30 s）：Native Heap 685 MB、Other mmap 1,201 MB（即 guest 后备）、**GL mtrack 1,095 MB**（GPU 内存，不在 RSS 内）、TOTAL PSS 3.8 GB。GL mtrack 与启动即创建的固定池吻合：`bda_pagetable_buffer` 2^26 × 8 B = **512 MiB**、staging 256 MiB（Medium）、device 128、UBO stream 64、download 32（`buffer_cache.cpp:24–46`）。

结论：启机瞬间的 ≈2 GB = 游戏加载写入的 guest RAM（1.2 GB，8–30 s 内涨满，真实数据）+ Session 初始化时一次性的 ≈640 MB host 大块 `malloc`（scudo secondary，之后基本不涨）。后者具体调用点尚未定位（代码里没有按页建表的显式候选；可对 debuggable APK 用 `wrap.com.shadps4.android` 打开 `LIBC_DEBUG_MALLOC_OPTIONS=backtrace` 后 `am dumpheap -n` 符号化）。

## 复现

- 锁频：spatial-debug-tool `device_frequency_start(profile=swan-evt-legacy, 600s)`，结束后 `device_frequency_stop` 归档恢复记录。
- 构建：`cmd //c 'build\run-host-build.cmd'` → `android/shadps4-app && ./gradlew.bat assemblePlaystoreDebug` → `adb install -r -d`。
- 运行：`tools/ab_scale.sh <label> <knob> <outdir> 1`（重启 Session、屏幕 Circle 过片头、等诊所、3×10 s FPS、`gpu_memory request/status`、截图、提取本 Session 的 `Internal scale|Texture GC` 日志）；`tools/promo_analyze.py` 汇总提升/原生分配/native pass；`tools/startup_mem.sh` 启动内存采样。

## run G：同窗 PROF（修复后帧结构）

同一 APK 5bd0982e / host 38d51a5f，pid 24228 的 generation 2（`ab_scale.sh` 重启 Session，MemAvailable 1.0–1.4 GB），FPS 窗口 **9.65 / 9.49 / 9.24**；两份 20 s PROF（`bb-clinic-G-copyinherit-prof/`）：

| | PROF 1（gpu_timing off，SHA e68dbe23…） | PROF 2（gpu_timing detail，SHA 04cd1f1a…） |
|---|---|---|
| 完整帧 | 186，mean 106.9 / p50 106.9 / p90 112.5 ms（最慢 129.6 ms） | 182，最慢 129.4 ms |
| 同窗 GPU busy（kgsl，2 s 采样） | 97–98% @726 MHz，present 194/20 s | 97–98%，present 189/20 s |
| GPU zone | 0 | 12,059：`GPU.GuestCommands` 242 批、均值 **49.3 ms**（每帧 2 批 ≈ 98 ms）；`GPU.GuestRenderPass` **10,890 个共 909 ms**（≈90 个/帧、均值 83 µs、最大 15.1 ms）；HostPrepare/PostProcess ≈1.2 ms/帧；Present ≈2.5 ms |

分线程每帧预算（PROF 1，`analyze_frame_budget`）：

| 线程 | 每帧 | 构成 |
|---|---|---|
| Guest-19（帧 owner） | 106.9 ms | `scePthreadCondWait` 1 次 **86.7 ms**（等游戏渲染线程）+ 12.3 ms 自身工作 + ≈8 ms GNM HLE |
| Guest-1（游戏主线程） | 106.9 ms | **74.0 ms 未标注**（guest JIT + off-CPU）+ 32.8 ms HLE（6,161 次/帧：`pthread_mutex_lock/unlock` 2,630+2,668 次 ≈ 18 ms，`scePthreadCondWait` 4.7 ms，WaitSema 3.4 ms，SetPs/VsShader 2.6 ms） |
| GpuComm（PM4→Vulkan） | 106.9 ms | `PM4.Resume` 73 次 **69.9 ms** + 37 ms 未标注（等 PM4 输入） |
| 3× GpuDone | — | `Vulkan.CompletionWait` 每次 p50 92 / p90 105 / p99 109 ms，覆盖整帧 |
| Present | — | `Present.DriverCall` p90 93.7 ms、`RedrawFenceWait` p50 42.6 ms |

基线 A 里 Guest-1 每帧有 88 ms 在 `CondWait`（等 GPU 管线）；现在只剩 4.7 ms，Guest-1 自身的 JIT+HLE 就占满 107 ms，同时 GPU 每帧 ≈98 ms、97% busy——**CPU（Guest-1）和 GPU 同时顶到帧时长**，任何单侧的削减都不会再直接抬 FPS。

**PROF 2 的 render pass 只占 7.6% 是 zone 丢弃造成的假象**：旧 `ZonesPerBatch = 96` 只保留了每批最前面的 ~90 个 pass（帧首的小阴影 pass，均值 83 µs），后面的 G-buffer/lighting pass 全部被丢（当时未查 `dropped_zones`）。不能据此得出"GPU 时间在 render pass 之外"的结论。

### run H：分类 GPU zone（APK 50381eca / host 90a1c062，pid 13673 gen1）

detail 模式新增 `GPU.GuestDispatch`（compute dispatch）、`GPU.HostTransfer`（detile / image upload / copy / blit / 回读）、`GPU.BufferUpload`（per-draw buffer 上传）三类 zone，容量提到 384/批仍丢弃 53,792 个（约一半），所以下表是**有偏的下界**（丢弃偏向批尾）：

| zone（PROF `feca0691…`，252 批 ≈126 帧，`GuestCommands` 均值 49.3 ms/批 ≈ 98.7 ms/帧） | 记录数 | 合计 | 折算每帧 |
|---|---|---|---|
| `GPU.GuestRenderPass` | 29,848（≈237 个/帧，均值 230 µs，最大 15.9 ms） | 6.85 s | ≥ 54 ms |
| `GPU.HostTransfer` | 5,280（≈42/帧，均值 311 µs） | 1.64 s | ≥ 13 ms |
| `GPU.GuestDispatch` | 9,071（≈72/帧，均值 78 µs） | 0.71 s | ≈ 5.6 ms |
| `GPU.BufferUpload` | 3,429 | 0.07 s | ≈ 0.5 ms |
| 丢弃的 zone + barrier/等待残差 | — | 3.16 s | ≈ 25 ms |

结论：GPU 时间的大头仍在 render pass 内，但**不是像素量而是 pass 实例数**——每帧 ≈237 个（含丢弃后估计 ≥300 个）dynamic rendering 实例，而 `attachment_passes` 只统计带 draw 的 117 个，其余是 clear / 被 upload、dispatch、barrier、拷贝打断后重开的实例。tiler 上每次重开都要把全部附件 load/store 一遍，7 个 MRT 的 G-buffer 每次即十几 MiB GMEM 往返；这部分固定开销与分辨率无关，把 0.5 倍率省下的像素填充抵消掉了。HostTransfer ≥13 ms/帧（其中每帧 ≈2 次放大回读 blit+copy，以及 detile/upload）是第二项。同窗 GPU busy 98%，present 212/22 s（timing 查询本身约 +2 ms/帧 GPU 开销，本轮 FPS 不作 A/B）。

### run I：完整 GPU 分解与 render pass 打断原因（APK d20a4cad / host 359373e0，pid 30668 gen1）

`ZonesPerBatch = 1024`，`dropped_zones = 0`；FPS 窗口 9.28 / 9.37 / 9.33（run H 同法 9.36 / 9.44 / 9.58），GPU busy 99%。PROF `9a9157d6…`（20 s，174 帧，54,415 GPU zone；`GuestCommands` 226 批 / 113 帧，均值 50.2 ms/批 ≈ **100 ms GPU/帧**）：

| zone | 数量（每帧） | 合计 | 每帧 | 占比 |
|---|---|---|---|---|
| `GPU.GuestRenderPass` | 35,324（**313**，均值 223 µs，最大 16.3 ms） | 7.87 s | **69.7 ms** | 69% |
| `GPU.HostTransfer` | 5,436（48） | 1.59 s | 14.0 ms | 14% |
| `GPU.GuestDispatch` | 8,820（78） | 0.75 s | 6.6 ms | 6.6% |
| `GPU.BufferUpload` | 3,764（33） | 0.11 s | 1.0 ms | 1% |
| 残差（zone 之间的 barrier / cache flush / GPU 侧等待） | — | 1.03 s | 9.1 ms | 9% |

`Scheduler::EndRendering(cause)` 计数（`gpu_memory status` 两次快照差，23.3 s / 218 帧，`bb-clinic-I-breaks/breaks-delta.txt`）：每帧 **313 个 render pass 实例**（`begins` 与 GPU zone 数一致），1,491 个带附件 draw；打断原因：

| 原因 | 每帧 | 占比 | 发射点 |
|---|---|---|---|
| **barrier** | **198** | **63%** | `Pipeline::BindResources` 的 buffer barrier（`vk_pipeline_common.cpp:34`）与 `Image::Transit` 的 layout 转换（`image.cpp:673`）——本轮未再细分 |
| hle | 43 | 14% | `ExecuteShaderHLE` 的 copy shader 模拟（`vk_shader_hle.cpp:48`），走 `vkCmdCopyBuffer` |
| dispatch | 27 | 8.6% | guest compute dispatch |
| state_change | 26 | 8.3% | 附件/渲染区域切换（"天然"pass 数） |
| image_copy / image_upload / buffer_upload / download / present | 6 / 6 / 4.7 / 2 / 1 | 6% | 拷贝、上传、回读 |

结论：**GPU 时间 70% 在 render pass 里，但每个"天然"pass 平均被切成 12 个实例**（313 / 26）；tiler 上每个实例都要重新 bin + 把附件 load/store 一遍，这部分固定开销与分辩率无关，是 0.5 倍率省下像素后 GPU 仍 99% busy 的直接原因。三个最大的碎片化来源都在 host 侧：懒 barrier（读侧在 draw 前插 barrier，而非写侧在 dispatch/copy 之后、pass 之外插）、copy-shader HLE 走 transfer、compute 与 draw 交错。下一轮的候选：(1) 把 buffer/image barrier 拆开计数并按"写侧提前发射"改造 `Buffer::GetBarrier`/`Image::Transit` 的时机；(2) copy-shader HLE 在 pass 内改为延后到 pass 结束或用 compute 保持 pass；(3) `HostTransfer` 14 ms/帧中每帧 ≈2 次放大回读 blit+copy 可改为 `Download` 直接从 scaled 后备回读再由 CPU 侧放大（避免 GPU 放大）。这些都需要新的 A/B，本文不作提速声明。

GPU 时间戳的 CPU 对齐仍是估计（`gpu_cpu_alignment=unknown`，Turnip 无 calibrated timestamps），只用批内相对时长，不做跨设备帧归属。同窗 KGSL/sched 仍被 Litep 1.5 GiB 守卫挡住。
