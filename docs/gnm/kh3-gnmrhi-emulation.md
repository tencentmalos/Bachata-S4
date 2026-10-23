# KH3 GnmRHI 命令流、模拟器语义对照、闪黑假设与 Android 性能问题（2026-10-16）

**数据来源**：
- PM4 trace：`build/validation/kh3-flicker-1014/buf1/traces/41519d7fe57468e2fe575acdc0e1da99.*`（16 帧，flip 5660–5676，奥林匹斯）。
- 用 `tools/ps4-gpu-trace/ps4_gpu_trace.py summary/packets/frame` 解码，文本在 `build/validation/kh3-gnmrhi-20261016/kh3_packets_buf1.txt`。
- 背景见 [ue4-gnmrhi.md](ue4-gnmrhi.md)、[gnm-concepts.md](gnm-concepts.md)。

**结论边界**：本文第 3、4 节是候选与建议，尚无一条经过设备 A/B 证实。

## 1. KH3 每帧的命令流形态（16 帧平均，约 750 draw + 30 dispatch）

| 项 | 每帧 | 说明 |
|---|---|---|
| `sceGnmSubmitCommandBuffers` | 约 25 | 来自两个线程；**CCB 全为 0**（不用 CE/CUE，与 4.9 起的 LCUE 一致）；DCB 253–11955 dword，分布在 0x30_0003_0000–0x30_005f_0000 的 64 KB 段里 |
| ACB 提交 | 约 1 | 异步计算 |
| `EVENT_WRITE_EOP` | 154 | 约 143 次把 1 写进 DCB 自身，随后 `WAIT_REG_MEM`（equal 1）等它，即 GPU 原地排空；事件为 BOTTOM_OF_PIPE_TS（0x528），多数带 TC wb/TCL1/TC 动作（0x38528），常与 `ACQUIRE_MEM` 连用、包住 compute dispatch |
| `WAIT_REG_MEM` | 165 | 143 次等自身 label；约 14 次等其它 DCB 段里先前 EOP 写的 label；gfx↔ACB 互等走 `0x20_087xxxxx`；其余等 VO label |
| `ACQUIRE_MEM` | 243 | `RHITransitionResource` 的缓存同步（4.11 起必须显式） |
| `DMA_DATA` | 约 495 | 几乎全是写 0x3022C 的 L2 预取；只有 2 次是填 0 的 fill（`0x20_08702000/400`，复位 compute 计数和 ACB label 区）；**没有 memory→memory 拷贝**，即这 16 帧没有 GPU 碎片整理搬移 |
| `EVENT_WRITE` 0x138/0x139 | 43 对 | 遮挡查询 begin/end（PIXEL_PIPE_STAT），结果在 `0x30_0007xxxx` |
| `EVENT_WRITE_EOS` | 16 | |
| `RELEASE_MEM` | 3 | ACB 写 label |
| `WRITE_DATA` | 1 | 只有驱动 flip label |
| 时间戳 EOP（data_sel=4） | 若干 | `r.PS4TimeSubmitWaits` 和 GPU 计时用 |

**绑定的数据**（frame 0）：
- 常量缓冲 b0/b1 主要在 `0x20_5c......`（约 670 次/帧），推测是 GnmRHI 的每帧临时常量环。
- 另有 `0x20_021e....`、`0x20_37......` 等区域。
- 每帧有 8 次 64 KB 的 VB 在 `0x20_5c8.....`，应是 `FGlobalDynamicVertexBuffer` 的 Volatile 块。

**宿主侧**：每帧约 128 个 render pass。结束原因：

| 原因 | 次数 |
|---|---|
| state_change | 63 |
| sampled_image | 43 |
| buffer_upload | 9 |
| 其它 | 13 |

其中 buffer_upload 多出现在 64 KB 动态 VB 被重新上传时，会出现连续几个 1-draw 的 pass。

## 2. 与本仓 Liverpool 语义的对照

