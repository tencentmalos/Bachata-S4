# KH3：池内存与类型切换的 GPU 目标不再整图重传（2026-10-12）

KH3 问题 #12（[问题汇总](../../../bugs/kh3-cusa15072/README.md#12-每帧纹理整图重传)）。

- 设备与驱动：AYN Odin3（Adreno 830），Turnip 主干 `65b63f66bf5`（驱动 `a847eb5a…`）。
- 游戏：Ver 1.10。
- 证据：在本机忽略目录 `build/validation/odin3-kh3-preempt-20261012/` 下。
  - `F`–`J`：上午的 v1–v4。
  - `K-cs-targets`：v4 上的逐条上传日志。
  - `L-cs-targets2`：v5 的计数、A/B、截图和 simpleperf。

## 结论

“觉醒之所”平台，v5 包（APK `d12d487c`，host `e9a3d834`），同一进程交替 7 个窗口，每个窗口 10 秒，关闭 `upload_diag`：

| 配置 | 帧率 | GpuComm（CPU 时间/10 s） |
|---|---|---|
| 全开（默认） | 33.2、33.6、34.3 | 2.70–2.73 s |
| 全关（相当于本问题修复前） | 29.4、29.0 | 6.12–6.28 s |
| 关 `pool_revive`，其余开 | 34.0、34.0 | 2.92–2.96 s |

- **帧率**：约 +15%。
- **GpuComm CPU 时间**：约 −56%。
- **每帧上传**：约 12 次降为 0。
- **剩余开销**：GpuComm 采样中 `RefreshImage` 从约 56% 降到 1.3%。
- **对比上午的 v4**：同场景开启时 GpuComm 为 4.62 s，帧率约 31.6。
- **画面**：三种配置的截图差异（均值 2.4–3.1）不超过同一配置两张截图之间的差异（2.1–3.7）。菜单画面与 v4 的平均差为 0.45。
- **暂停画面**（锁 30 FPS）：此处只剩 LUT 上传，`expand_twin` 开/关的 GpuComm 为 2.88 对 2.94–2.97 s/10 s。
- **GPU 故障**：本轮没有出现。

开关（DebugBus `upload_diag`，都默认开），计数在 `upload_diag status` 中：

- `pool_undefined on|off`：GPU 先写的目标在新提交、CPU 没写过的池内存里不上传；
- `pool_revive on|off`：池回收时保留 GPU 写过的图像，重新提交后原样复用；
- `expand_twin on|off`：类型切换时复用停放的图像，copy 覆盖整图时不上传。

## 原因

1. **池回收/提交释放图像。** `memory_describe` 显示 1600×900 目标所在地址每隔几帧被 Guest-44 池回收（`pool-decommit`）、Guest-57 池提交（`pool-commit`），每次映射的物理页不同；同一地址有时放 R16G16B16A16，有时放 B10G11R11。两者都调 `TextureCache::UnmapMemory` 释放区内图像。下次使用时重建的图像带 `Dirty`，从 guest 内存整图上传。guest 内存里是新物理页的残留，从来不含 GPU 渲染的结果。上传主要成本是 `ObtainBufferForImage` 的暂存拷贝（`__memcpy_aarch64_nt`）和随后的解平铺与拷贝命令，并会打断当前 render pass。
2. **彩色渲染目标先刷新、后标记（v4 剩下的“先被采样”）。** `Rasterizer::BeginRendering` 先调 `UpdateImage`（刷新、上传），之后 `FindRenderTarget` 才设置 `usage.render_target` 和 `GpuModified`。新建的渲染目标第一次刷新时还不知道自己是渲染目标，于是不跳过上传，被计为 `sampled_undefined`（每帧约 2.9 次）。存储图像和深度目标是先标记、后刷新，没有这个问题。
3. **两张 LUT 每帧在二维数组和 3D 之间切换。** 两张 32×32 A2B10G10R10 LUT 每帧先以 32 层二维数组出现，再以 32×32×32 体纹理出现，各 2 次（地址每次启动不同）。从上传记录推断，二维数组一侧大概率是渲染目标，未逐 draw 核实。同地址、同大小、其中一方是 3D 时，`ResolveOverlap` 调 `ExpandImage`：新建图像，先从 guest 内存整图上传，再从旧图 GPU 拷贝覆盖，最后释放旧图。每帧 4 次上传全部浪费（guest 内存从未变化，outcome 都是 unchanged），另有 4 次建图、释放。

   **更正**：上午记录说两张 LUT 由计算着色器 `0x0eb6153695de6e61` 写入，这是误判。该着色器以 R32 存储缓冲写 `0x2003c00000` 起的 0x1c0000 字节，那里是另一张格式无法识别的 1600×900 图像，与 LUT 无关。`upload_diag` 原来的“image-writing dispatches”只统计写缓冲且起始地址正好是某张图像的 dispatch，名字有误导，已改为“buffer writes at image bases”。

