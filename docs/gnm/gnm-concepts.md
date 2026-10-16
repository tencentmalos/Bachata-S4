# GNM / GNMX 概念与公开资料摘要

来源编号见 [README](README.md)。
- 标 **[未核实]**：只出自一般 AMD/GNM 知识，没有在代码或幻灯片中读到。
- PM4 包定义以本仓 [`src/video_core/amdgpu/pm4_cmds.h`](../../src/video_core/amdgpu/pm4_cmds.h) 为准。

## 1. 分层

| 层 | 作用 | 本仓对应 |
|---|---|---|
| Gnm（libSceGnmDriver） | 低层接口：把 PM4 包写进命令缓冲（`sceGnmDraw*`、`sceGnmSet*Shader`），提交（`sceGnmSubmitCommandBuffers` / `SubmitAndFlip` / `SubmitDone`） | [`gnmdriver.cpp`](../../src/core/libraries/gnmdriver/gnmdriver.cpp)；Android 的提交入口在 `src/core/host_runtime/guest_graphics_hle.cpp`；guest 快路径编码器在 `guest/runtime/gnm/` |
| Gnmx（游戏静态链接） | `GfxContext`（DCB + CCB + 常量更新引擎 CUE）或 `LightweightGfxContext`（LCUE，无 CE） | 无，属于游戏代码 |
| 引擎 RHI（UE4 GnmRHI） | 在 Gnmx/Gnm 之上实现 RHI：多缓冲资源、uniform 分配、提交线程、查询、碎片整理 | 无，属于游戏代码，见 [ue4-gnmrhi.md](ue4-gnmrhi.md) |
| GPU 命令处理器（CP） | 执行 PM4：PFP/ME（draw engine，DCB）、CE（constant engine，CCB）、MEC（计算队列，ACB） | [`liverpool.cpp`](../../src/video_core/amdgpu/liverpool.cpp)：`ProcessGraphics`、`ProcessCeUpdate`、`ProcessCompute` |

## 2. 命令缓冲

- **DCB**（draw command buffer）：由 draw engine 执行，含寄存器设置、draw/dispatch、事件与等待。
- **CCB**（constant command buffer）：由 CE 与 DE 并行执行，操作片上 CE RAM（本仓按 48 KB 模拟，真机大小 **[未核实]**）。
  - `WRITE_CONST_RAM` 写 CE RAM，`LOAD_CONST_RAM` 从内存装入，`DUMP_CONST_RAM` 把 CE RAM 一段拷到内存环形缓冲中的下一项。
  - CE 与 DE 靠计数器同步：
    - CE 每准备好一次绘制所需的表就 `INCREMENT_CE_COUNTER`；
    - DE 在 draw 前 `WAIT_ON_CE_COUNTER`，draw 后 `INCREMENT_DE_COUNTER`；
    - CE 复用环形项前 `WAIT_ON_DE_COUNTER_DIFF(N)`：CE−DE ≥ N 时等待，防止覆盖 DE 还没取走的项。
  - `WAIT_ON_CE_COUNTER` 的 bit0 表示环形缓冲回绕时需要失效标量缓存（GPCS4 头文件 `si_ci_vi_merged_pm4defs.h:1738-1758`；radeonsi 用它做 sL1 flush）。
- **GfxContext 与 LightweightGfxContext**：
  - GfxContext：CPU 维护每阶段的 T#/S#/V#/常量缓冲影子表，经 CCB 写入 CE RAM，再 dump 到环形缓冲；DE 通过 `SET_SH_REG` user data 拿到环形项地址 **[未核实细节]**。
  - LightweightGfxContext：CPU 直接把表写进内存，只设 user data 指针，CCB 为空 **[未核实]**。
  - MKX 用 GfxContext（S1 第 30 页）；UE4 4.9 的 PS4 RHI 用 LCUE（4.9 发布说明“Disabled DispatchDraw support from LCUE”）。KH3 实测 CCB 全为空，见 [kh3-gnmrhi-emulation.md](kh3-gnmrhi-emulation.md)。