| GnmRHI 依赖的行为 | 真机 | 本仓（Android 默认） | 影响 |
|---|---|---|---|
| EOP label 何时可见 | GPU 执行到管线末端后 | 默认 parse 模式在 CP 解析时写；completion 模式在宿主 GPU 完成后写。但 CP 用待定值放行等待后，紧接的 `WRITE_DATA` 会触发 `SettleWaits` 提前结清（血源 99.7% 的 fence 如此） | KH3 每帧 143 次自等待，completion 模式实际也很少推迟游戏对 label 的观察。因此 10-14 “completion fence 已排除”不能说明 label 时序与闪黑无关 |
| label 写入是否被缓存跟踪 | — | EOP/EOS 经 `TryWriteBacking` 直写物理后备，缓冲/纹理缓存不知道；`RELEASE_MEM`/`WRITE_DATA` 用 memcpy，会缺页 | KH3 的 label 多在 DCB 自身与 `0x20_087xxxxx`。后者与 compute 的 4 字节计数 V#（`0x20_08702000`）同页，影响面待查 |
| `ACQUIRE_MEM` / 缓存动作 | 显式 flush/失效 | 空操作，屏障由缓冲/纹理缓存的用途跟踪推出 | 10-14 的 `full_barriers` 对照已排除“缺屏障” |
| draw 读到的数据 | GPU 执行时读内存 | 解析时快照：stream 拷贝、驻留缓冲脏页上传、纹理上传都在解析时录制，宿主不直接映射 guest 内存给 GPU | 只有在宿主侧把已录制的数据更新早了（WAR），或解析时 guest 数据已是另一帧，才会读错 |
| 遮挡查询结果 | 真实 ZPASS 计数 | 假计数，end−begin 恒正，永远可见 | 游戏不会因查询剔除任何东西，GPU 照画所有图元；查询本身仍有 43 对 draw，渲染线程仍以 bWait=true 等上一帧结果 |
| GPU 时间戳 | GPU/系统时钟 | 宿主纳秒，解析时取 | stat unit 的 GPU 时间无意义；KH3 有没有据此调分辨率未核实 |
| 提交 | 硬件环 | 每次 submit 都复制 DCB，再一次 `ProcessGraphics` | 每帧 25 次 |

## 3. 闪黑：候选原因与实验（未证实）

**已知条件**（[10-14 记录](../validation/android-native-host/kh3-flicker-controls-20261014.md)）：
- 需要 rwlock 快路径与叶子 HLE 的 DrawIndex 系列同时开启。只开 dispatch 组为 0 次，只开 DrawIndex 为 7 次/5 min。PM4 字节与 HLE 一致。
- 在 guest 的 DrawIndex 编码后加 `mfence`，仍有 2 次；FEX 向量/REP 按 TSO 排序，仍有 3 次。
- 坏帧：
  - 黑区与可见区之间是屏幕空间的直线：一条斜线、几条直线围成的多边形，或右侧竖直黑边；可见区内容位置正确。
  - 也有全黑只剩 UI、整帧缺阳光、另一相机角度且缺天空（10-15 strip15245）。
- **guest 侧正常**：buf1 trace 第 8 帧（flip 5669）是一次全黑帧。与同奇偶帧相比，命令、上下文寄存器、guest 缓冲内容、宿主事件序列都无异常差别。
- **已排除**：
  - 纹理缓存的启发式：`pool_undefined/pool_revive/expand_twin/tiny_writeback/adopt_mips`；
  - pass hoisting、predication、提交 HLE；
  - completion fence、写跟踪预测、`pool_map` 写锁内、管线跳过；
  - `full_barriers`、`dcb_verify`（guest 不改已提交的命令缓冲）、`cp_delay`/`submit_drain`（与 guest 领先量无关）；
  - `vk_recorder off`、Turnip `sysmem`、命令 BO 对齐；
  - 坏帧前后 ±300 ms 无纹理重传，`gpu_data_overwrites` 为 0；
  - present 合成（10-16 `present_compose=0` 仍闪）；
  - CE/DE：KH3 不用 CCB。

因此问题在宿主执行侧。

### 3.1 10-16 设备实验

条件：
- 设备 Odin3，脚本 `leafrun2.sh all 1`（快路径全开，自动跑跳 5 min）；
- 证据在 `build/validation/kh3-flicker-1016/<轮次>/`（录像、`strips/`、日志）；
- 闪黑次数是 `dipstats.py` 检测数，经逐帧截图确认。

**实际设置**：
- KH3 每游戏设置是 `internal_scale 1.0`，logcat 为 `Guest internal scale=100.0 percent`，即今天的所有轮次都是 100% 内部分辨率。
- DMA 是开着的：`Guest direct memory access=true`。

