# 慢帧采集接入与 Thor 血源慢帧分析（2026-10-06）

## 1. 接入

- **Foundation**：`codex/shadps4-xr-foundation` 合入 main 到 541063e（本地合并提交 cbe5c9b，未推送），带来
  profiler SDK 的慢帧采集（`SlowFrameCapture`、`ProfilerRing::StartSlowFrames` 等）。冲突处理见合并提交说明：
  `profiler_ring slow ...` 移植进本分支独立的 `ProfilerRingCommands`；overlay、perf_metrics、foveation 保留本分支版本
  （foveation 另补 main 引擎需要的 `Capabilities::SupportsGpuUpload`）。shadPS4 侧 CMake：`spatial::foundation_foveation`
  别名到本仓的窄 foveation 目标，XR 场景库链接 main 拆出的 `spatial::foundation_render_vulkan`。
- **shadPS4**（参考 azahar baa64971c）：DebugBus `profiler_ring slow status|stop|dump <id>|start [factor=2 window_ms=2000
  samples=30 min_ms=0 pre=5 post=2 cooldown_ms=1000 count=8 mib=8]`（选项与 Foundation 命令一致）。帧是 Guest 的
  `sceGnmSubmitDone` 提交周期（原有 `Profiler::Frame()` 标记，不是显示帧）。每次会话开始（Android `Prepare` 完成、
  桌面启动）调用 `Profiler::BeginSession`：写入采集身份（`title app_ver build generation frame=gnm_submit_done`）并
  `ResetSlowFrameHistory`。快照导出到 `<log>/profiler/slow-*.prof` 与 `.prof.json`（含触发值与配置）。
- 桌面 clang-cl 与 Android host/APK 均编译链接通过；Thor APK 52894723。

## 2. 采集

Thor，血源大教堂区，30 FPS 上限，内部分辨率 0.5；先站立、后用 DebugBus 手柄前进并左右转镜头约 64 s。
`profiler_ring slow start factor=1.8 min_ms=50 samples=30 pre=5 post=2 count=16 mib=16`：检测到 9 次，保留 7 份
（2 份在冷却/忙碌期被抑制），基线约 34–36 ms，阈值约 61–65 ms。

读取：Litep MCP 可直接打开（文件需放在 Litep 允许的目录，如 `~/.litep`，`sdk_revision` 填
`0b467a861569345a64fd88cb2e5daa8ff542c14f`）；本页的逐帧拆分另用 SDK 的 `trace_dump --json` 导出后脚本统计。
时间都是 span 的经过时间（含等待），PROF 不含调度信息。

## 3. 结果

每格为“慢帧 / 前后正常帧中位数”（ms，独占时间）：

| id | 帧 | 游戏帧线程等 flip 事件 | 等提交门 | GpuComm Draw | 着色器翻译 | 录制线程等管线 | 管线创建 | vkQueuePresent | GPU 完成等待 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 87.2 | 47.5 / 0 | 0 / 0.3 | 32.2 / 16.9 | 0 | 0 | 0 | 63.1 / 25.2 | 233 / 51 |
| 2 | 95.8 | 53.8 / 0 | 0 / 0.7 | 33.5 / 20.2 | 0 | 0 | 0 | 72.8 / 2.3 | 260 / 56 |
| 3 | 77.6 | 56.0 / 0 | 10.5 / 0.5 | 32.6 / 20.8 | 0 | 0 | 0 | 61.1 / 6.0 | 212 / 53 |
| 4 | 87.3 | 50.3 / 0 | 0 / 2.6 | 39.1 / 22.8 | 0 | 0 | 0 | 64.3 / 1.8 | 235 / 58 |
| 5 | 87.2 | 54.9 / 0 | 0 / 0 | 33.4 / 12.8 | 0 | 0 | 0 | 67.3 / 4.5 | 236 / 48 |
| 6 | 63.9 | 0 | 27.0 / 0 | 43.7 / 19.1 | 0 | 0 | 0 | 0.4 / 9.0 | 61 / 64 |
| 7 | 112.4 | 0 | 72.6 / 0.7 | 14.9 / 15.6 | 28.4 / 4.1 | 63.6 / 0 | 133.5 / 0.1 | 1.6 / 0.5 | 65 / 51 |

每个提交周期的 draw 数（慢帧加 `*`）：1–5 号慢周期 2300–2800 个 draw（正常 1300–1600），其后紧跟一个 12–18 ms、
300–480 个 draw 的短周期；6 号 1906 个；7 号 1171 个，与前后相同。

