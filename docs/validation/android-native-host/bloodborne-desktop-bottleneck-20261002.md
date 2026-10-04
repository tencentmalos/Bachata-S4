# 血源桌面版瓶颈：Windows / RX 7600M XT，2026-10-02

## 结论

1. **这台机器上当前限帧的是 GPU 队列，不是 CPU。** 基线 51.2–51.3 FPS 时，进程在独显 3D 引擎上的利用率为 97–98%（Windows PDH 计数器），每帧约 19 ms GPU 工作。GpuCommandProcessor（下称 GpuComm）每帧约 15 ms 在 CPU 上，另有约 4 ms 在 `Present.FrameFenceWait` 等前一帧的 GPU fence，也就是被 GPU 反压。
2. **Render scale 无效的原因：主深度缓冲无法缩放，连带整条 G-buffer 被提升为原生分辨率。** AMD Windows 驱动的组合深度模板格式（`D32_SFLOAT_S8_UINT`、`D16_UNORM_S8_UINT`）只有 `BLIT_SRC`，没有 `BLIT_DST`。缩放资格检查要求两者都有，失败时标记 `RequireNative("backing not scalable")`。主深度 `0x24f3f0000`（1920×1080 D32S8）因此是 `native-required`。凡是同时挂这张深度和缩放颜色附件的 pass，都因 `scale-mismatch` 把全部颜色附件提升为原生（`promote mixed attachment pass`）。结果是 84.5% 的 pass GPU 时间都在 1920×1080 原生 pass 里，G-buffer pass 一项就占 55%（约 9.7 ms/帧）。
3. **允许深度缩放的实验可以把帧率提到 55.3–55.5 FPS，但仍然受 GPU 限制。** 实验期间 mixed-pass 提升次数为 0，G-buffer 降到 960×540；但 G-buffer 仍约 7.1 ms/帧，平均每个 draw 约 8.8 µs GPU 时间，成本主要按 draw 计而不是按像素计。GPU 仍 95–98% 忙，GpuComm 升到 82–91%。
4. **还有约 3.3 ms/帧的 GPU 时间来自 GPU 跨 PCIe 读取主机内存里的 stream 数据。** 把 stream buffer 放到显存（`upload_diag stream_host off`）时，GPU 每帧工作从 18.0 ms 降到 14.7 ms，但 CPU 经 BAR 写显存很慢，帧率反而掉到 41–43 FPS。所以主机内存仍是这台机器的正确默认值。
5. **桌面版 `sceNetEpollWait` 有空转缺陷。** epoll 里没有 socket 时它立即返回、不等 timeout，游戏线程 `NexusRevolution Socket` 因此 100% 占满一个核，且每次调用都分配一个 vector。游戏实际传入的 timeout 是 33 ms。按 timeout 等待后进程总 CPU 从约 4.1 核降到约 2.8 核，这台机器上帧率不变。
6. **GPU timing detail 模式本身会让桌面帧率下降约 45%**（28 FPS 对 51 FPS，submits/flip 5.2 对 3.3）。detail 模式下测得的帧率不能当作游戏帧率；GPU 区间时长本身仍可参考。
7. **同日已实施第 8 节的 1、2、5 项和第 4 项的一部分**（第 9 节）：起点场景从 51–52 FPS 提到 59–60 FPS（游戏 60 帧上限），独显 3D 负载从 98% 降到 65–70%，GpuComm 从 13.5 ms/帧降到 11.8–12.7 ms/帧。第 3 项经测量是 Thunderbolt 带宽受限，没有改动。
8. **Android（AYANEO Pocket DS）上限帧的是游戏主线程 Guest-1，不是 GPU**（第 10 节）。同一起点 26.4–27.1 FPS（游戏 30 帧上限），GPU 81%（680 MHz 满频）。渲染提交线程每帧等 Guest-1 约 29 ms；Guest-1 每帧约 25 ms 在 CPU 上，其余等任务线程。所以 Android 上降 render scale 不会提帧。本轮修改里只有纹理绑定缓存在 Android 上生效，同一会话开/关对照没有可测差别。

## 现场与可复现信息

- 机器：Ryzen AI 9 HX 370（12 核 24 线程），接交流电，平衡电源计划。显示由 Radeon 890M 负责，游戏在独显 RX 7600M XT 上运行（vendor 0x1002 / device 0x7480，驱动 32.0.31041.1004，`gpu_id -1`）。copy 引擎约 10% 忙，用于跨适配器呈现。
  - 独显连接方式后来已核实：PCIe 路径为独显 → AMD 交换芯片 → Intel `8086:15EF`（Thunderbolt 3 Titan Ridge 桥）→ USB4 根端口，带宽约 2.75 GB/s（见 9.2）。