| 轮次 | 条件 | 帧数 | 检测 | 逐帧确认 |
|---|---|---|---|---|
| compose0-1 | `present_compose=0` | 4667 | 3 | 2296 斜线黑区；2541 整片纯蓝天空；2306 未看 → 确认 2 |
| ss1-1 | `scaled_storage_off=1` | 4683 | 1 | 2720 绿光、2722 粉白过曝，更像游戏特效，**不算** → 确认 0 |
| ffin-1/2/3 | `gpu_fences frame_finish on`（每帧结束等宿主 GPU 空闲，新诊断开关） | 3106 / 2971 / 2958 | 0 / 0 / 3 | 2640、2794 黑区带斜边；2063 敌人近景压暗，**不算** → 确认 2 |
| ffoff-1/2 | 同一 APK，开关关 | 4664 / 4828 | 1 / 2 | 3152、3092 斜线黑区；4239 全黑只剩 UI → 确认 3 |
| dma0-1/2 | KH3 每游戏关闭 DMA | 4525 / 5084 | 0 / 1 | 1155 屋顶地面整块黑、墙和角色正常 → 确认 1 |
| nocb-1…6 | `TU_DEBUG=csalign64k,nocb`（关并发 binning，LRZ 开着；日志确认） | 5033 / 5141 / 4163 / 3440 / 4921 / 4521 | 0 / 0 / 1 / 1 / 0 / 0 | nocb-3 第 2711 帧远景建筑消失、只剩暗天空和近墙 → 确认 1；nocb-4 第 1019 帧是前后两帧过曝发白，**不算** |
| **nolrz-1…5** | `TU_DEBUG=csalign64k,nolrz`（日志确认 `csalign64k,nolrz,gmem_warmup`） | 4178 / 5851 / 4547 / 7674 / 4789 | 0 / 1 / 0 / 0 / 0 | nolrz-2 第 1533 帧是镜头被角色挡住时的抖动淡出，**不算** → 确认 **0** |

闪黑的检测器会把游戏自己的特效误报成坏帧（抖动淡出、镜头近景压暗、闪光），所以表中“检测”只做候选，以逐帧截图为准。

**统计**：
- LRZ 开着的 9 轮共 37486 帧，确认 8 次（约 0.21 次/千帧）。
- LRZ 关闭的 5 轮共 27039 帧，同样的频率下期望约 5.8 次，实际 0 次；泊松概率约 0.3%。
- `nocb` 六轮共 27219 帧确认 1 次，期望 5.7 次，概率约 2%：关并发 binning 也明显降低，但没有降到 0。
- 解读 `nocb` 要小心：Turnip 在 `lrz.fast_clear` 关闭且有 LRZ 深度视图时本来就关并发 binning（`tu_cmd_buffer.cc` 的 `tu7_emit_concurrent_binning_start`，原因 “LRZ fast clear disabled”），而 fork 在 A8XX 上默认关了 LRZ fast clear（`04e1d665b27`）。所以带深度的 pass 已经没有并发 binning，`nocb` 只多关了不带深度的 pass，降低量里有一部分可能只是时序改变。
- 样本仍然不大，但足够把 LRZ 当作主要嫌疑。

**结论**：
- **渲染倍率规划**：100% 下照样闪，排除。
- **宿主资源跨帧复用过早**（stream 环、staging、描述符堆、GC 释放）：`frame_finish` 让每帧结束时宿主 GPU 空闲，仍出现确认的闪黑，排除。头两轮 0 次是偶然，说明单轮 0 次不能当结论，每个条件至少要 3 轮。
- **DMA**：DMA 开着的会话里 `upload_diag status` 显示 `dma_bounds bounded_calls=0 full_calls=0`，KH3 从未执行 DMA draw；关闭 DMA 也照样闪，排除。另外，关闭 DMA 会改变着色器编译方式（`ReadConst` 一律走 flatbuf），对 KH3 画面无影响。
- **异步计算交错**：
  - 每帧一次 ACB：先 `WAIT_REG_MEM` 等图形队列写“输入就绪”，再 200×113 组的 1600×900 全屏 compute，最后 `RELEASE_MEM` 写“完成”；图形队列再等这个“完成”。
  - 这对 label 从约 6 个槽位（`0x20_08702fc8..ff8`、`c00/c08`）的池里分配，只写 1，复位由游戏 CPU 完成。
  - buf1 16 帧（含坏帧）中，ACB 的等待都在本帧图形队列写 label 之后才放行，dispatch 都在图形队列等待之前录入，顺序正确。