## 改动

- **池提交/回收挂钩**：
  - `PoolCommit` 调 `RasterizerHooks::CommitPoolMemory`，`PoolDecommit` 调新的 `DecommitPoolMemory`（默认实现分别是 `MapMemory`、`UnmapMemory`）。
  - Vulkan 光栅器在提交时调 `TextureCache::MarkUndefinedContent`；回收时调 `TextureCache::DecommitMemory`。
- **只保护学过的页**（v2 起）：
  - 纹理缓存记录放过“GPU 先写”图像（渲染、深度、存储目标）的页（`rt_history`）。
  - 提交时只对其中的页各加一个写关注者（整段写保护），并把这段记为独立的未定义段（`separate_interval_set`，每次提交一段）。
  - 页上已有其它关注者时不加：旧映射的保护可能已丢失，CPU 写可能看不到，这段继续上传。
- **何时失效**：
  - CPU 写入（写缺页或宿主写，经 `InvalidateMemory`）：整段变为已定义，并从学过的页中去掉，以后不再保护。
  - 池回收或解除映射：释放对应范围。
  - GPU 图像回写到 guest 内存（`RecordImageDownload`）、缓冲回读（`Rasterizer::ReadMemory`）：释放对应范围。
  - 一段从提交到回收都没有图像用过：回收时从学过的页中去掉，下次用到时再学。
- **跳过上传**（`pool_undefined`）：
  - 条件：`RefreshImage` 中，图像是渲染、深度或存储目标；整个范围落在一个未定义段内；图像自身已被跟踪；缓冲缓存在该范围内没有 GPU 数据。
  - 结果：清掉 `Dirty`、不上传，内容未定义。新提交、CPU 没写过的内存在 PS4 上同样没有确定内容。
  - 未定义段不随跳过消耗，同一地址上换格式的别名图像也可以跳过。
- **用途提前标记**（v5，修原因 2）：
  - `PrepareRenderState` 在查找颜色和深度目标后立即设置 `usage.render_target` / `usage.depth_target`，早于本 draw 的纹理绑定和 `BeginRendering` 的刷新。
  - `BindTextures` 第一轮查找时，对写入的绑定设置 `usage.storage`：同一次 dispatch 里先刷新的采样绑定也知道这张图会被写。
  - `FindTexture` 在刷新前设置 `usage.storage`（v3 起）。
- **池回收保留、再提交复用**（v5，`pool_revive`）：
  - **保留**：`DecommitMemory` 中，GPU 写过且 CPU 没写过（`SafeToDownload`）、整张在回收范围内的图像不释放，取消注册后放进保留表。
    - 上限 32 张、256 MiB，超过时删最老的；240 次提交未用到也删除，有内存压力时全部删除。
    - 同时把它的范围记为学过，下次提交时受写保护。
  - **复用**：`FindImage` 新建图像前，若范围内没有任何已缓存图像，取信息完全相同的保留图像，原样注册复用，内容不变，相当于这次提交拿回了原来的物理页。
  - **不复用的情况**：范围已被 CPU 写过（不在未定义段内），或缓冲缓存在该范围有 GPU 数据（计为 `stale`）。
  - **其它映射**：解除映射或非池映射会删除重叠的保留图像。
  - **复用之后**：图像标为 `GpuModified`、不带 `Dirty`；缓冲同步版本作废（缓冲缓存已随回收失效）；标记所在段已用过，以保持该段被学到。
- **类型切换停放复用**（v5，`expand_twin`，修原因 3）：
  - `ExpandImage` 先判断拷贝能否覆盖整个新图像（`CopyCoversImage`）：纹素网格、位宽、采样数相同，层级与层数（3D 与二维数组之间为切片数）不少于新图像。
  - 能覆盖时：
    - 源图的 guest 或缓冲数据更新（`CpuDirty` / `GpuDirty`）：只从 guest 内存或缓冲上传新图像，不再用过期的源图覆盖。原实现先上传再拷贝，会把过期内容盖在新数据上。
    - 否则：只拷贝；先按哈希处理 `MaybeCpuDirty`；新图像继承源图的 `GpuModified`。
  - 双方互相覆盖（纯粹的重新解释，如 3D ↔ 二维数组）时，旧图停放而不是释放，下一次反向转换直接取回（`TakeParkedImage`），不再建图。
  - 不能覆盖时（资源扩大、压缩视图）行为不变。
  - 停放上限 4 → 8；取回时清掉绑定标记。