三类：

1. **呈现阻塞（1–5 号，7 份中 5 份）**：Presenter 的 `vkQueuePresent` 驱动调用阻塞 61–73 ms，GPU 完成线程的等待
   同时变长，游戏帧线程在 `sceKernelWaitEqueue`（等 flip 完成）多等约 50 ms。慢周期与随后的短周期合计约 99 ms
   （三个 33 ms），只装了约两帧的 draw，即实际丢了一帧。场景本来 GPU 99% 忙；在这台机器上 present 的驱动调用会
   等到这一帧的 GPU 工作完成，所以这类慢帧反映的是 GPU 的耗时，CPU 侧只是多处理了约 1.7 倍的 draw。PROF 里
   没有 GPU pass 时间，哪段 GPU 工作变长还需要开 GPU 计时另采。
2. **GpuComm 处理量大（6 号）**：一个周期 1906 个 draw，GpuComm 的 Draw 43.7 ms（中位 19.1），另有 4.1 ms 纹理上传；
   游戏帧线程在提交门等了 27 ms。
3. **新管线（7 号）**：GpuComm 上翻译新着色器 28 ms；编译线程创建图形管线 133 ms；这个 draw 没有被跳过，录制线程
   在 `Pipeline.WaitReady` 等了 64 ms，GpuComm 在 `RecorderSync` 跟着等 64 ms，游戏帧线程在提交门等了 73 ms。

## 4. 补充：按 GpuComm 帧边界复核、开 GPU 计时重采

**“一个周期 2500 个 draw”不是错处理了别帧的工作。** 每个游戏提交周期都正好是一帧的提交（11 次
`SubmitCommandBuffers` + 1 次 `SubmitAndFlip`）；按 GpuComm 自己的 flip（`VideoOut.Prepare`）切分，每帧 draw 数稳定
（约 1500 或 1650，随场景）。上表按游戏线程时间切分，GpuComm 落后游戏处理，慢周期里正好处理了上一帧的剩余部分。
按 GpuComm 帧看，1、2、4、5 号的慢帧里 GpuComm 空闲 40–53 ms：在等游戏提交，游戏在等 flip 完成。

**开 `gpu_timing detail` 重采（5 份，触发 id 10–14）：** 每帧 GPU 上的游戏命令批次 17–31 ms（随位置变化），
慢帧中与前后帧相同，没有一份是 GPU 工作变长。因此第 3 节“呈现阻塞反映 GPU 耗时”的推断不成立：
`vkQueuePresent` 阻塞时 GPU 工作并未变长，等待在显示/合成一侧（BufferQueue），需要 Perfetto 的 SurfaceFlinger
记录才能继续区分。新的 5 份：

| id | 帧 | 主要原因 |
|---|---|---|
| 10 | 170 ms | 纹理流式加载：一帧内 604 次纹理刷新（stage 17 + upload 14 + detile 8 ms），暂存请求合计约 30 ms（十几次各 2–4 ms），GpuComm Draw 101 ms，游戏在提交门等 117 ms；这段时间 GPU 上没有游戏命令 |
| 11 | 97 ms | 新管线：翻译 14 ms，创建 121 ms，录制线程等 61 ms |
| 12 | 81 ms | GpuComm Draw 24 ms（中位 16），无明显单一原因 |
| 13 | 98 ms | 呈现：`vkQueuePresent` 25 ms（中位 8），重绘等 fence 21 ms，GPU 批次不变 |
| 14 | 69 ms | 小幅波动 |

暂存请求的停顿可能来自暂存环的上限（策略的 2 倍，满时等 GPU 释放下一块），也可能是新建 16 MiB 块（含映射预缺页）；
未加计数器区分。

## 5. 局限

- 帧是 Guest 提交周期，与显示帧不是一一对应；1–5 号的 87 ms 包含了下一帧的一部分工作。
- PROF 没有调度、锁和 KGSL 记录；present 阻塞的具体等待对象（GPU 完成还是 SurfaceFlinger 缓冲）未区分。
- 每份快照只有触发帧前 5 帧、后 2 帧；只采了一段约 64 s 的走动。

## 6. 跟进：新管线等待与跳过模式被整局关闭

