# KH3 GnmRHI 静态分析交接（2026-10-16，进行中）

目的：
- 把 KH3（CUSA15072 01.10，UE "++UE4+Release-4.17"）里 UE4 PS4 RHI（GnmRHI）的提交与资源多缓冲实现读出来；
- 用它判断两件事：闪黑的可能原因（见 [KH3 问题汇总](../../bugs/kh3-cusa15072/README.md) 与 [10-14 闪黑二分](../validation/android-native-host/kh3-flicker-controls-20261014.md)），以及 GnmRHI 在 Android 版上的性能坑。

当前进展为 **r6**：第6–11节依次记录 lifetime、binding、C++、UE接线、graphics和GS/tessellation；最新结果见[阶段分析](../gnm/kh3-gnmrhi-stages.md)，导航见[资料入口](../gnm/README.md)。前面保留资料整理与原始问题背景；涉及实际实现时，以后续各节的 KH3 证据和明确边界为准。

## 1. 本地文件

| 内容 | 路径 |
|---|---|
| 用户提供的资料 | `C:\Users\Admin\Downloads\GNM_GNMX_技术资料与源码阅读指南.md` |
| KH3 解密 eboot（`ps4_guest_code.py` 用） | `build/validation/odin3-kh3-preempt-20261012/kh3-exe-parent/export/app0/elf/eboot.bin.elf`（SHA256 前缀 `29524c8302995aac`） |
| 分析 ELF（文件偏移 = 模块偏移 + 0x4000，数据段 vaddr 0xa6fc000 ↔ 文件 0xa700000） | `build/validation/odin3-kh3-preempt-20261012/kh3-exe-parent/eboot-analysis.elf` |
| 给 IDA 的 ET_DYN 版 | `D:\workspace\kh3-export\eboot.analysis.dyn.elf` |
| 已有 IDA 数据库（之前会话建的，1.9 GB，是否分析完整未确认） | `%TEMP%\SpatialDebugTool\IdaMcp\native\98f5eb4fb6084c918c80b4c65a7f3d3a\eboot.analysis.dyn.elf.*` |
| KH3 符号（v2）与 guest patch | `guest/games/CUSA15072/01.10/symbols`、`guest/games/CUSA15072/01.10/{query_wait.cpp,tick_wait.cpp,cpu_waits.recipe.json}` |
| KH3 PM4 trace（16 帧，奥林匹斯） | `build/validation/kh3-flicker-1014/buf1/traces/41519d7fe57468e2fe575acdc0e1da99.gpu.pm4.trace`（同目录 `.gcmdtrace.ps4`）；另有 `kh3-flicker-1014/ab31/traces/`、`trace1/` |
| 本轮整理的中间产物 | `build/validation/kh3-gnmrhi-20261016/`（见下表） |
| 本地 UE 源码（5.8.3 release ZIP，commit `396c9f05`，无 git 历史，无主机平台代码） | `C:\workspace\UnrealEngine` |
| OpenGNM（GNM 结构/命令构造参考，`4b295ca`） | `references/opengnm` |
| 工具 | `tools/ps4-guest-code/ps4_guest_code.py`（xrefs/disasm/imports/func/str）、`tools/ps4-gpu-trace/ps4_gpu_trace.py`（summary/packets/passes/who） |

`build/validation/kh3-gnmrhi-20261016/`：

| 文件 | 内容 |
|---|---|
| `kh3_ue_strings.txt` | eboot 中与 Gnm/PS4/r.* 相关的 UTF-16 字符串（偏移是分析 ELF 的文件偏移） |
| `kh3_cvars_full.txt` | 同上，带每个 cvar 的完整帮助文字 |
| `kh3_fn_3a9470.txt` | GnmRHI cvar 注册函数完整反汇编 |
| `kh3_packets_buf1.txt` | 上述 trace 全部 PM4 包的文本解码（约 29 万行） |
| `gnm-research/` | 已下载的 GDC 2016 MKX、PSSL 2013 PDF，UE 论坛发布说明原文，以及文本抽取（`out/`） |

地址约定：
- 下文都是**模块偏移**；运行时地址 = 0x400000 + 偏移（`Loading module eboot.bin to 0x400000`）。
- `fn_<偏移>` 是 `ps4_guest_code.py` 按 `.eh_frame_hdr` 起的名字。

## 2. 已确认的 GnmRHI 事实

**引擎版本**：
- eboot 中有 `++UE4+Release-4.17`，源码路径 `D:\Tres\UE4\JK\Engine\Source\Runtime\PS4\...`。
- 源文件：GnmCommands.cpp、GnmManager.cpp、GnmMultiBuffer.cpp、GnmShaders.cpp、GnmTexture.cpp、GnmUAV.cpp、GnmVertexBuffer.cpp。
- Epic Japan 2019 年 Square Enix 讲座写的是 UE4.17.2 基线；它对应 KH3 是推断。

**cvar 默认值**：
- 来源是 `fn_3a9470` 中 `RegisterConsoleVariableRef` 的引用变量初值。
- 运行时可能被 ini 覆盖，未核实。