- **坏帧不是游戏剔除**：buf1 第 8 帧的 draw 数与前后帧一致（776，1600×900 目标 333，阴影 237，按相机移动平滑变化）。dma0-2 那种“单个物体整块黑”是否另有原因，未定。
- **对照方法要改**：KH3 有按周期 3 轮换的资源（1920×1080 D32S8 深度在 `0x205e/0x205f/0x2060_000000`，第 15 帧换到新地址 `0x205c...`），显示缓冲按周期 2 交替。与坏帧比较应取间隔 6 帧的同相位帧（第 8 帧对第 2、14 帧），而不是 10-14 用的第 6、10 帧。

### 3.2 与 Tetris “曝光读回” 的对照（10-16 晚）

[Tetris 那个问题](../validation/android-native-host/tetris-exposure-readback-20261016.md)：UE4.26 的 GPU 把曝光值拷进一块 CPU 可读的线性小图，CPU 把它读进下一帧的 View 常量（`PreExposure`），而模拟器没有把 GPU 结果写回 guest 内存，CPU 读到残留垃圾，下一帧整屏变黑或颜色溢出，约每 3 帧黑 1 帧，必现。

**KH3 没有同样的回路，依据：**
- **没有这个字段**：eboot 里找不到 `PreExposure`、`OneOverPreExposure`、`LastExposure`（ASCII 和 UTF-16 都是 0 个）。UE4.17 的眼适应只存在于 GPU 一侧。
- **曝光目标存在，但从不写回**：
  - KH3 每帧有一条 `64×32 → 32×16 → 16×8 → 8×4 → 4×2 → 2×1` 的 R16 降采样链，终点是一个 1×1 `R32G32` 目标（`0x2002e30000`，不同次启动的 trace 里地址相同），随后被后处理和几十个片元着色器采样。
  - 设备上读这块 guest 内存：6 秒内字节恒定，是启动初期的残留（`-2.89, -0.79`）。
  - `upload_diag status`：`tiny_writeback writebacks=89`（启动期后不再增长），`per_frame_skips` 每 5 秒涨约 190，即每帧约 3 个 ≤16 像素的小目标（1×1、2×1、4×2）被“连续每帧都写的目标不写回”规则跳过。
- **但 CPU 不读它们**：
  - 用 guest 调试器在设备上对这些地址设读监视：`0x2002e30000`（曝光值）15 秒，降采样链 `0x202cd55700/5600/5500/5400/5000/4000` 12 秒，**零命中**。
  - 读监视在 KH3 里是有效的：同一轮对提交线程每帧都读的 GnmRHI 帧序号设监视，立刻有大量命中（带 `watch-rip`）。读监视最长 8 字节，所以只能监视这些起始地址，不能整块监视。
- **没有 GPU 写进 CPU 区域**：16 帧 trace 里 compute 写的 buffer 一共 24 段（4 字节的异步计算计数、若干 16 KB 的 CMASK/HTILE 类元数据、几块 0.1–1.8 MB 的数据），都在 `0x20_…` 池内存；没有写向 `0x30_…`（CPU 可见的 Onion 区，命令缓冲就在那里）的 GPU 写入。
- **症状也不同**：Tetris 是确定性的，每 3 帧一次，整屏统一变黑或变色；KH3 是每 5 分钟 1–2 次的随机偶发，黑块带直线边，画面其余部分正常。曝光错误只会让整屏亮度成比例变化，做不出带斜边的黑块。

**结论**：KH3 的闪黑不是 Tetris 那类 GPU→CPU 读回。

**留下的隐患（与 KH3 闪黑无关）**：`tiny_writeback` 对“每帧都写的小渲染目标”不写回，所以这类目标在 guest 内存里一直是旧值。KH3 恰好没有 CPU 读它们；换一个 CPU 会读每帧小渲染目标的游戏就会踩到。Tetris 的 `tiny_linear_writeback` 只覆盖线性的小 storage 图，不覆盖这种渲染目标。