**管线缓存是有效的。** 只清配方缓存、保留驱动缓存（三轮）：图形管线 203–204 条，每条平均 0.06 ms，最长 1.2 ms；
慢帧里 120–130 ms 的是首次真编译。两份缓存都清（四轮）：图形管线 205–211 条共 2.7–3.0 s（平均 13–14 ms），
计算管线 27 条、最长 262–276 ms（单条，无法跳过）；绑定等待 112–119 次、2145–2297 ms。

**编译线程优先级不是原因。** 冷驱动缓存下默认（编译线程 nice +4）两轮 2145 / 2183 ms，关闭主机优先级两轮
2269 / 2297 ms；等待时长就是编译本身。“等待时临时提高编译线程优先级”的改动做过后已撤回。

**跳过模式被整局关闭的原因：** 被跳过的 draw 漏写的目标在下一帧被读（时间抗锯齿的历史缓冲），记为“跨帧读到缺失内容”，
原规则直接整局关闭跳过。每轮都在开局后十几帧内触发，之后所有新管线都要等。

**改动（`vk_missing_content.cpp`、`vk_pipeline_stats.cpp`、`vk_rasterizer.cpp`）：**

- 读到的缺失范围距上次标记不超过 4 帧（`HistoryFrames`）视为历史/反馈：这一帧会在其上画完整内容，丢失会自行淡出；
  该范围取消标记，读它写出的内容也不标记，跳过继续。
- 距上次标记超过 4 帧视为陈旧内容（只画一次、之后一直用的目标）：跳过暂停 60 帧（`SkipPauseFrames`），清空标记后恢复；
  一局最多暂停 16 次（`MaxSkipPauses`），用完后整局关闭。CPU 回读、间接参数两类仍立即整局关闭。
- 状态：`pipeline_cache status` 增加暂停次数、“skipping paused” 阻挡计数、按历史读清除的范围数。
- 着色器翻译超过 20 ms 时写一行日志（IR / SPIR-V / module 分段、GCN 长度）。

**结果（两份缓存都清，同样的走动脚本，各两轮）：**

| 版本 | 绑定等待 | 被跳过的 draw | 跳过状态 |
|---|---|---|---|
| 原规则 | 112–119 次，2145–2297 ms | 24–30（21 条管线，12–13 帧） | 开局即整局关闭 |
| 仅暂停 60 帧（中间版） | 103–107 次，1880–1900 ms | 93–95（43 条管线） | 暂停 7–8 次：每次恢复后下一帧又读到历史 |
| 按间隔区分历史/陈旧（最终） | 25–28 次，760–836 ms | 1148–1176（180 条管线，55–56 帧，最长连续 4–5 帧） | 暂停 1–2 次（间隔 5、11 帧的读） |

剩余等待主要是计算管线（最长 314–318 ms，dispatch 不能跳过）。截图画面正常。这两轮走得更远一些，
翻译 318–319 个、计算管线 31 条（原规则下 308 / 27）。

**慢翻译：** 两个约 5800–6000 dword 的计算着色器（cs `0x42f2a521`、`0x2da7fe60`）在 GpuComm 上翻译 137–150 ms，
几乎全在转 IR（`TranslateProgram`）阶段，SPIR-V 生成 1.3–1.6 ms；只在首次遇到时发生（之后走配方缓存），未进一步定位。

设备：APK `2ee49f11`；测试后配方缓存与驱动缓存已从备份放回，`debug.shadps4.thread_priority` 为空。

## 7. 跟进：大计算着色器翻译慢

`TranslateProgram` 增加分步计时（整次翻译超过 20 ms 时写一行 `Translation of ... steps (ms): ...`）。cs `0x42f2a521`
（4957 条 GCN 指令，1160 个 IR 块，约 1 万条 IR 指令）共 102–116 ms：`SsaRewritePass` 第一次 57–69 ms、第二次 15–17 ms，
DCE 6 ms、指令翻译 7 ms，其余各 1–2 ms。

simpleperf（`--call-graph fp`，2 kHz，GpuComm）：SSA pass 内约 60% 在 malloc/free（scudo 分配 30%、释放 21%，另有缺页），
几乎都来自 `Inst::SetArg` / `ClearArgs`：每个操作数的“使用者”记录是 `boost::container::list` 的一个堆节点。整个
`TranslateProgram` 中有 37% 的样本栈上有 malloc/free。