| cvar | 变量 | 初值 | 帮助文字要点 |
|---|---|---|---|
| r.PS4CommandBufferSplitSize | 0xa6fc50c | 32768 | 达到该大小时自动拆成单独一次 submit |
| r.PS4ContinuousSubmits | 0xa6fc514 | 1 | GPU 何时从 CPU 拿到命令（边录边交） |
| r.PS4EventBasedFrameEnd | 0xa6fc510 | 1 | 提交线程基于事件，0 为轮询 |
| r.PS4TimeSubmitWaits | 0xa6fc518 | 1 | 统计“提交来得不够快”造成的 GPU 饥饿 |
| r.PS4DoComputeAutoFlush | 0xa7266c0 | 0 | 开启时 compute 前后做 GPU wait + cache flush（模拟 DX11） |
| r.PS4.AsyncComputeBudgetMode | 0xa6fc508 | 1 | 0 = CU masking |
| r.PS4StallsOnMarkers | 0xa726e20 | 0 | |
| r.PS4WindowedVSync / Top/BottomVsyncWindowMargin | 0xa726a8c… | 0 | SCE_VIDEO_OUT_FLIP_MODE_WINDOW |
| r.PS4DefragPoolSize | 0xa6fc51c | 2300（MB） | GPU 碎片整理池 |
| r.PS4.AllowDefrag / r.PS4.GPUDefragPerfTest / r.PS4FlushForAsyncReallocateTexture2D | 在 `fn_3b2c60` 注册 | 未读 | GPU memmove 搬移资源 |

**关键函数**（xrefs 得到，均未反编译）：

| 偏移 | 依据 | 推测作用 |
|---|---|---|
| `fn_3a9470` | 注册上表 cvar | GnmRHI 初始化 |
| `fn_3a5640`（+0x15c0/+0x15e1） | 引用 `SubmitDoneAsyncTaskThreadPS4`、`SubmitGPUCommandsAsyncTaskThreadPS4` | 创建两个提交线程 |
| `fn_39c920`（+0x1f4 引用 "Error: Neo does not support WindowedVSync"、"Final submission failed"） | 0x39ca47 调 `sceGnmSubmitAndFlipCommandBuffers`，0x39ca90 调 `sceGnmSubmitCommandBuffers` | 帧末最终提交 + flip |
| `fn_39cbb0`（+0x502/+0x533 引用 "async submitCommandBuffers failed"、"async compute submit failed"） | 0x39d152 调 `sceGnmSubmitCommandBuffers`，0x39d26a 调 `sceGnmSubmitDone` | 异步（continuous）提交线程主体 |
| 0x395e74、0x3a8d79 | 调 `sceGnmSubmitDone` | 其他 SubmitDone 路径 |
| `fn_3a3010`（+0x355 引用 "FGnmMultiBufferResource created without Static, Volatile, or Dynamic specified"） | | FGnmMultiBufferResource 构造：多缓冲份数与轮换规则在这里 |
| `fn_3cbec0` | "RHICopyVertexBuffer is not supported yet in Gnm" | |
| 0x3a0d10（运行时 0x7a0d10） | 已有 guest patch `query_wait.cpp` | `RHIGetRenderQueryResult` 等待循环；全局 `GnmRHI_QueryWaitSuccessCount` 运行时 0xc20e20c |

**PM4 trace 中每帧的形态**（16 帧平均，奥林匹斯，约 750 draw/帧）：
- **提交**：每帧约 25 次 `sceGnmSubmitCommandBuffers`，来自两个线程，**CCB 全部为 0**。即不用 CE/CUE，`IncrementCeCounter` 等 CE 包不会出现；UE 4.9 发布说明也写着 PS4 RHI 用 LCUE。单个 DCB 253–11955 dword。
- **GPU 自排空**：每帧约 143 次。形式是 `EVENT_WRITE_EOP`（0x38528 = BOTTOM_OF_PIPE_TS + TC wb/TCL1/TC 动作，或 0x528）把 1 写进 DCB 自身前面的 dword，紧跟 `WAIT_REG_MEM`（func=equal，ref=1）等它；前面常有 `ACQUIRE_MEM`。
  - 多数围绕 compute dispatch，与 4.11 起 `RHITransitionResource` 显式屏障一致（DoComputeAutoFlush 默认 0）。
  - 模拟器 parse 模式下 EOP label 在 CP 解析时就写，这些等待立即通过。completion 模式下每帧会有约 143 次真实往返。
- **跨缓冲等待**：每帧约 14 次 `WAIT_REG_MEM` 等其它 DCB 段里先前 EOP 写的 label。gfx↔ACB 互等经 `0x20_087xxxxx` 段：ACB 用 `RELEASE_MEM` 写，gfx 用 `DMA_DATA` 填 0 复位。另有 VO/flip label 等待（0xfe0000000）。
- **DMA_DATA**：每帧约 495 次，几乎全是写 0x3022C 的 L2 预取（liverpool.cpp 跳过）；只有 2 次是填 0 的 fill（`0x20_08702000/400`，cp_sync）。没有 memory→memory 拷贝，即这 16 帧里没有 GPU 碎片整理搬移。
- **遮挡查询**：每帧 43 对 `EVENT_WRITE` 0x138/0x139（PIXEL_PIPE_STAT_CONTROL/DUMP），结果在 `0x30_0007xxxx`。模拟器写单调递增的假计数，begin/end 之差恒正。
- **其他**：时间戳 EOP（data_sel=4，GPU clock）每帧若干；`WRITE_DATA` 只有驱动 flip label。

## 3. 闪黑相关的待验证假设（未证实）

完整版见 [docs/gnm/kh3-gnmrhi-emulation.md 第 3 节](../gnm/kh3-gnmrhi-emulation.md)。

**更正**：本文初稿把 `pool_revive/tiny_writeback/expand_twin/adopt_mips` 列为候选，这些在 10-14 已排除。