**KH3 里过曝闪光帧的归属**：前面逐帧看过的两处“前后帧发白、中间帧正常”（`ss1-1` 的 2720/2722、`nocb-4` 的 1018–1020）我当作游戏特效处理，没有证据说明是曝光值出错；曝光目标在 GPU 上是正确生成并由 GPU 采样的，guest 里的旧值不影响它。

### 3.3 剩余候选

1. **Turnip LRZ（已确认参与）**：
   - 10-14 只测过 `sysmem`（关 GMEM），sysmem 下 LRZ 仍然生效，所以当时没有排除它。
   - 如果某个 pass 用了过期或不属于它的 LRZ 缓冲，像素会按旧几何的轮廓被整片拒绝。这能同时解释直线边界的黑区、整块缺失的物体和整屏全黑。
   - 纯蓝天空（compose0-1 第 2541 帧）这一类不一定出在 LRZ；是否同源待查。
   - fork 在 A8XX 上已有多处 LRZ 规避：默认关 LRZ fast clear（`04e1d665b27`），render pass 里有 LRZ 读改写时关并发 binning（`fa9873b2cab`）。
   - Turnip 在有 GPU 方向跟踪的芯片（A7XX 及以上，含 A8XX）上，LOAD 深度的 render pass 会沿用上一次的 LRZ 状态（`tu_lrz_init_state` 的 `reuse_previous_state = !clears_depth`），包括上一个 pass、甚至上一个命令缓冲留下的。沿用的前提是：两次之间对这张深度的每一次写入都经过 LRZ 感知的路径。读到的 Turnip 路径（blit、buffer→image 拷贝、image→image 拷贝、resolve、清除）都调用了 `tu_disable_lrz`/`tu_lrz_clear_depth_image`，没发现遗漏。
   - **下一步实验**（要改 Turnip，需先问用户）：加诊断开关 `lrznoreuse`——LOAD 深度时不沿用 LRZ、清除时仍用 LRZ。不闪就说明是“沿用过期/不属于它的 LRZ”，再去查哪次深度写入没让 LRZ 失效；仍闪则是 LRZ 在清除后的使用本身有问题。
   - **可行的缓解**：A8XX 默认或按游戏关闭 LRZ。KH3 上 `nolrz` 五轮的录像帧数不低于 LRZ 开着的（均值约 5400 对 4780，场景不严格对齐，只说明没有明显变慢）；对其他游戏的帧率影响还没测。
   - KH3 里最可疑的用法：主深度 `0x200a000000`（1600×900 D32S8）每帧被多个 pass 反复载入，并且在 pass 之间被“hoist conflict”拷贝改写（`copy writes 0x200a000000-0x200a700000, pass wrote it`）。这类 render pass 之外对深度的写，是否让驱动的 LRZ 失效，是重点。
2. **纹理缓存的图像选择与内容**：同地址、同格式的图像（周期 3 的深度、按需换块的临时池）在个别帧被换成另一张，或在帧中途被“hoist conflict”拷贝改写。
3. **GPU/驱动执行本身**：需要 GPU 回放或 RenderDoc 抓到坏帧后比较。

**观测工具**：
- gcmdtrace 记录每个绑定的前 192 字节与哈希。
- 按 4.18 `SceneView.h:586-650` 的成员表，view 常量开头是 TranslatedWorldToClip 等矩阵，FrameNumber/StateFrameIndexMod8 在表末（`:649-650`）。KH3 改过引擎，实际偏移未核实。

## 4. GnmRHI 在 Android 版上的性能问题