- 繁忙核心在 CPU 0–7，频率约为基频 2.0 GHz 的 115–130%（PDH `% Processor Performance`）。这是功耗受限的 APU，单线程吞吐有限。没有核心被 park。
  - 之后重新启动、刚进入同一场景时，`_Total` 的忙时间加权频率约为 170%（3.4 GHz），见 [桌面状态指标](desktop-status-metrics-20261002.md)。两次测量的口径（逐核 / `_Total`）和时间点都不同，差异原因未核实。
- 源码 `1c56a3f1f` 加本地未提交改动。基线 exe SHA 前缀 `8f1eb5a1872fe3bc`。实验 exe `ee594dd820c96d65b86a102cd8de43bfc68bfb986e5baa7f9faf68322c79af18`：两个实验开关都由环境变量控制，默认关闭，不设变量时行为与基线相同。原 exe（`dbd7d36f…`）备份在 `D:\workspace\shadps4-win-bb\backup-20261002-1524\`。之后部署的 exe 已换成 `6694cb60…`：同样带这两个开关，另加 Windows 状态指标。
  - 第 9 节修复完成后，`D:\workspace\shadps4-win-bb` 部署为最终 exe `3d1bad3aedc53550d5a09688e17cb6fa4b38217bd0353007d2d3e9de94474884`，两个实验开关已删除。`6694cb60` 备份在 `backup-20261002-metrics\`。修复期的测量都在测试目录 `D:\workspace\shadps4-win-test` 进行，使用存档副本。
- 设置：CUSA03023 每游戏配置 render scale 50%、FSR1+RCAS、guest patch `bloodborne_60fps_v1`。全局为 Mailbox、vblank 60、`async_graphics_skip`、录制线程开。
- 场景：读档后中央亚楠起点，角色静止。窗口起初在前台，之后被其他窗口遮挡。用 `SetProcessInformation` 关闭 EcoQoS 的对照帧率不变，说明遮挡不是变量。
- 帧率与 draw 数取自 DebugBus `debug_status` 的 `guest_flip`、`host_draw`、`queue_submit` 计数差值（`rate.py`）。overlay 显示的 51.1 FPS 与 `guest_flip` 速率一致。
- Litep `0.1.0-local.20261002.70d50ee`，桌面目标通过 DebugBus 控制器注册。
  - 采样 A：`bb-desktop-file-17909374338662309-1.prof`，SHA `8305d9b333818df4bf45928b530d1f3f1fccf89a0013ace2bfd621c42aad4c2d`，72.5 MB，38.8 s，911 万事件，2016 帧，0 个丢失 chunk。只有边界处有未闭合的 scope，标记为 truncated。
  - 采样 B（`497db97c…`，开 GPU detail）未分析，因为它处在 detail 模式的 28 FPS 状态。
  - 原始 PROF 和索引在 `C:\Users\fangfang\.litep\bb-desktop-20261002\`，不入库。
- 小体积证据在 [evidence/bloodborne-desktop-bottleneck-20261002](evidence/bloodborne-desktop-bottleneck-20261002/)：栈采样报告、两份 pass log、三份逐线程 CPU、vulkaninfo 片段、提升日志。

## 1. GPU 是限帧点

| 配置 | FPS | 3D 引擎忙 | 每帧 GPU（忙×周期） | GpuComm CPU |
|---|---:|---:|---:|---:|
| 基线 | 51.2–51.3 | 97.2–97.9% | ≈19.1 ms | 78.1% |
| 深度缩放实验 | 55.1–55.5 | 95.1–98.1% | ≈17.2–17.7 ms | 90.8% |
| 深度缩放 + epoll 修复 | 55.5 | 98.2% | ≈17.7 ms | 82.4% |
| 同上，stream 放显存 | 41.1–42.7 | 62.9% | ≈14.7 ms | CPU 受限 |

Litep 采样 A 中 GpuComm 的帧预算（帧 12300–12500，共 201 帧，GNM 提交周期，均值 19.17 ms、p50 16.80 ms、p90 25.15 ms）：

| scope | ms/帧 | 说明 |
|---|---:|---|
| PM4.Resume（inclusive） | 19.02 | 几乎覆盖整帧 |
| Rasterizer.Draw | 12.19 | 平均 1554 draw（p50 1411），约 7.8 µs/draw |
| Present.FrameFenceWait | 3.97 | 等前一帧的 GPU fence，即 GPU 反压 |
| Rasterizer.Dispatch | 1.72 | 121 次 |
| HLE.CopyShader | 1.48 | 58.6 次 |

线程等待关系与这一结论一致：
- GXRenderThread 89% 的阻塞样本停在 `sceGnmSubmitCommandBuffersForWorkload` → `WaitGpuIdle`（提交闸门），等 GpuComm 消化上一批命令。
- Game:Main 只有 40% 的样本在运行，其余 96% 的阻塞停在条件变量等待。运行部分里约 14% 是 60 FPS 补丁所在 pacer 区域（eboot+0x2035e58）调用 `gettimeofday` 的计时等待。Game:Main 有余量。

## 2. Render scale 为什么无效

- vulkaninfo `--show-formats`：RX 7600M XT 和 890M 上，`D16_UNORM_S8_UINT`、`D32_SFLOAT_S8_UINT` 的 optimal tiling 有 `BLIT_SRC`、没有 `BLIT_DST`；`D32_SFLOAT`、`D16_UNORM` 两者都有。
- 缩放资格检查 [image.cpp](../../../src/video_core/texture_cache/image.cpp)：缩放分支要求 `IsFormatSupported(fmt, BlitSrc|BlitDst)`。不满足时 `if (selected < 8 && !IsScaled()) RequireNative(SemanticNative, "backing not scalable")`。
- 日志（基线）依次出现：
  1. `native allocation 1920x1080 D32SfloatS8Uint use=4 domain=native-required reason=semantic-native mask=0x4 0x24f3f0000`
  2. `native pass … causes=native-attachment scale-mismatch native_attachment=semantic-native[depth] … 0x24f3f0000`
  3. `promote mixed attachment pass (mixed-pass) … R8G8B8A8Unorm …`，同样的提升还发生在 B10G11R11 和 RGBA16F 上。
- pass log（基线，600 个 pass，约 3 帧，GPU 合计 52.93 ms）：
  - 1920×1080 原生 pass 占 84.5%（257 个 pass、4721 draw）。
  - 6 色加深度的 G-buffer pass 占 55.2%：54 个实例（多数是被切开的 resumed 段）、3090 draw，约 9.7 ms/帧。
  - B10G11R11 光照 pass 占 11.2%。
  - 附件中 `native(mixed-pass)` 出现 601 次，`native(size-protect)` 出现 81 次。

### Turnip 对照（Android）

- **Turnip 不存在这个问题。** [`tu_formats.cc`](../../../references/mesa-turnip/src/freedreno/vulkan/tu_formats.cc) 对能当颜色目标的格式加 `BLIT_DST`。[`fd6_format_table.c`](../../../references/mesa-turnip/src/freedreno/fdl/fd6_format_table.c) 把 `Z32_FLOAT_S8X24_UINT`（即 `D32_SFLOAT_S8_UINT`）标为可作颜色目标（`_TC(Z32_FLOAT_S8X24_UINT, 32_FLOAT, …)`）。`references/mesa-turnip`（9b8a3567）和 `references/mesa-turnip-xr-fdm2`（d15b7c01）都是如此，所以 Turnip 上 D32S8 同时有 BLIT_SRC 和 BLIT_DST。
- **Swan 实测与之一致。** 2026-09-21 Swan 的日志（[evidence](evidence/swan-bloodborne-baseline-20260921/)）中同一张主深度为 `Internal scale allocation: 1920x1080 D32SfloatS8Uint -> 960x540 … mode resample`。mixed-pass 提升只出现在修复 copy-alias 之前的 run E，修复后为 0。
- 因此 Android 上 render scale 提帧不明显另有原因：2026-09-28 Thor 报告中 GPU 不是瓶颈（CPU 双线程受限），不是这条级联。同日在 Pocket DS 上的实测见第 10 节：GPU 81%，限帧的是 Guest-1。
- 两个实验开关只由环境变量启用；深度缩放实验只在格式缺 `BLIT_DST` 时改变行为，在 Turnip 上不改变任何路径。

## 3. 实验：允许深度缩放（`SHADPS4_EXPERIMENT_DEPTH_SCALE=1`）

- 改动：深度格式只要有 `BLIT_SRC` 就允许缩放；所有写入缺少 `BLIT_DST` 的深度 backing 的 blit 改为跳过并记日志（内容丢失）。只用于测量，不能作为修复。
- 结果：
  - 0 次 mixed-pass 提升。
  - 5 次跳过的 blit 都发生在图像创建时（upload/resample），涉及 960×540、480×270、240×135 的 D32S8。**本轮没有核对实验状态下的画面**。
  - GPU timing（detail）：GuestFrame 15.47 ms/帧（基线 18.1）；render pass 11.85 ms/帧（基线 14.3，每帧 192 个 pass）；BufferUpload 2.61 ms/帧（每帧 157 次）；HostReadback 0.39 ms/帧。
  - pass log：960×540 pass 占 83.3%；G-buffer 占 58.5%，约 7.1 ms/帧、8.8 µs/draw。像素降到四分之一，G-buffer 只省了约 27%。
- 结论：深度缩放是让 render scale 生效的前提，这台机器上实测 +4 FPS；剩余 GPU 成本以逐 draw 成本为主。
- 该实验开关已删除，由 9.1 的正式实现取代。

## 4. 剩余 GPU 成本

- **stream 数据跨 PCIe 读取：约 3.3 ms/帧**（见第 1 节表格最后两行）。显存方案受 CPU 写 BAR 的速度限制，帧率反而更低。
- **小 buffer 上传：约 2.6 ms/帧 GPU 时间**，每帧 150–157 次，每次约 17 µs。与分辨率无关。
- 其余逐 draw 成本（顶点处理、屏障、描述符）没有进一步拆分，需要 RGP 或 RenderDoc 计数器。

## 5. CPU 侧：GpuComm

GPU 降下来后，下一个限制点是 GpuComm：基线每帧约 15 ms CPU，实验中升到 82–91%；以 60 FPS 为目标，预算只有 16.7 ms。用户态栈采样共 652 个 tick，每次挂起平均 0.85 ms，有扰动。GpuComm 运行样本中的占比（inclusive，有重叠，不能相加）：

- Rasterizer::Draw 83%，其中：
  - BindResources 38%：BindTextures 20.9%，BindBuffers 15.5%
  - ObtainBuffer 23.8%：CopySparseMemory 9.1%（叶子落在 VCRUNTIME140 内存拷贝附近，无私有符号）、SynchronizeMemory 9.1%、IsRegionGpuModified 4.8%（含 FutexMutex）
  - GetGraphicsPipeline 14.2%：RefreshGraphicsKey 13.7%，其下 GetProgram 12.8%、StageSpecialization 6.2%
  - TextureCache::FindImage 9.9%（自身 6.1%）、ImageInfo 构造 5.6%、Image::FindView 4%
  - PrepareRenderState 5.9%
- DispatchDirect 8.3%，其中 copy-shader HLE 7.5%
- MemoryManager::ProtectGpu → VirtualProtect 2.7%

另外，GXWorker 的运行样本中约 12–16% 处理 guest 写 GPU 跟踪页触发的异常：`KiUserExceptionDispatcher` → `InvalidateMemoryFromWriteFault` → `VirtualProtect`，并伴有 BufferCache FutexMutex 争用。Windows 上这条异常路径开销较高。

## 6. `sceNetEpollWait` 空转

- [net.cpp](../../../src/core/libraries/network/net.cpp) 在 epoll 里没有 socket、也没有待处理解析时直接返回 0，不等待 timeout；每次调用还先分配 `std::vector<epoll_event>`。
- 采样：`NexusRevolution Socket` 100% 运行，热点依次是 `RtlGetSystemTimePrecise`（gettimeofday）、`sceNetEpollWait`、堆分配与释放、HandleTable 锁。
- 实验 `SHADPS4_EXPERIMENT_EPOLL_SLEEP=1`：日志显示空 epoll 6 的 timeout 为 33000 µs。该线程不再出现在 CPU 前列，进程总 CPU 从约 4.1 核降到约 2.8 核，帧率 55.5（不变）。
- Android 走另一套 socket 实现（`host_runtime/guest_sockets.cpp`，带 stop token），不受影响。
- 该实验开关已删除，由 9.5 的正式实现取代。

## 7. 测量注意事项

- 开 `gpu_timing detail`：28.2 / 27.9 / 28.7 FPS，submits/flip 5.2–5.3；关闭后 51.2 FPS、3.3。detail 状态下的帧率不能用，GPU 区间时长可参考。
- 关闭 EcoQoS（`SetProcessInformation` 关掉 ProcessPowerThrottling 的执行速度节流）后帧率不变，已恢复为系统默认。
- 栈采样器在 `scratchpad/sampler`（不入库）：挂起线程 → `GetThreadContext` → `StackWalk64` → DbgHelp/PDB 解析符号，guest 帧沿 RBP 链回溯。不需要管理员权限，但无法区分 runnable 和 running。本机非管理员，没有内核 ETW（上下文切换）数据。

## 8. 建议顺序（分析时提出，实施情况见第 9 节）

1. **深度模板缩放不依赖 `BLIT_DST`。** 在 BlitHelper 增加 shader 深度重采样：用 fullscreen pass 写 `gl_FragDepth`，模板在支持 `VK_EXT_shader_stencil_export` 时一并写入，覆盖 `BlitBacking`（上传、`ReallocateScale`、回读放大）。然后放开深度目标的缩放资格。实验上限 +4 FPS、GPU 每帧约少 2.5 ms。实验中跳过的 blit 必须由这一实现补上，才能保证正确。
2. **stream 数据先整块拷到显存再读。** CPU 仍写主机内存，每批提交前用一次 `vkCmdCopyBuffer` 拷到显存环。潜在收益最多约 3.3 ms/帧 GPU，需要扣掉拷贝本身的开销，做 A/B 验证。
3. **合并小 buffer 上传**（每帧约 150 次，2.6 ms GPU），减少每次上传前后的屏障。
4. **压低 GpuComm 的逐 draw 成本**：
   - 按 sharp 缓存纹理查找结果（FindImage / ImageInfo / FindView 合计约 26%）；
   - 跳过未变化的 stream 拷贝（memcpy 约 11%）；
   - 寄存器未变化时复用 RefreshGraphicsKey 的结果（约 14%）。
5. **修正 `sceNetEpollWait`**：无 socket 时按 timeout 等待（同时响应停止），去掉每次调用的 vector 分配。正式实现不能用实验里的固定上限。

## 9. 实施结果（同日，本地未提交）

按第 8 节顺序逐项处理。测量方法统一为：
- 每轮都从用户目录复制同一份存档，冷启动后用 DebugBus `desk_pad` 进入中央亚楠起点。只在菜单画面稳定时才按圆键，进入世界后不再发送任何输入。
- 画面稳定在每帧约 1572 个 draw 后，测 3 个 8 s 窗口。
- 测试目录为 `D:\workspace\shadps4-win-test`，存档是副本，用户的存档未被写入。

### 9.1 深度模板缩放不再依赖 BLIT_DST（已实现）

**实现：**
- **重采样 pass**：`BlitHelper::ResampleDepthStencil` 用一个 fullscreen pass 写入 `gl_FragDepth`。模板通过 `VK_EXT_shader_stencil_export` 写入；本机 890M 和 RX 7600M XT 都支持该扩展。
  - 取样规则与 nearest blit 相同：`floor((x+0.5)·src/dst)`。用整数运算实现，所以 2:1 这类比例的结果是确定的。
  - CPU 上传的深度不带模板平面，这种情况只写深度，保留原有模板。
- **触发条件**：
  - `Image::CanResample` 认为“能 blit，或者是可做深度附件、可采样的深度格式”就可以缩放；带模板的格式还要求支持 stencil export。
  - `BlitBacking`、`BlitCopy` 和 `ObserveUsage` 只在缺少 BLIT 能力时才走这个 pass。
  - Turnip 等支持 blit 的驱动行为不变。
- 实验开关 `SHADPS4_EXPERIMENT_DEPTH_SCALE` 已删除。

**结果（同一会话交替运行）：**

| | FPS | 3D 引擎 | mixed-pass 提升 |
|---|---|---|---|
| 原生深度（B1、B2） | 51.2–51.6 | 98% | 15 |
| 深度重采样（A3、A4） | 55.4–56.2 | 98% | 0 |

- 主深度 `1920x1080 D32SfloatS8Uint` 分配为 960×540。
- Khronos validation（SDK 1.3.296）运行中，新 pass 没有报错。
- 截图与原生深度时一致。

### 9.2 stream 数据经 DMA 队列拷进显存（已实现）

**新发现：**
- 本机独显是外置的。PCIe 路径为：独显 → AMD 交换芯片 → Intel `8086:15EF`（Thunderbolt 3 Titan Ridge 桥）→ USB4 根端口。隧道带宽约 2.75 GB/s。
- 每帧 stream 数据约 9,760 次拷贝、7.46 MB。
- 驱动提供一个只做拷贝的队列族（family 2，对应 SDMA），它与跨适配器呈现共用 `eng_1` 拷贝引擎。

**实现：**
- **传输队列**：独显上额外创建这个传输队列（`Instance::GetTransferQueue`）。
- **staged stream buffer**：CPU 写主机内存中的副本；绑定使用一份显存镜像，镜像在图形和传输两个队列族之间 `CONCURRENT` 共享，因此不需要所有权转移。
- **拷贝与等待**：每次图形提交前，把本批次提交过的区间（每圈最多 1 段，绕回时 2 段，包含对齐填充）在传输队列上拷进镜像；图形提交等待对应的 timeline 值。
- **写后读冲突**：ring 原有的 tick 等待保证，任一区间在两份副本中都不会在使用它的图形批次完成前被覆盖。
- **tile manager 例外**：tile manager 仍使用原来的主机 ring。否则几乎每个批次都要等一次拷贝，而这些拷贝排在呈现拷贝之后；实测此时开、关两种模式都只有 47–48 FPS，3D 引擎 58–69%。
- **队列锁**：`vkDeviceWaitIdle` 的调用者（如交换链重建）持有 `QueueMutex` 来同步所有队列，因此提交到传输队列时也持有这把锁。
- **开关**：`upload_diag stream_dma on|off`，默认开；`gpu_memory status` 中新增 `stream_dma` 计数。

**结果（同一冷启动流程、相同 CPU 频率，交替运行）：**

| | FPS | 3D 引擎 |
|---|---|---|
| DMA 开 | 59.8–60.1（到 60 帧上限） | 73% |
| DMA 关 | 55.4–55.7 | 98% |

GPU timing detail 下 guest 帧约 10 ms GPU，原来约 19 ms。

**顺带修复**：图像上传暂存区（`ObtainBufferForImage`）和 detile 输出的偏移原来只按 `minStorageBufferOffsetAlignment` 对齐，AMD 上这个值是 4；改为至少 16 字节。此前 validation 报 `VUID-vkCmdCopyBufferToImage-dstImage-07975`（RGBA32F 偏移 770840），修复后消失。

### 9.3 小 buffer 上传（已测量，未改动）

新增计数 `gpu_memory status` → `arena_uploads`。每帧数据如下：

| 项目 | 数值 |
|---|---|
| CPU 脏数据上传 | 29 次，5.5 MB，88 个区间，均为只读绑定 |
| 按绑定大小 | 超过 256 KiB 19 次，64–256 KiB 1.2 次，16–64 KiB 8.6 次 |
| GPU 上传时间（detail timing） | 2.07 ms |

5.5 MB 按 2.75 GB/s 计正好约 2.0 ms，所以这部分是 Thunderbolt 带宽受限，而不是屏障或命令数量的问题。合并上传和屏障减少不了要传的字节数。

把上传也挪到传输队列，需要按区间追踪批内先后访问，并做双向跨队列同步。在 GPU 已有余量（3D 引擎约 70%）的情况下，不值得冒这个风险。

### 9.4 GpuComm 逐 draw 成本（部分实现）

**已实现：**
- **纹理绑定缓存**：
  - 按取到的 T#、shader image resource 标志和数组元素作 key，缓存 `FindImage` 的结果（包括它对 view 的调整）。
  - texture cache 在注册或注销任何图像时递增 generation，缓存随之整体失效。
  - 命中时仍执行 `FindImage` 的逐次记账：`ObserveUsage`、访问 tick 和 LRU，每个调度 tick 只做一次。
  - storage 绑定不进缓存。开关为 `upload_diag texture_bind_cache on|off`。
  - 命中时在 texture cache 锁内确认图像仍处于已分配、已注册状态。若已被其他线程释放（guest 解除映射），本次 draw 绑定空图像，之后的 draw 重新查找并检查。
- **view 查找**：`Image::FindView` 先检查该 backing 上一次返回的 view。

**结果（同一会话交替 4 轮，GpuComm CPU 时间 / 帧）：**

| | GpuComm ms/帧 |
|---|---|
| 缓存开 | 11.60 |
| 缓存关 | 12.37 |

节省 0.77 ms（6.2%），命中率 99.9%（3,862 万次命中，3.9 万次未命中）。

**未实现：**
- **管线 key 的 `StageSpecialization`**：构造与比较约占 GpuComm 的 9%（约 1.1 ms/帧）。
  - 它读取 flattened user data、runtime info、绑定起点，以及 guest 内存里的 fetch shader 代码。
  - 要安全地按输入做记忆化，必须把这些输入全部纳入 key；本轮没有做。
- **stream 拷贝去重**：判断数据是否变化，本身要和拷贝一样读一遍数据，CPU 上省不下多少。

### 9.5 `sceNetEpollWait` 空转（已实现）

- **空 epoll 的等待**：既没有可报告的 socket，也没有待处理的解析时，按 timeout 等待（微秒；负数表示无限）。等待期间，`sceNetEpollControl` 成功修改或 epoll 被销毁都会唤醒它。
  - 唤醒后若有新注册，剩余的 timeout 用于原生 epoll。
  - epoll 被销毁则返回 EBADF。
  - 无限等待每 100 ms 复查一次，不会卡住退出。
- **事件数组**：改为线程局部的复用缓冲，不再每次调用都分配。
- 实验开关 `SHADPS4_EXPERIMENT_EPOLL_SLEEP` 已删除。
- **结果**：`NexusRevolution Socket` 线程每 10 s 的 CPU 时间从 10,000 ms（99.5%）降到 15.6 ms。

### 9.6 合计（修复前 exe 与修复后 exe 交替各 2 轮，同一流程与场景）

| | 修复前（B1、B2） | 修复后（F1、F2） |
|---|---|---|
| FPS | 51.1–52.5 | 59.1–60.0（游戏 60 帧上限） |
| 独显 3D 引擎 | 97–98% | 65–70% |
| GpuComm CPU / 帧 | 13.46 / 13.46 ms | 11.78 / 12.73 ms |
| 进程 CPU | 3.69–3.76 核 | 3.45–3.51 核（同时多出约 16% 的帧） |
| mixed-pass 提升 | 15 | 0 |

修复前的 exe 是 `6694cb60`（含 Windows 状态指标，两个实验开关默认关闭）；修复后的 exe 是 `02b9bd13`。随后加上 9.2 的队列锁和 9.4 的失效保护，得到最终 exe `3d1bad3a`，复测仍为 60 FPS、3D 74%、GpuComm 12.74 ms/帧、mixed-pass 提升 0。

### 9.7 边界

- 只测了中央亚楠起点这一个场景、这一台外置独显（Thunderbolt 3）机器。DMA 拷贝的收益取决于链路和拷贝引擎；显示接在独显上的台式机没有测过。
- 新代码在 Android（Turnip）上的情况：
  - 深度重采样 pass 不会触发，因为 Turnip 支持 blit。
  - 传输队列只在独显上创建。
  - 纹理绑定缓存在所有平台默认开启。Android 实测见第 10 节。
- 掌机 CPU 在长时间运行后会降频（本轮同一会话中，有效频率从 3.4 GHz 降到 2.6 GHz）。降频后各模式都变成 CPU 受限、帧率相同，因此 A/B 必须用冷启动交替的方式做。
- 代码已于同日提交，见 [主干归档记录](git-publish-20261002.md) 的“晚间归档”一节。
- 证据：`final-ab-before-after.txt`、`texture-bind-cache-ab.txt`、`final-before.png`、`final-after.png`、`stack-samples-gpucomm-after.txt`（纹理缓存首版时采样）、`validation-final-summary.txt`。

## 10. Android 实机（AYANEO Pocket DS，同日）

**现场：**
- 设备 AYANEO Pocket DS（`01108YHE01017563`，SG8275 / Adreno 740，Android 13）。
- 安装按本轮代码构建的 APK `3e8e160a682ac7fb5397262085a883dc65c411f4ffcbd7ff79293883e51b27fb`：
  - host `590594e5…`，源码 `8d3082e31` 加当时未提交的工作区（之后已按功能分组提交，内容不变）；
  - 驱动为默认的 Turnip `01a3548f`（351a4847 加 barycentric）；
  - 本机没有 `astcenc`，打包时跳过 `compressXrCinemaTextures`，XR 影院贴图未打入，只影响 XR 影院。
- 覆盖安装前的 APK 为 `e0f199c9…`（09-27）。安装前备份了血源存档与设置（本地 `build/validation/bb-pds-20261002/`，tar `65211889…`）。
- 设置为默认值（Internal Scale 0.5）。读档后在中央亚楠起点，角色静止，每帧约 930 draw。

**结果：**
- **帧率**：26.4–27.1 FPS（游戏 30 帧上限）。
- **GPU**：kgsl gpubusy 81–82%，频率 680 MHz（A740 最高频率）。
- **帧长**：LiteP PROF 10 s，270 个完整帧，均值 37.1 ms，p50 36.6 ms，p90 41.4 ms。帧长连续分布，没有集中在 33.3 ms，说明没达到 30 帧上限。
- **每帧开销**：
  - **Guest-20**（渲染提交线程，帧标记所在）：每帧一次 `scePthreadCondWait`，等 29.1 ms，等的是 Guest-1 交出这一帧；自身提交约 8 ms，其中 `sceGnmInsertPushMarker` 43 次共 0.78 ms，每次 18 µs。
  - **Guest-1**（游戏主线程）：
    - HLE 11.9 ms（449 次）。其中等其他线程约 7.8 ms：`CondWait` 4.1、`WaitSema` 2.9、`shadSyncWait` 0.8。
    - `SignalSema` 每次 35 µs，`cond_broadcast` 每次 24 µs。计时包含被唤醒线程抢占的时间。
    - 其余 25.2 ms 为 JIT 游戏代码和调度。线程 CPU 时间为 25.3 ms/帧。
  - **GpuComm**：
    - PM4 处理 22.8 ms：900 次 draw 共 16.7 ms（约 18.6 µs/draw），dispatch 3.7 ms，CopyShader HLE 2.9 ms。
    - 其余约 14 ms 空闲。线程 CPU 时间为 21.5 ms/帧。
- **结论**：Android 上帧率由 Guest-1 决定，GPU 未满载，所以降 render scale 不会提帧。Guest-1 加速后，下一个瓶颈是 GpuComm（约 23 ms/帧）。

**本轮修改在 Android 上的效果：**
- **纹理绑定缓存**：同一会话开/关各 2 轮，GpuComm 21.5–21.8 ms/帧，FPS 26.4–27.1，没有可测差别。命中率 99.9%，每帧约 4,200 次命中。桌面上省 0.77 ms/帧，Android 上看不出。
- **epoll**：Android 的 `sceNetEpollWait` 由 `guest_sockets.cpp` 实现，本来就按超时等待，没有桌面的空转。本轮修复只影响桌面。
- **深度重采样、传输队列**：Turnip 支持 blit，且是集成 GPU，这两项不会启用。

**下一步候选（未实施）：**
- Guest-1 的同步 HLE 单次代价（`SignalSema`、`cond_broadcast`），以及它与 GpuComm 在大核上的调度争用。需要 sched trace 才能区分。
- `sceGnmInsertPushMarker` 单次 18 µs，偏高。
- GpuComm 逐 draw 约 18.6 µs。

**边界：**
- 只测了一个场景、一次会话。render scale 没有做对照，“降 render scale 不提帧”是由 GPU 81% 推断的。
- 没有 sched/KGSL sidecar，未标注的时间没有拆分成 on-CPU、排队、睡眠。
- 没有与旧 APK 对比。
- 设备上的采集文件核对 SHA 后已删除。游戏留在运行状态。存档在读档（22:22）和自动保存（22:27）时被游戏改写，角色未移动。
- 证据：[android-pocketds-prof-budget.txt](evidence/bloodborne-desktop-bottleneck-20261002/android-pocketds-prof-budget.txt)、[android-pocketds-threads.txt](evidence/bloodborne-desktop-bottleneck-20261002/android-pocketds-threads.txt)、[截图](evidence/bloodborne-desktop-bottleneck-20261002/android-pocketds-central-yharnam.png)。PROF（`4d369dd8…`，17 MB）只保存在本地。
- 后续深入分析（调度、采样、写跟踪缺页、同步唤醒、温控）见 [bloodborne-android-bottleneck-20261002.md](bloodborne-android-bottleneck-20261002.md)。

## 边界（分析部分）

- 只测了一个场景（中央亚楠起点、静止）和一台机器。
- 深度缩放实验会跳过写入深度的 blit，画面正确性未核对，不能作为修复交付。
- 两个实验开关默认关闭，正式修复都未实现。
- 分析部分没有做 Android 对照，本结论不能直接套用到 Android 设备；Android 实测见第 10 节。
- 采样 B 和栈采样的 folded 文件未入库。
- 游戏存档没有修改（只读档，角色静止）。