**现状**：
- 10-14 在 buf1 trace 的一次全黑帧里已确认，guest 命令、寄存器、guest 缓冲内容与正常帧一致，问题在宿主执行侧。
- 当前首选：
  1. 渲染倍率（75%）的缩放计划在个别帧不一致，含 10-13 的 scaled storage；
  2. 宿主每 draw 数据环（stream / staging / 描述符）复用；
  3. GpuByteKeeper 等值跳过。
- 下一轮设备实验：全开下分别设 `scaled_storage_off=1`、Render Scale 1.0、`upload_diag keep_gpu off`；再加一个每帧宿主 `waitIdle` 的诊断开关。

**对 IDA 结论的期待**：
- GnmRHI 多缓冲与 uniform 回收规则用来确定 guest 侧数据的复用节奏。坏帧 guest 数据正常，所以这条优先级低于宿主侧。
- 提交与查询部分仍有用：用于性能问题 P1/P3（查询等待、边录边交）。

## 4. 请 IDA 侧帮忙做的事

设备与工具：
- Odin3 `6edb0593` 由这边用来做闪黑 A/B。10:09 有一轮被 `user_stop` 中断，如果是你停的，请说一声。IDA 侧请不要操作设备。
- 需要运行时数据时，写下要读的地址，由这边用 `memory_describe` 或 guest debugger 读。
- IDA：这边在 10:2x 调过一次 `ida_workspace_open`，调用被中断，状态显示未建工作区；之后这边不再开 IDA。
  - 每个 MCP 服务进程只能有一个原生数据库。80 MB 的 eboot 首次分析要几十分钟，超时后重复同一个 `workspace_open` 即可（幂等）。
  - 也可以先试用第 1 节的现有数据库。

1. **`fn_39cbb0` / `fn_39c920` / `fn_3a5640`**：
   - continuous submit 的实现：何时切一次 submit；DCB 段如何链接。
   - CPU 提交后是否还会改已提交的 DCB（打补丁、REWIND）；命令缓冲内存复用前等哪个 label。
   - 帧末 SubmitDone 与 flip 的顺序。
2. **`fn_3a3010`（FGnmMultiBufferResource）**：
   - Static/Volatile/Dynamic 三类各自的份数与轮换规则；复用前等待的 fence/label 地址或类型；
   - Lock/Unlock（RLM_WriteOnly）在 CPU 侧写到哪一份。
3. **uniform buffer**：
   - `RHICreateUniformBuffer`（SingleFrame/MultiFrame）在 GnmRHI 中的分配器：环形缓冲大小、按帧 fence 回收的条件；
   - `RHIUpdateUniformBuffer` 是否原地改写。
4. **`RHIGetRenderQueryResult`（0x3a0d10 附近）**：
   - 结果是否就绪靠什么判断：结果字 bit63、单独 label，还是先清零；
   - bWait=false 时未就绪返回什么。
5. **`fn_3b2c60` 与碎片整理**：GPU memmove 用 DMA 还是 compute；搬完后 CPU 何时、凭什么把资源指针改到新地址。

建议：
- 函数名、结构体字段尽量写进 `guest/games/CUSA15072/01.10/symbols`（v2 格式，参见该目录现有条目），这边的 guest debugger 可以直接用。
- 结论连同依据的偏移写回本文第 5 节，或另起一节。

## 5. 资料整理进度

- 已完成：GDC 2016 MKX（DCB/CCB、label、EOP 时间戳、多线程 chunk 拼接）与 PSSL 2013 的逐页摘要；UE 4.9–4.20、4.26 发布说明中的 PS4 条目；KH3/Square Enix 2019 讲座要点。
  - 要点：4.15 起 Epic 不再公开 PS4 发布说明，4.15–4.18 没有公开的 PS4 渲染条目。
  - 原文在 `build/validation/kh3-gnmrhi-20261016/gnm-research/`，整理稿将放 `docs/gnm/`。
- 已完成：OpenGNM（`4b295ca`）与 GPCS4（`88480de3`）对照。
  - 两者都没有实现 Gnmx CUE：OpenGNM 只到 PM4 构造层，GPCS4 直接丢弃 CCB。
  - 发现的 shadPS4 问题（GpuByteKeeper 等值跳过、EOP label 绕过写跟踪、`WAIT_ON_DE_COUNTER_DIFF` 判断反了等）见 [kh3-gnmrhi-emulation.md 第 3 节](../gnm/kh3-gnmrhi-emulation.md)。
- 已完成：UE 4.18 公开镜像 `windystrife/UnrealEngine_NVIDIAGameWorks` @ `b50e6338` 的通用 RHI 帧模型、资源生命周期约定和 PLATFORM_PS4 钩子。读过的源码副本在 `build/validation/kh3-gnmrhi-20261016/ue418/files/`，`ue418/fetch.sh` 可续取。
- 整理稿已写入 [docs/gnm/](../gnm/README.md)：
  - [GNM 概念](../gnm/gnm-concepts.md)
  - [UE4 GnmRHI](../gnm/ue4-gnmrhi.md)
  - [KH3 命令流对照、闪黑假设与 Android 性能问题](../gnm/kh3-gnmrhi-emulation.md)