- **ACB**：计算队列命令缓冲，由 MEC 执行。异步计算用 `RELEASE_MEM` 写 label，与图形队列经 label + `WAIT_REG_MEM` 互等。

## 3. Label、fence 与等待

| 机制 | PM4 | 语义 | 本仓实现 |
|---|---|---|---|
| `writeAtEndOfPipe` / `writeImmediateAtEndOfPipe` | `EVENT_WRITE_EOP` | 之前的工作到达管线末端（可选缓存动作）后写 32/64 位数据、GPU 时钟或系统时钟，可选中断 | 默认（Android parse 模式）在 CP 解析到时立即写；completion 模式在宿主 GPU 完成后写（`FenceLabels`）。label 经 `TryWriteBacking` 直写物理后备，不触发写跟踪 |
| `writeAtEndOfShader` | `EVENT_WRITE_EOS` | 前面的 CS/PS 完成后写 | 同上 |
| 计算队列释放 | `RELEASE_MEM` | 计算队列上的 EOP 等价物 | memcpy 写入，会触发缺页 |
| 立即写 | `WRITE_DATA` | CP 解析到时写；`wr_confirm` 等写入确认 | memcpy 写入；写前先结清待定等待（`SettleWaits`） |
| 等待 | `WAIT_REG_MEM` | 轮询内存或寄存器直到比较成立（ME 或 PFP） | 协程 yield 轮询；VO label 例外，阻塞等 flip |
| CP DMA | `DMA_DATA` | 填充或拷贝，`CP_SYNC`；目标为寄存器 0x3022C 时是 L2 预取 | 预取跳过；fill/copy 交给 rasterizer 的 `FillBuffer`/`CopyBuffer`，按命令流顺序录制 |
| 缓存同步 | `ACQUIRE_MEM`（SURFACE_SYNC） | CB/DB flush、K$/I$/TC 失效与回写 | **空操作**；屏障由缓冲/纹理缓存自行跟踪 |
| 遮挡查询 | `EVENT_WRITE` PIXEL_PIPE_STAT_CONTROL/DUMP（0x38/0x39，index 1） | 把每个 DB 的 ZPASS 计数写到 begin/end 槽 | 写单调递增的假计数（`OcclusionCounterStep`），end−begin 恒正 |

**MKX 的用法**：
- S1 第 11 页：label 是“GPU 可写、CPU 之后会读的内存”，用 `writeImmediateAtEndOfPipe` 写，值是 marker 字符串指针，即一个 64 位值。
- 第 10 页：看门狗等 label 超过阈值就报 GPU hang。
- 第 15 页：GPU 计时器用 `writeAtEndOfPipe(kEopFlushCbDbCaches, kEventWriteDestMemory, <Shared Onion 内存>, kEventWriteSourceGpuCoreClockCounter, 0, kCacheActionNone, kCachePolicyLru)`。
  - 时钟源另有 `kEventWriteSourceGlobalClockCounter`，在本仓 pm4_cmds.h 中对应 DATA_SEL 3 与 4。
  - 本仓 `GetGpuClock64` 返回宿主纳秒；按 SDK 频率换算的游戏会得到错误的 GPU 时间 **[未核实对 KH3 的影响]**。

## 4. 内存类型

- **Onion**：CPU 可缓存（WB）、GPU 经 Onion 总线一致访问。用于命令缓冲、label、CPU 要回读的数据（MKX 时间戳在 “Shared Onion”）。
- **Garlic**：CPU 写合并（WC）、GPU 高带宽。用于 VB/IB、纹理、fetch shader。
  - UE 4.10 起 VB/IB 默认放 Garlic，除非需要 CPU 回读；4.11 起 fetch shader 也放 Garlic。
- **Flexible / 直接内存**：
  - UE4 的 OOM 日志区分 CPU / Garlic / Onion / FrameBuffer / Flexible；
  - 4.12 起在主区用尽时退回 flexible 内存。
- 本仓不区分 Onion 与 Garlic：guest 内存统一映射，GPU 数据经缓冲/纹理缓存复制到宿主资源。

## 5. 提交