| # | 问题 | 依据 | 方向（未实施） |
|---|---|---|---|
| P1 | 遮挡查询仍在等，但结果恒为“可见” | 43 对/帧，`r.NumBufferedOcclusionQueries=1`、bWait=true；Guest-57 在 `RHIGetRenderQueryResult` 空转（已用 usleep 补丁缓解） | 结果本来就恒可见，可考虑 guest patch 让查询立即返回可见，或经 ini/cvar 关闭 `r.AllowOcclusionQueries`，省掉等待和 43 对查询 draw。反过来，实现真实查询能让 GPU 少画被遮挡物，但渲染线程要等宿主 GPU 上一帧完成，需评估 |
| P2 | 每帧 143 次 GPU 自排空 | EOP 自写 + WAIT_REG_MEM | parse 模式几乎零成本；若要在 Android 打开 completion 模式，先识别“EOP 写 DCB 自身 + 紧跟 WAIT”的成对模式并按管线屏障处理，否则每帧要 143 次宿主往返 |
| P3 | 边录边交，每帧约 25 次提交 | ContinuousSubmits=1、SplitSize=32768 | 每次提交都有 HLE 入口、DCB 复制、Liverpool 唤醒和 GpuIdle 中断。可以 A/B：把 `r.PS4ContinuousSubmits`（运行时 0xaafc514）设 0，或加大 SplitSize（0xaafc50c），看 GpuComm 与渲染线程的 CPU 变化 |
| P4 | 动态 VB/IB 与临时常量每帧换地址 | Volatile 64 KB 块、view/uniform SingleFrame 新内存；每帧约 5400 次缓冲数据事件、约 7 次/draw | 小块走 stream 拷贝，成本在 GpuComm 的拷贝和写跟踪缺页上；64 KB 块超过 stream 阈值，走驻留上传并切断 render pass（每帧 9 次）。可评估提高 stream 上限，或对 Volatile 区按“每帧全写”的特点免跟踪 |
| P5 | 显式屏障被忽略，靠推断切 pass | `ACQUIRE_MEM` 243/帧为空操作；每帧约 128 pass，其中 43 次因 sampled_image 结束 | 在 Adreno tile 架构上每次切 pass 都要 load/store。可研究按 `ACQUIRE_MEM` 的实际 flush 范围合并或推迟切分（须保证正确性） |
| P6 | 异步计算在单队列上不并行 | ACB 约 1 次/帧，gfx↔ACB 用 label 互等 | 宿主只有一个队列，`AsyncComputeBudgetMode`（CU masking）无意义；互等只造成协程切换。影响小 |
| P7 | GPU 时间戳是假的 | EOP data_sel=4 | 若 KH3 用 GPU 时间做动态分辨率或预算（未核实）会误判；stat unit 无意义 |
| P8 | GPU 碎片整理搬移 | 池 2300 MB；`r.PS4.GPUDefragPerfTest` 说明是 GPU memmove | 发生时会有大块 CopyBuffer 和新地址重建纹理；需要先观察是否出现 |

已完成的相关优化：叶子 HLE（draw/marker/clock/TLS）、rwlock guest 快路径、`RHIGetRenderQueryResult` 与帧率上限等待补丁。见 [KH3 loading 与快路径](../validation/android-native-host/kh3-loading-tls-memlock-20261013.md)。

## 5. 对照 OpenGNM/GPCS4 时发现的本仓代码问题（与 KH3 闪黑无关，或未验证）

- **`WAIT_ON_DE_COUNTER_DIFF` 判断反了**（`liverpool.cpp:745-750`）：
  - 写成 `(de_count - ce_count) >= diff`，计数是 u32，等价于“等到 ce == de”。比硬件更保守，不会读到陈旧数据。
  - 但若 CCB 尾部在 ce > de 时停在这个包上，提交末尾的排空循环会一直转。
  - 正确写法是 `ce_count - de_count >= diff`；`liverpool.h` 里未使用的 `Diff()` 已是对的。
- **嵌套 DCB `IndirectBuffer` 清零 `cblock` 计数**：进入嵌套的 `ProcessGraphics` 时会 `cblock.Reset()`，且嵌套任务的 `ce_task` 为空，`WAIT_ON_CE_COUNTER` 会对空句柄调 `.done()`。只影响用 CCB 的游戏。
- **CE RAM**：`constants_heap` 是全局静态的 48 KB；`WRITE/DUMP_CONST_RAM` 无越界检查；`LOAD_CONST_RAM` 未实现，会落入 UNREACHABLE；`DUMP_CONST_RAM` 的 increment 位被忽略。
- **completion 模式下待定值不结清**：只有 `WRITE_DATA` 和信号量前调用 `SettleWaits()`，而 `DMA_DATA` 的 CPU 路径、`COND_EXEC`、间接参数会读到尚未写入 label 的内存。Android 默认 parse 模式，不受影响。