- 对 IDA 侧有用的 UE4 事实：
  - **uniform buffer 不可变**，view 常量每帧在新内存里新建（每个 view 至少 2 次）。
  - **Volatile**：每次 Lock 换新内存，PS4 上约 8 MB 池。
  - **Dynamic**：公开代码中的使用约束不能直接当 KH3 实现；本轮确认 KH3 普通 Dynamic 是三份、每次写 Lock 轮换，Dynamic|UAV 为单份，见第 6 节。
  - **compute 写的 morph target 缓冲**须为 Dynamic + UAV。
  - **遮挡查询**：`r.NumBufferedOcclusionQueries=1`，以 bWait=true 读上一帧结果。
  - **GPU 可落后多少帧**由平台的 `RHIEndDrawingViewport` 决定，在 GnmRHI 内，需要 IDA 确认。
- GitHub：没有使用用户提供的账号密码。公开的 EpicGames/UnrealEngine 本来也不含 GnmRHI。

## 6. KH3 01.10 二进制还原结果（2026-10-16）

完整记录：[KH3 01.10 的 GnmRHI：提交、缓冲与回收实现还原](../gnm/kh3-gnmrhi-reconstruction.md)。以解密 eboot SHA `29524c83…` 为准，UE 分支为 **4.17**。本轮未操作 Odin3；没有新运行时验证，也没有把这些发现直接归因为闪黑。

| 原问题 | 本轮结果 |
|---|---|
| continuous submit / DCB 拼接 | `0x372930` 封装地址与长度，`0x372b20` 合并 pending batch；32 KiB 为检查阈值，物理命令块 64 KiB，`0x3870f0` 写满后换块。已读路径没有 REWIND / 回改已提交载荷；不是全二进制的排除结论 |
| 提交与帧结束 | `0x39cbb0` 是 drain helper，线程 Run 为 `0x3a8370`。drain 等 `G.frameDoneLabel[1-slot] == frameSerial-1`；`0x39c920` 先 drain 后最终 Submit/Flip。SubmitDone 由下一轮 drain 或 `0x3a8b50` 超时 worker 补做，并非每次 flip 后立即同步调用 |
| Static / Dynamic / Volatile | `0x3a3010`：Volatile 一槽；否则 Static 或 UAV 一份；否则 Dynamic 三份。`0x39a100` 每次非 Volatile 写 Lock 都轮换；Volatile 每次分配；Lock 内没有 fence。`0x3cbeb0` Unlock 直接返回 |
| Uniform / 临时池 | `0x391d20/0x3a3ce0` 创建时复制常量并持有资源引用；usage≤1 和 Volatile 共用 `0x36b620`，1 KiB 对齐、8 MiB 块。两个前端 bank 经 RHI 命令交接；底层四代块池、闲置≥6 帧送延迟 free |
| GPU 回收条件 | `0x37d6210` 按 `0xa7065f0` 的 ticket 排队；`0x37d6b20` 只释放 ticket≤GPU 完成值的项并推进 ticket。`G+0x258` 指向 EOP 写的完成值，独立于两槽帧标签。不能把“四代池”称为“四帧 fence” |
| Query 就绪与清零 | Begin `0x37dc50` CPU 清 256 字节；End `0x37de30` 对时间查询 CPU 清 8 字节再发 EOP。SDK `0x68bbed0` 发 0x138/139。occlusion 要所有 8/16 对 begin/end 的 bit63；时间查询仅需非零；普通 nonblocking 未就绪写0，但提前拒绝 / 超时保留 OutResult |
| 碎片整理 | 默认值补齐：AllowDefrag=1、PerfTest=0、FlushForAsyncReallocateTexture2D=0。`0x3b2c60` 只是注册，延迟 free 也不是 memmove。DMA/compute 搬移到 CPU 更新资源指针的闭环仍未恢复 |

符号：[index.json](../../guest/games/CUSA15072/01.10/symbols/index.json) revision 2；新增 38 个符号、9 个部分结构，合计 45 个符号。新条目标为 candidate，已有条目的历史 confidence 保留；新原型单独标 candidate。原始 ELF SHA、运行时兼容身份、分片哈希由 `guest_debug_symbols_prepare` 校验。

IDA 已可用：工作区 `kh3-gnmrhi`，imagebase 0；已保存独立 `review.i64`，位置与 SHA 见完整分析第 1 节。后续可直接打开此快照，避免重新分析全 eboot。注意数据库输入是 ET_DYN 分析 ELF，其 SHA 与符号包锁定的原始解密 ELF 不同，不能放宽 SHA 门禁强行导入。

下一步优先：按完整分析第 8 节只读核对帧标签 / free ticket 与 host fence 的时序，再追 uniform descriptor 的绑定；碎片整理需要补真正搬移入口。闪黑继续服从第 3 节已有坏帧证据，优先排查宿主侧。

## 7. 进一步语义还原与可运行参考（2026-10-16）

详见[参数、资源绑定、LCUE 与 dispatch](../gnm/kh3-gnmrhi-binding-semantics.md)和[模型入口](../../tools/gnmrhi-semantics/README.md)。沿用 KH3 01.10 / UE4.17 身份，没有新设备采样。