**改动：** `Pools::inst_pool` 改为 `IR::InstPool`（指令对象池 + `std::pmr::unsynchronized_pool_resource`），使用者链表节点
从这个池分配，`ReleaseContents` 时整体释放；`Inst` 因此大 8 字节（静态断言 184 → 192）。

**结果：** 同一着色器 83 ms（SSA 第一次 44 ms、第二次 12 ms）。冷配方缓存一轮翻译出的 289 个 SPIR-V 与原缓存逐字节
相同（两个着色器的排列编号因遇到顺序不同而互换，内容集合一致）。桌面 clang-cl 编译通过。

**剩余（未改）：** 首次创建 phi 时新内存的缺页约 15%（只在遇到更大的着色器时发生）；删除平凡 phi 时
`uses.remove()` 线性扫描约 9%；`DefTable` 的哈希表节点约 8%；其余是算法本身：每个汇合块对读到的寄存器先建 phi、
再逐个判定是否平凡并删除，1160 个块时数量很大。要再降需要改成对已封闭块立即求值并删除平凡 phi，改动较大，暂未做。
另一个 cs `0x2da7fe60`（5969 dword，此前 137 ms）在这两轮的走动中没有遇到，规模相近，预计原因相同。

设备：APK `4a268f01`，测试后配方缓存已放回。

## 8. 跟进：SSA 重写改为封闭块立即求值

`ssa_rewrite_pass.cpp` 原实现是 Braun 等人算法的“全部延后”形式：按逆后序读写，汇合块读到变量就建空 phi，
全部遍历完再统一补操作数，最后用工作表删平凡 phi。改为论文的封闭块形式：

- 按逆后序填块；一个块的前驱全部填完即封闭（进入前检查，填完后检查后继）。封闭块里新建的 phi 立即读前驱补齐操作数并当场
  判定平凡，平凡则替换并把读到它的 phi 递归复查；该块与沿途单前驱块记下的定义直接是替换后的值。
- 未封闭块（循环头，回边尚未填）建不完整 phi，封闭时补齐。从入口不可达的前驱会让块一直不封闭，最后统一补齐。
- 递归深度上限 64：更深的 phi 和待复查的 phi 放入队列，由最外层读取处理，避免大着色器栈溢出。
- 已删除的 phi 仍可能被记为某块的当前定义：删除时把替换值放入数组，序号存进该指令的 `definition` 字段（SPIR-V
  生成前不使用，pass 结束清零），读定义时沿它解析。第一版用 `unordered_map` 存这个转发关系，插入/查找/析构占了一半时间，
  SSA 第一遍反而变成 62–80 ms，改为数组后解决。
- 结束时仍跑一遍原来的平凡 phi 工作表，作为兜底。

**结果（cs `0x42f2a521`，冷配方缓存）：**

| 版本 | SSA 第一遍 | SSA 第二遍 | 整次翻译 |
|---|---|---|---|
| 原实现 | 57–69 ms | 15–17 ms | 102–116 ms |
| + 使用者节点内存池（第 7 节） | 44 ms | 12 ms | 83 ms |
| + 封闭块立即求值 | 33–34 ms | 11 ms | 74–75 ms |

一轮（同一走动脚本）翻译 308 个着色器合计 382 ms、单个最长 77 ms；原实现 306–308 个合计 494–511 ms、最长 122–128 ms。

**输出一致性：** 与原缓存逐个比较 SPIR-V（去掉 24 字节缓存封装）。深度上限 64：285 个中 280 个逐字节相同，7 个仅 `OpUndef` 编号或
排列编号不同，4 个仅同一块内 phi 的顺序不同（按“操作码+类型+操作数标签”迭代标注 phi 后排序比较相同）。为覆盖延后队列路径，
临时把深度上限设为 2 跑一轮：314 个中 309 个逐字节相同、5 个等价、0 个不同（其中一轮翻到了旧缓存里没有的另一个排列，
IR 指令数 10236 与 10207 交替出现，那一轮的比较不作数，重跑取到同一排列）。全部 `spirv-val --target-env vulkan1.3` 通过；
游戏内截图正常。桌面 clang-cl 编译链接通过，桌面未运行。`FragmentLdsPass` 内部也调用该 pass，血源这几轮是否走到未确认。

剩余热点：读变量的链式查找与 pass 自身循环约 40%，使用者链表约 11%，新 phi 首次触碰内存的缺页约 10%。

设备：APK `8403442d`（深度上限 64），配方缓存已放回。