- **锁**：未定义段与学过的页由独立的 `undefined_mutex` 保护，取锁顺序在纹理缓存 `mutex` 之后、页管理器锁之前。下载回写有时不持 `mutex`，也只取这把锁。保留表由纹理缓存 `mutex` 保护。
- **诊断**（只在 `upload_diag start` 期间记录）：
  - 每次上传记录刷新时的用途（T 采样、S 存储、R 渲染目标、D 深度）、建图方式（fresh / expand / expand_twin / twin_reused / revived / depth_copy）、当前 draw 或 dispatch 的着色器，以及同一范围上一次被释放的 GPU 图像（写入方式、格式、距今帧数、是否被保留、写入它的着色器）。
  - `status` 增加“upload causes”和“uploads over freed GPU content, by its writer”两张表，以及 `expand_twin=`、`pool_revive=` 两行计数。
  - 图像记录最后一次以写方式绑定它的着色器和方式。

## 迭代与负结果

| 版本 | 做法 | 结果 |
|---|---|---|
| v1（APK `f7afc97d`） | 每次池提交都整段写保护，首个图像刷新时消耗，只针对渲染/深度目标 | 作废。“7颗心”画面开 7–8 FPS、关 17 FPS：游戏也在池内存里放 CPU 写的数据，`cpu_writes` 累计约 150 万次缺页；保护操作还持纹理缓存锁。测量窗口内一次也没跳过 |
| v2（`9edc61c7`） | 只保护学过的页；CPU 写整段失效；独立锁 | 暂停画面 GpuComm −17%；“7颗心”无效果：那里的图像先当存储图像写入，不经过判断 |
| v3（`724d72e9`） | 加入存储目标 | “觉醒之所”帧率与 GpuComm 都有收益（开着 `upload_diag` 测，数字偏大） |
| v4（`8792906e`） | 回收时去掉没用过的段 | 加保护的段数在暂停画面降到约 1/6，跳过次数不变。关诊断：31.4 对 28.8 FPS，GpuComm −24% |
| v5（`d12d487c`） | 渲染/深度/存储用途提前标记；池回收保留、再提交复用；`ExpandImage` 停放复用、覆盖时不上传 | 见结论 |

v5 运行中的 10 秒差分（348 帧，34.8 FPS，开着诊断）：

| 每帧 | 次数 |
|---|---|
| 复用回收前的图像（`revived`） | 8.0 |
| 跳过上传（`skips`） | 0 |
| 先被采样后上传（`sampled_undefined`） | 0 |
| LUT 复用停放图像（`twin_reuses`） | 4 |
| 仍上传：范围已被 CPU 写过（`miss_defined`） | 0.03 |

v4 同场景的对应数字：跳过 4.7，`sampled_undefined` 2.9，`miss_defined` 0.28，LUT 上传 4。

装机的 v5 包与仓库源码只差一个计数器改名（`expansions`，装机包里显示为 `copies`），复测之后改，Android host 编译通过（`8864d31b`），未重新装机。

测量注意：
- `upload_diag start` 会对每次上传做 XXH3，simpleperf 中占 GpuComm 样本 45%。上传次数多的一侧被额外拖慢，v1–v3 的开/关差值因此偏大。本文所有 A/B 都是关闭诊断后测的。
- `ab.sh` 现在接受 `name=on|off[,…]` 形式的组合开关。
- A/B 结束时开关停在最后一种配置，需要手动恢复。

CPU 写入的来源：v2 日志中的写入全部来自 Guest-57，紧跟在提交之后，位置是目标的第一页，或 B10G11R11 目标之后的 96 KiB（`+0x618000`，像是 CMASK 一类元数据）。这些段因此照常上传。

## 剩余与边界

- **GpuComm 新的最大项**：simpleperf（全开，关诊断，8 s）中 `sched_yield` 占 GpuComm 样本约 20%，主要来自 `SrtGuestReader` 构造（`GetProgram` → `RefreshFlatBuf`）和 `ProtectGpu`，在等 MemoryManager 的锁。很可能是池提交/回收线程持写锁时的等待，未核实。其次是 `GetGraphicsPipeline` 约 20%、`ObtainBuffer` 约 15%。
- **语义差别**：
  - GPU 先写的图像落在新提交、CPU 没写过的池内存上时：开 `pool_revive` 时内容是同一地址上次回收前的 GPU 内容，只开 `pool_undefined` 时内容未定义（可能是显存里的旧数据）。两种情况都不再是 guest 内存的字节。
  - 若有游戏依赖池内存回收、再提交后由 CPU 以外的途径得到的内容，可用开关关闭。
  - 保留的图像最多占 256 MiB 显存。
- **只在 Odin3 KH3 上验证**：
  - 其它游戏、Pocket DS、桌面都没测。代码对桌面通用，但只在 Android 上编译过。
  - “7颗心”画面（v1 的回归点）本轮未复测。
- **同期 GPU 故障**：上午共 4 次，与本改动没有可见关联。
  - fault 73、74、76：#11 典型签名（IB2 页内偏移 `0xe88`、保护写）。
  - fault 75（v2 包，片头，ts 12512）：新签名 `CP BV opcode error | opcode=0x70970001`，在 BV 管线；snapshot 已归档，未分析。

  晚上 v4、v5 各一次启动，都没有故障。