- `0x368f70 → 0x36d740`：CPU loose-constant shadow 的写入 / high-water / dirty、16 字节 extent、64 KiB 上传块及大分配分支。上传 helper 不清 dirty，由绑定调用者确认；保留旧 extent 与丢弃旧 extent 两种行为已恢复。
- `0x36db40 / 0x384e20 / 0x3854b0 / 0x386240`：UB dirty mask 与 shader ResourceTableBits 相交；packed entry 的 8/16/8 位分别为 UB slot、resource index、bind index，texture/SRV/sampler 三类展开；末尾清整个 stage mask。
- reflection uint16：`ffff` 未使用、bit15 选直接 user SGPR 或 CPU table、低14位为索引；表按 GNM stage 间隔 `0x1800` 字节。`0x36b0f0` 在绑定 buffer SRV 时从 multibuffer 当前槽刷新 descriptor 地址。
- SDK `0x4c39b50`：CS shader dirty 绑定 shader；resources dirty 复制整个 staging 前缀到新上传区，再写表 / 子表指针 SGPR。已提交表与可修改 CPU staging 分离；CCB=null 不等于没有 LCUE 常量管理。
- `0x36cfb0 / 0x3772d0`：direct / indirect compute 的绑定与发包闭环；`0x36d440` 的 EOP → WAIT → ranged acquire，以及可选 post-compute EOS → WAIT → acquire。`0x388b30` 也无条件调用前者，故 AutoFlush=0 不能推成全局无同步。
- `0x380650 → 0x386a40 → 0x386680 → 0x4c3a220 → DrawIndexAuto` 的图形调用边界已记录；SDK graphics commit 的函数体未完整恢复，不套用 CS 实现。

新增 `gnmrhi-binding.symbols.json`：16 个符号、6 个部分结构。索引 **revision 3**，累计 **61 个符号、15 个类型、41 个带原型符号**。新条目仍为 candidate，两个图形函数名称仅有调用边界证据，已在各自 evidence 中说明。

验证：原始 ELF SHA、兼容运行时身份、三个分片哈希经 `guest_debug_symbols_prepare` dry-run / 实生成通过；新产物在 `build/validation/kh3-gnmrhi-20261016/symbols-generated-r3/`。clang `x86_64-none-elf` 检查通过：149 个 size/offset 断言及生成 C 的语法。模型 10 个场景测试通过，但没有做二进制差分、真实 shader 执行或设备画面验证，也没有绕过 IDA 的输入 SHA 门禁。

后续优先补 shader 创建 / reflection 生成 / shader 切换 dirty 的闭环，再补 graphics LCUE、UAV、texture clear/resolve；defrag 的搬移入口仍未找到。新文档 §8 给出跨 UE PS4 游戏的结构指纹及逐层确认条件，偏移和符号包不可直接跨游戏复用。

## 8. C++ 语义实现（2026-10-16）

用户进一步要求基于伪码提供 C/C++ 版本，已新增 [tools/gnmrhi-semantics/cpp](../../tools/gnmrhi-semantics/cpp/README.md)，独立 C++20 静态库，无第三方依赖。

- `gnm_rhi_semantic.h/.cpp`：loose constants、UB packed resource map、SGPR / LCUE 绑定、表快照、SRV 地址刷新、direct / indirect compute、EOP/EOS / WAIT / ACQUIRE、Dynamic/Volatile Lock、ticket 延迟释放、query 就绪与聚合核心。
- 通过 `Backend` 接分配 / 写入 / PM4 / SDK 调用，通过 `ResourceResolver` 接资源描述符。已识别包字直接实现，未知 SDK shader 和 dispatch emitter 保留显式接口。indirect 的 PFP_SYNC_ME / SET_BASE 保持在 LCUE commit 前。
- `recording_backend.h`、`demo.cpp`：CPU 合成地址、字节存储和命令事件，展示两个 dispatch 的不同 backing 与保留的旧表。无 GPU 执行，示例的 label 分配不冒充真实 DCB NOP 布局。
- C++ emitter 遇到不完整 staging、未恢复 reflection extras、非 CS commit 会拒绝；不以零补未知状态。shader/UB dirty 的生产、完整 texture 虚调用、graphics LCUE、UAV、defrag、真实提交与并发未实现。

验证构建在 `build/validation/kh3-gnmrhi-20261016/cpp-build`：VS 18 2026 / MSVC 19.51、Windows x64 Release 编译成功；CTest 1/1，内部 **13/13** 场景通过，demo 正常生成命令流。检查了固定 PM4 字面量、部分常量更新、两次 dispatch 旧快照、三类资源映射、动态槽轮换、indirect 顺序、free ticket 与 query ready 位。不是 shadPS4 整仓构建、设备或二进制差分验证。

## 9. 关联 UE 接口的参考实现（2026-10-16）

用户要求进一步接回 UE 的实际类和调用链。新增[关联分析](../gnm/kh3-gnmrhi-ue-reference.md)与 [ue4-reference](../../tools/gnmrhi-semantics/ue4-reference/README.md)：真实 UE 头文件、`FGnmCommandContext : IRHICommandContext`、`FGnmDynamicRHI : FDynamicRHI`、shader / UB / VB 的 `FRHI*` 派生类及 `FRHICommandList` 调用示例。KH3 4.17 为语义目标，4.18 公共源码仅校对接口；未在完整 UE/PS4 工程中构建，其余纯虚方法没有填成成功占位。

- `0x377bc0`：CS 参数 wrapper，public 参数顺序吻合；BaseIndex / NumBytes 分别截为 low16，再写 context+0xae8。`0x369390` 是对应命令 Execute 的内联候选，不能由 word load 反推原 command 字段宽度。
- `0x36a4c0`：立即按当前 CS reflection 绑定 UB 常量，保存原 UB 并置 context+0x91c；资源 map 留到 dispatch 展开。null / constantSize=0 的常量部分取全局默认 UB（指针位于0xa726658），资源引用仍是原 UB，默认 descriptor 内容未知。
- `0x36cb00`：shader+0x1d8 program、shader+0xb0 内嵌 reflection；program 改变令 shader dirty，resources 总标脏，更新 dispatch flags。函数本体没有 all-UB-dirty / loose-dirty 写入；不扩大为全引擎结论。
- `GnmResources.cpp` 保留创建时复制、资源 AddRef、GPU backing 延迟回收接口和逐写 Lock 轮换；引用计数不等于 GPU 完成。