- `sceGnmSubmitCommandBuffers(count, dcb[], dcbSize[], ccb[], ccbSize[])`：
  - OpenGNM 的 Orbis 包装要求 count ≤ 64、每个 DCB ≤ 0x3ffffc 字节（`references/opengnm/src/driver_orbis.c:616-637`）；
  - 本仓 Android 已去掉 64 的上限（KH3 #17，UE4 一次交 72 个）。
- `SubmitAndFlip`：最后一批与 flip 一起提交。
- `sceGnmSubmitDone`：每帧通知系统可调度。UE 4.12 起超过 2 秒未调会记日志。
- 本仓：
  - 每个 DCB/CCB 对变成一次 `ProcessGraphics`；
  - 提交时复制 DCB/CCB（own copy），遇 REWIND 才从原缓冲重读；
  - draw 的描述符与常量在解析时读取：SRT flatten、stream 快照、缓冲同步。

## 6. GDC 2016 MKX（S1）逐页要点

- p.9–11：GPU Snooper 起于早期 PS4 GPU hang 调试，用 label + marker 定位挂在哪段。
- p.15–18：EOP 时间戳计时，相邻时间戳之差即每段 markup 的时间。
- p.27：`pushMarker`/`popMarker`（Razor），带作用域的封装，零售版编译掉。
- p.30：PS4 平台上下文是 `sce::Gnmx::GfxContext`，由 DCB / CCB / Memory 组成。
- p.33–36、52：采集缓冲按字节打包，记录开始标签、计时器索引、描述字符串、结束标签。
- p.40：每线程一个 GFX 上下文；计时器池无锁，snooper 数据列表加锁。
- p.41–48：deferred 子上下文可以被任何上下文调用；调用时把数据切开（split）或转交（transfer）。
- p.49：提交时得到按执行顺序排列的 chunk 列表；分析方法与单线程相同，只是数据可能分散在多个 chunk 中。
- p.61–62：异步计算的记录列为后续工作。

## 7. PSSL 2013（S2）逐页要点

- 没有 GNM 接口、DCB/CCB、label、缓存动作或描述符格式的内容。
- p.4–5：DX11.2+/GL4.4 特性外加 SCE 扩展；异步计算；统一地址空间 8 GB GDDR5。
- p.11、p.25：所有阶段都可用 RW_Texture、RW_Buffer 和原子，包括 VS/PS。即顶点和像素着色器也可能写 storage。
- p.23：native ISA intrinsic `ballot`、`sad`。
- p.29–40：部分驻留纹理（64 KiB tile，`Sparse_Texture*`，`Sample()` 多一个状态输出）；同一物理页映射到多个虚拟地址（pool 纹理加 sparse 纹理）；Append/Consume 加 CPU 回读做 tile 重映射。
- p.42：compute + LDS 生成 mip。

## 8. OpenGNM 与 GPCS4 的边界

- **OpenGNM**（`references/opengnm` `4b295ca`）：
  - 没有 GfxContext/LCUE/CUE；CE 指令只有定义（`include/pm4/sid.h:258-263`），`WAIT_ON_DE_COUNTER_DIFF` 连定义都没有。
  - 可参考的命令构造：
    - EOP 构造（`src/drawcommandbuffer.c:1685-1761`，数据非丢弃时设 `SEND_DATA_AFTER_WR_CONFIRM`）；
    - Fill/Copy（DMA_DATA，`CP_SYNC`，按 0x1ffffc 字节分块）；
    - `WaitGraphicsWrite`（ACQUIRE_MEM：CB/DB + TC/TCL1 失效 + TC 回写）；
    - flip 完成等待（VO label 等于 0）。
  - Generic 后端的 submit/flip 什么都不做（`src/driver_generic.c:1457-1517`），不能当作 PC 版 GNM 渲染器。
- **GPCS4**（`88480de3`）：
  - 只提交 `dcbGpuAddrs[0]`，丢弃 CCB，CE 指令只打错误日志；每次提交后同步等空闲。
  - label 用 Vulkan timeline + 异步线程写，只支持 EQUAL 比较；`WRITE_DATA` 超过 2 个 dword 的部分被丢弃。
  - 只适合不用 CE 的游戏，不宜用来判断兼容性。