新增 `gnmrhi-ue-bridge.symbols.json` 5 个符号、6 个部分结构；索引 **revision 4，66 个符号、21 个类型、45 个带原型符号**，新增均为 candidate。接口检查器通过 10 个 override 与原始公共 UE 头文件的一致性检查；不沿用旧 C++20 测试结果作为这版 UE 编译证明。未操作设备。

符号 prepare dry-run / 实生成均成功，生成至 `build/validation/kh3-gnmrhi-20261016/symbols-generated-r4/`；clang `x86_64-none-elf` 通过 173 个 size/offset 断言及生成 C 语法检查。四个分片和原始/兼容 ELF 身份校验通过。

## 10. Graphics / draw、VS/PS资源与graphics LCUE（2026-10-16）

新文档：[graphics深入还原](../gnm/kh3-gnmrhi-graphics.md)。C++在 [GnmGraphics.cpp](../../tools/gnmrhi-semantics/ue4-reference/GnmGraphics.cpp)，真实UE接口；共享C++14算法在 `GnmGraphicsCore.h`，由独立测试直接使用。仍以KH3 4.17分支为语义目标。

- `0x382bf0` BSS setter、`0x384390` this-=8 thunk；重标五阶段UB mask为fffff、vertex dirty及discard-loose-extent；无强制loose dirty。VS/PS按已emit标记及native key选择SDK Set/Update。2500项BSS history为CPU引用环，不能当GPU fence。
- `0x37f220` 与UE graphics pipeline fallback关联，但还使用EDX stencil ref，不冒称stock4.18的单参数同ABI方法；BSS factory/完整fixed state仍留边界。
- `0x37e180` stream setter快照当前backing；**修正r3：context+928是stream0地址，declaration在+aa8**。16×24字节stream；declaration元素8字节，BSS remap控制descriptor槽，FirstInstance使用element stride。
- `0x386a40`完整主参数为context/FirstInstance；vertex → VS/PS/GS UB maps → `0x386680` loose。补齐VS/PS/GS UB setter与VS/PS texture/sampler/SRV命令Execute候选，后者不能标成public setter ABI。
- `0x4c3a220`完整伪码2999行，普通顺序VS shader→PS shader→VS table→PS table→semantic linker。表+7…+e以及VS专用+4/+5/+6/+f已在C++实现，区分pointer与marker+scalar。输入base是GNM+2e8，commit只是录DCB，不是queue submit。
- VS/PS语义表按低8位ID找首个匹配，保留Neo位运算；`0x68bb930`写SPI_PS_INPUT_CNTL。表地址公式中program+3是byte，不是DWORD。
- `0x380650` direct primitive、`0x381550` direct indexed；后者NumVertices未使用，index地址在draw执行期间解析。draw后恢复instances1/base0。只闭合triangle list/strip和line list；`0x380f90`是indirect indexed，未当direct实现。
- ES/GS、LS/HS分支已确认存在，但copy shader/ring/tess参数未闭合；参考BSS拒绝GS/tess。SDK shader/draw emitter、prefetch与selection资源副作用经必须实现的平台接口接入，未填成功占位。

新增18 symbols /9 types、补先前三个函数原型；当前 **revision5：84 symbols、65 typed、30 types、5 shards**。prepare dry-run/实生成到 `symbols-generated-r5/` 成功；clang通过285个size/offset断言与生成C语法检查。24个UE override签名匹配；MSVC Debug构建、CTest2/2（旧13场景+新增6组graphics）通过。**完整UE/PS4 SDK工程和设备均未验证；未操作Odin3、未改变闪黑结论。**

静态工件：`decomp/graphics-382bf0.cpp`、`graphics-4c3a220.cpp`、`graphics-381550.cpp`、`graphics-380f90.cpp`及`static/fn_*.asm.txt`。Reverse Study本轮打开已有review.i64快照；未放宽原始ELF与分析ELF的SHA门禁。下一步从GS/ES copy-shader/ring、LS/HS tess参数、shader loader以及UAV/texture状态追踪继续。

## 11. GS/ES、copy shader、ring 与 LS/HS（2026-10-16，r6）

新增[完整阶段分析](../gnm/kh3-gnmrhi-stages.md)、[GnmStagesCore.h](../../tools/gnmrhi-semantics/ue4-reference/GnmStagesCore.h)，扩展[UE graphics实现](../../tools/gnmrhi-semantics/ue4-reference/GnmGraphics.cpp)。仍是KH3 Release-4.17语义，公开4.18仅校对接口。

- **UE GS接线**：BSS+1c0非空且VS+260 alternate ES存在时，VS参数/顶点流走ES4（reflection+268），GS走3，内嵌copy占VS2，PS1。逻辑UE VS的UB引用和loose shadow不迁成copy shader参数。新增GS的parameter/UB/texture/sampler/SRV override与command-list示例。
- **copy shader**：`G+4*byte(G+3)+38`为header，flags在copy+26。`4c3dd50/4c3e010`选择普通/片上profile。`4c3e390`是context初始化，建LCUE+c268/c380/c498/c5b0四个0x118 profile；table0/64DWORD，offchip用selector+c，onchip用+f，extended另用+6。+10a辅助map不是+ea vertex map，完整填充者仍待查。
- **ring owner**：manager全局A7260E0的前缀已接上：+0/+38两份16KiB辅助buffer（其用途未完全闭合），+70 ESGS和+90 GSVS各4MiB/64KiB对齐。分配在3a5640，辅助重建3a8260。不要在A7260E0再建重复符号或改掉原manager名称。
- **ring table**：382bf0比较6个布局key，变化才在DCB NOP内放192byte表，地址GNM+360=LCUE+78；同key复用。4c3ec80生成GSVS四write+一read descriptor，68bbb10发17DWORD布局寄存器。不能混淆manager backing、DCB共享表和LCUE upload table，reset寄存器不是GPU fence。
- **片上GS**：4c37870按64KiB容量算groups和512byte LDS单位，通常最多64组、32 instances时63组；ES+14非零override才替换LDS位，0保留原header。GS/copyVS的+f是LCUE+c6e8/c6ea两个WORD乘积。模式180003/600003与stage-enable b0已入core。
- **LS/HS**：4c3a220生成9DWORD内部常量，descriptor仍records3/format20077fac；reflection+e8先绑HS5，再绑domain所在VS2或ES4。LS reserve23、HS reserve30；HS config对应num_patches/input/output control points；负数override改的是native+18 VGT_TF_PARAM bits17..18，不是HS resource2。代码保留shader未dirty时descriptor复用。
- **顺序边界**：ES/GS块在LS/HS之前。组合GS+tess可能在ES commit后重新标ES resources dirty，尾部不再处理ES；core保留并测试此行为，未判定真实游戏bug。
- **UE tess入口尚未闭合**：3bace0工厂、382bf0 setter、386a40准备只消费VS/PS/GS；但369830 hull UB、37f900 hull texture、36a9e0/36c620 command Execute确实存在。不能断言无tess支持。`CommitLsHs`是给定状态的SDK语义重放，UE BSS仍拒绝tess；patch draw、TF_MEMORY_BASE backing、LS变体/fetch创建继续待查。

[符号index](../../guest/games/CUSA15072/01.10/symbols/index.json) **r6：100 symbols /75 typed /39 types /6 shards**；新stages分片16条目、9部分类型，manager原条目补ring前缀证据。prepare dry-run和实生成到`symbols-generated-r6/`通过；clang365个size/offset断言与生成C语法检查通过。29个UE override签名匹配。MSVC Debug、CTest3/3（原13+graphics6+stages8）通过。**完整UE/PS4 SDK工程、真实GS/tess draw和设备画面未验证；本轮仅静态分析，未操作设备。**

新增完整伪码：`decomp/stages-{4c3dd50,4c3e010,4c3ec80,4c37870,68bbb10,4c3e390,4c3e9f0,3bace0,3a5640,3a9470,3a8260,38f300,369830,37f900,36a9e0,36c620}.cpp`。generation=`dd667cad7bd1fbbb3ccebb78823e22b5857b22066c517c708fbef2960fed29c0`。SQL全局data-xref查空的几处，改用原始RIP扫描核对；不能将空索引解释成无引用。

## 12. Tetris Release-4.26 比对与曝光读回验证（2026-10-16）

后续跨游戏验证已转到 Odin 3 / Tetris CUSA13427，完整内容见 [4.26 分析](../gnm/tetris-ue426-gnmrhi-readback.md)、[实机记录](../validation/android-native-host/tetris-exposure-readback-20261016.md)和 [C++14 参考](../../tools/gnmrhi-semantics/ue426-reference/README.md)。KH3 上述偏移和 r6 符号保持自己的版本身份。

- Tetris ELF SHA `157845ba4297a000ac9cfeb26b7b78925d7d903a2ee34575c74d0f5ede6e9677`，字符串明确为 Release-4.26。Reverse Study 打开被 `database_in_use` 阻止，没有关闭既有 IDA；本轮以 FDE 边界、原始指令、resolved 虚表和 RIP xref 人工重写语义。
- 三槽纹理历史 `0x2170ba0` → readback wrapper → GNM Map `0xfa13d0` / fence Poll `0xf8c390` / context Write `0xf991a0` → ViewState+424 → View+5554 → UB+874/+878 → tonemap 已闭合。纹理三槽与 KH3 动态 buffer 的逐 Lock 轮换不是一回事；偏移均十六进制。
- 闪烁根因为 1×1 linear storage staging 的 GPU 内容未返回 guest CPU，完成标签单独修正无效。新增默认小型 linear storage 写回，同一进程 off→on 从 151 帧 100 黑变为 152 帧 0 黑；最终 APK `b2d42cd7` 默认冷启动再次 151/0，UB 为 2/0.5。
- C++14 参考保留 next/current 消费顺序、pending/Poll、Clear 不清 label、实际 EOP 字、Map/Unmap 和异常正数曝光行为；CTest 4/4。完整 UE4.26 PS4 SDK 工程未构建。
- 后续继续 4.26 graphics/shader loader/LCUE、GS/ES/LS/HS 时，必须重新获取该 ELF 的布局证据；尚未把这些部分恢复为与 KH3 同等深度，也尚未输出 Tetris v2 符号包。此修复不替代 KH3 跑跳闪黑的独立排查。

## 13. Tetris 4.26 graphics / LCUE、LUT范围核查与Swan部署

新增[graphics深入还原](../gnm/tetris-ue426-gnmrhi-graphics.md)、`ue426-reference/GnmGraphicsCore.h`与`GnmGraphicsReference.h`。使用同一ELF的FDE/指令/虚表重新取证，未套用KH3 ABI；Reverse Study仍被database_in_use阻止，未取得Tetris Hex-Rays伪码。

- `f9af50` pipeline候选：G=C+348、L=C+510，secondary interface=C+218；full/diff为`f9f470/f9f840`，static UB按VS→GS→PS。五阶段mask全fffff，vertex/discard标脏，无强制loose dirty。第三参数是apply-static低字节，未当成KH3的stencil ref。
- `f95620`：prepared vertex entry12字节，stride在entry而不在16字节stream。FirstInstance乘积32位后MOVSXD；stride0走format字节数。Prepare为VS maps+loose→PS→GS。
- `f95930/f95be0/f95e90`：UB+28资源，sampler(+220)→texture(+250)→SRV(+210)，PS增加UAV(+230)；清整个频率dirty。unified view binder `f96a20`不包含KH3式backing刷新。
- `3c4de20`普通LCUE：VS/PS Set/Update→各自immutable表→semantic linker；反射offset WORD从+12开始，map从+1e/+7e/+9e/+be/+e6开始；每stage staging仍1800字节。pointer不带marker，raw/scalar带marker；commit不等于queue submit。
- direct draw topology取pipeline+68；indexed current backing在Prepare后、LCUE前读取；indirect区别byteOffset与argumentIndex×20，保留尾部重复NumInstances(1)。SDK及固定状态/retirement为必须接入的后端边界。
- GS alternate ES+278/reflection+280、192B ring表L+78及全局backing来源已关联；完整GS/tess commit、ring owner和shader/pipeline生成端未完成。参考ordinary context显式拒绝这些模式，没有宣布完整UE4.26 GnmRHI可运行。
- C++14 MSVC Debug与CTest5/5通过，新增8组graphics场景；`evidence.json`固定39函数范围，identity/range验证通过。没有新设备GPU执行验证，也未导出Tetris v2符号包。
- **16像素/LUT**：实际LUT是32层RT→tiling19的32³采样纹理，非曝光linear CPU staging。仅放大阈值无法命中其入口/布局/depth/layers条件；无条件CPU写回会增加Finish/retile且不能替代同帧GPU alias。保留≤16默认规则，扩展方案和现有trace证据见专文§9；没有以尺寸猜测CPU读回需求。
- **设备子任务完成**：Swan PB3110PGL6240001G安装APK b2d42cd7，Odin3 CUSA13427四文件5,246,883,930bytes三端SHA一致、游戏库识别。按用户授权删除XRGameNative Steam下MonsterHunterRise约34.54GiB，余30.92GiB；外部存档/设置和99个诊断目录保留。未启动游戏，Swan XR回归未做；记录`build/validation/tetris-transfer-swan-20261016/README.md`。未commit/push本批修改。


## 14. Tetris 4.26 GS/ES、ring、LS/HS 深入恢复与提交整理

按用户要求保持≤16像素策略；新增[阶段专文](../gnm/tetris-ue426-gnmrhi-stages.md)、`GnmStagesCore.h`及9组`stages426_tests.cpp`，扩展UE职责context支持GS及ES indirect。

- `f9d570` PSO工厂仅选VS2/mode0或ES4/modeb0；GS分支通过copy+26 flags分片外/片上，后者用64KiB预算算groups/LDS。native六个ring key仅片外填写，片上保留零。
- `3c4d900`四copy profile位于L+aa44/ab58/ac6c/ad80，stride114（KH3为118）；extended的aux map+106不同于vertex map+e6，完整数据生产端未闭合。copy占VS2并参与PS semantic link，UE VS参数走ES4。
- `fa1c50`两次`fec130(size=400000,align=400000,type=0)`初始化ESGS/GSVS，实际各4MiB/4MiB对齐；不是KH3的64KiB对齐。片外apply分配192B共享表，slots2/3 ESGS、4..7 GSVS write、8 GSVS read；helper不清未知slot也不传播dirty。生命周期仅补初始化，shutdown/free/retirement待续。
- `3c4de20`完整顺序GS块→LS/HS块→VS/PS尾部；tess内部9DWORD常量经reflection+e4同时绑HS与domain所在VS/ES，shader不dirty时复用descriptor。HS负数override改native+18 bits17..18，config高12位由helper保留且LCUE未查明原stack初值，参考显式输入。
- 公共UE Hull/Domain setter及PSO桥未闭合，reference仍拒绝UE tess pipeline；低层LS/HS只给定状态重放。组合mode在ES提交后重新标ES资源dirty的顺序已测试，不擅自宣判游戏bug。
- MSVC Debug、CTest6/6；graphics426 8组、stages426 9组。ELF+71个FDE范围SHA验证通过。测试未执行原二进制/真实GPU，没有本轮设备操作；未生成Tetris v2符号包。
- 本批按任务范围整理commit/push：曝光storage写回修复、4.26分析/参考/验证和进展索引。已有调试器、呈现、KH3闪黑诊断等工作区修改不混入。原ELF/IDA/反汇编/设备包留本地build证据目录。

接续：shader loader的ES/LS变体与reflection、extended copy辅助表、ring释放/退役、TF_MEMORY_BASE及patch draw、UE HS/DS调用者、4.26 queue worker。所有偏移继续受该ELF SHA约束。
