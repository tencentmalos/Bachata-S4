# GCN v2 阶段 C（第一步）：逐 draw 的排列匹配不再构造特化（2026-10-05）

依据 [gcn-translation-layer-v2-20261003.md](../../specs/gcn-translation-layer-v2-20261003.md) 第 3.2 节与第 5 节阶段 C。回放工具见 [gpu-replay-20261004.md](gpu-replay-20261004.md)，阶段 A 见 [gcn-stage-a-fences-20261004.md](gcn-stage-a-fences-20261004.md)。

环境：Windows，clang-cl RelWithDebInfo，血源 CUSA03023 1.00 + 60 FPS 包。测试目录 `D:\workspace\shadps4-win-test`，每次实机会话先把 `shadps4-win-bb\user\home` 同步过去。基线为已提交的 `919698321`。

**显卡**：上午的会话与回放在 RX 7600M XT（Thunderbolt 外置）上；13:13 起的会话日志只找到 1 个物理设备（Radeon 890M 核显），外置显卡已不在。下文每组数据都注明显卡，两者的帧率与绝对耗时不可比。

---

## 1. 结论

| 项 | 状态 |
|---|---|
| 排列匹配 | `GetProgram` 对每个候选排列不再构造 `StageSpecialization` 再比较，改为 `StageSpecialization::Matches` 边读边比；语义与原 `operator==` 一致（§2） |
| 预检 | binding 起点（`StartDiffers`）或 fetch shader 不同的候选在展开资源表之前就被排除；实机每次查找 1.76 个候选中 0.68 个在预检被排除（§3） |
| fetch shader | 每个 program 缓存自己的 fetch shader 解析（按 guest 代码逐字校验，不加锁、不拷贝），每次查找只取一次 |
| 其他 | 深度范围模拟未启用时只比 `enabled`；`Matches` 不再为 T# 重复取 sharp；buffer 跟踪的 GPU 修改查询跳过无 GPU 页的区域（不拿锁）；纹理绑定缓存的第 0 个元素不再查两次 |
| 正确性 | verify 模式实机四个版本共 1.1 亿次候选比较，快慢两种结果 0 次不一致（§3）；回放逐帧、逐事件图像哈希与同一显卡上的基线一致（§4） |
| 性能 | 同会话栈采样 A/B：`GetProgram` 占 GpuComm 约 12% → 9%，`GetGraphicsPipeline` 约 14% → 11.8%（两块显卡、三个版本结论一致，§5） |
| 未达阶段 C 验收 | spec 目标 `GetGraphicsPipeline` 低于 3%；逐 draw 的 user data 几乎每次都变，按变化跳过不适用，剩余开销与原因见 §6 |
| 回放差异 | 上午外置显卡上新 exe 的第一次回放出现过一次 G-buffer 单个 MRT 一帧不同（与阶段 A 那次同类，未定位）；下午核显上的回放与外置显卡基线差 33 张纹理的创建，原因是换了显卡，与代码无关（§4） |

---

## 2. 改动

- **`StageSpecialization::Matches`**（`shader_recompiler/specialization.h`）：给定当前 user data 与已展开的 flat buffer，判断某个已有排列是否等于此刻会构造出的特化，不构造。比较顺序、跳过规则与 `operator==`（this 为已存排列、other 为新构造）相同：
  - 先比资源数量、动态 T# 槽位掩码、顶点属性（步进率、数值类别、`dst_select`）、运行时信息（曲面细分阶段带常量缓冲修正）、FMask；
  - 都没有有效 buffer/image/FMask 时只比 user data 起点（或该阶段不读 user data）；
  - 否则比完整的 binding 起点，buffer 与 image 只在当前 sharp 有效处比较，sampler 全部比较。
  - 前提：调用方已确认 fetch shader 与该排列的完全相同（`CompareFetchShader` 返回 `Same`）。fetch shader 在 `operator==` 中按 `FetchShaderData::operator==` 比较，它不看属性的指令偏移与格式覆盖；只有这些字段不同（`Equal`）时，仍构造特化来判断。
- **`StageSpecialization::StartDiffers`**：binding 起点的比较规则同上（有有效资源时比完整起点，否则只在该阶段读 user data 时比 user data 起点）。起点由前面已处理阶段的资源数累加而来，共用 VS 的 program 对每种 PS 布局各有一个排列；`GetProgram` 先用它排除，不展开资源表。
- **fetch shader**（`frontend/fetch_shader.*`）：`CompareFetchShader(parsed, expected)` 区分 `Different`/`Same`/`Equal`；`FetchShaderCache` 为单一所有者的解析缓存（4 项，按代码地址，guest 代码逐字相同才复用），`Program::fetch_shaders` 持有一份。`GetProgram` 每次查找最多取一次（按 `has_fetch_shader` 与 SGPR 基址），在 `RefreshFlatBuf` 之前比较，不同就跳过该候选。
- **`DepthRangeEmulation::operator==`**：未启用时 `BuildDepthRangeEmulation` 返回默认值，比较只看 `enabled`；启用时与原来相同。
- **`ImageResource::NumBindingsFor(tsharp)`**：`Matches` 用已取到的 T# 计算 binding 数。
- **`RegionManager::MayHaveGpuPages`**（`buffer_cache/region_manager.h`）：GPU 修改位每次变化都在区域锁内同步一个原子标志；`MemoryTracker::IsRegionGpuModified` 遇到没有 GPU 修改页的区域直接返回，不拿锁。实机 64% 的查询走这条路（统计计数器只在测量版本中存在，已去掉）。DebugBus `upload_diag gpu_flag on|off`，默认 on。
- **`Rasterizer::BindTextures`**：第 0 个元素沿用入口处的哈希查找结果。
- **DebugBus `pipeline_cache spec_match fast|full|verify`**，默认 fast：
  - `full`：原来的方式（每个候选构造特化再比较）；
  - `verify`：两种都算（预检排除的候选也算），不一致时计数并打前 16 条 Error；同时统计每次查找比较了几个候选、几个被预检排除。
  - `pipeline_cache status` 增加 `permutation match` 与（verify 模式下的）`permutation lookups` 两行。

---

## 3. verify 模式实机

从第一帧起 `pipeline_cache spec_match verify`，进世界、出生点稳定后再留 20–30 s。

| exe | 显卡 | 候选比较 | 不一致 | 查找次数 | 每次查找比较运行时信息 / 资源 | 其中起点不同 | 新建排列 |
|---|---|---|---|---|---|---|---|
| `d6b07225` | 外置 | 30,399,529 | 0 | — | — | — | — |
| `1073babe` | 外置 | 31,967,551 | 0 | 18,215,128 | 1.79 / 1.76 | — | 243 |
| `7760a3e2` | 外置 | 26,821,271 | 0 | 15,490,135 | 1.76 / 1.73 | — | 313 |
| `82ed8005` | 核显 | 25,260,881 | 0 | 14,349,357 | 1.79 / 1.76 | 0.68 | 260 |

- 各版本 fetch shader 不同的候选都是每次查找 0.00 个：同一 program 的排列在血源中由 binding 起点区分，所以加了起点预检（`82ed8005`）。
- `82ed8005` 整个会话（含随后的 A/B）共 25,636,369 次比较，0 次不一致；各版本的日志中都没有 `Permutation match disagrees`，0 次崩溃。

---

## 4. 回放

`bb-final`（世界 5 帧，1875 个事件）。比较 `frames.txt` 与逐事件 `image_hashes.txt`。回放强制 sync 编译。

**外置显卡**（基线 `bb-final_lm1`，2210 行）：

| exe | 次数 | 全同 |
|---|---|---|
| `d6b07225` | 2 | 2 |
| `1073babe` | 2 | 1 |
| `9ccd3745` | 1 | 1 |

`1073babe` 的第一次回放（也是该 exe 的第一次运行）第 0 帧不同：从事件 155（该帧第一次写 G-buffer）起，image 21（`0x26e890000`，960×540 R8G8B8A8Unorm，G-buffer 的 MRT1）41 行哈希不同，同一提交写的 MRT0、MRT2、MRT3 与深度都相同，后续帧全同。两次回放的日志除帧哈希外逐行相同；匹配逻辑是确定性的，第二次回放全同，因此不是本改动的结果。阶段 A 那次（第 2 帧 image 22，即同一 G-buffer 的 MRT2）同样是新 exe 的第一次回放。原因未定位。

**核显**：与外置显卡的基线相比，`frames.txt` 全同，`image_hashes.txt` 少 33 行：外置显卡在事件 428–533 创建并上传的 33 张纹理（BC1/BC4/BC6H/BC7 等）在核显上没有创建，之后的 image 编号相应前移；其余每个事件写出的图像哈希全同。上午在外置显卡上与基线全同的旧 exe `9ccd3745`，下午在核显上回放得到的也是这 2177 行，所以差别来自显卡，与代码无关；纹理为什么只在外置显卡上创建未查（对画面无影响）。核显上以 `bb-final_sk1` 为基线：

| exe | 次数 | 与 sk1 全同 | 说明 |
|---|---|---|---|
| `82ed8005` | 5 | 4 + sk1 自身 | 其中 2 次开流式页（见阶段 D 记录），1 次为新拷贝的 exe 第一次运行 |
| `bc37e63f` | 4 | 4 | 其中 2 次开流式页 |
| `c9993608` | 1 | 1 | 归因变体：等待处不刷新动态图像表 |
| `3d13481a` | 2 | 2 | 回放按写缺页通知缓存（阶段 D），其中 1 次开流式页 |
| `9ccd3745` | 1 | 1 | 旧 exe |

核显上这 13 次回放（含每个 exe 的第一次运行）没有再出现外置显卡上那种单个 MRT 不同。

---

## 5. 性能（同一会话栈采样 A/B）

“新”= `spec_match fast` + `gpu_flag on`，“旧”= `spec_match full` + `gpu_flag off`。每轮切换后等 3 s，测 8 s 帧率与 6 s 线程 CPU，再对 GpuComm 栈采样 20 s。表中为各函数（含子调用）占 GpuComm 运行样本的比例。

`1073babe`，外置显卡，出生点静止，60 FPS 上限，约 740 draws/帧：

| 轮 | 模式 | 运行样本 | `GetGraphicsPipeline` | `GetProgram` | 特化构造+比较 / `Matches` | `IsRegionGpuModified` |
|---|---|---|---|---|---|---|
| 1 | 新 | 6115 | 13.08% | 9.98% | 4.53% | 3.30% |
| 2 | 旧 | 6400 | 15.00% | 13.14% | 8.78% | 3.42% |
| 3 | 新 | 6481 | 12.44% | 9.38% | 4.20% | 2.41% |
| 4 | 旧 | 7018 | 15.29% | 12.81% | 8.66% | 2.29% |

这一版为统计 GPU 修改查询的跳过比例加了两个全局原子计数器，抵消了跳过省下的锁，所以 `IsRegionGpuModified` 没有变化；计数器在后一版去掉。

`7760a3e2`（fetch shader 改为 program 级缓存、去掉计数器），外置显卡，存档位置已变，约 1130–1160 draws/帧、31–46 FPS（不封顶），一对：

| 模式 | 运行样本 | `GetGraphicsPipeline` | `GetProgram` | 特化构造+比较 / `Matches` | fetch shader 解析 | `IsRegionGpuModified` |
|---|---|---|---|---|---|---|
| 新 | 5831 | 10.75% | 8.25% | 3.62% | 0.15% | 1.65% |
| 旧 | 5735 | 13.93% | 11.58% | 7.79% | 1.05% | 2.84% |

`82ed8005`（加起点预检），核显，约 722 draws/帧、39–46 FPS：

| 轮 | 模式 | 运行样本 | `GetGraphicsPipeline` | `GetProgram` | 特化构造+比较 / `Matches` | fetch shader 解析 | `RefreshFlatBuf` | `IsRegionGpuModified` | GpuComm CPU |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 新 | 4902 | 11.69% | 9.08% | 3.61% | 0.06% | 0.45% | 1.37% | 12.51 ms/帧 |
| 2 | 旧 | 4602 | 14.02% | 11.91% | 7.61% | 1.52% | 1.04% | 2.33% | 12.80 ms/帧 |
| 3 | 新 | 4746 | 11.88% | 8.72% | 3.14% | 0.08% | 0.48% | 1.71% | 12.35 ms/帧 |
| 4 | 旧 | 4854 | 14.42% | 12.05% | 8.59% | 1.17% | 0.82% | 2.55% | 13.35 ms/帧 |

- `GetProgram` 相对 `Rasterizer::Draw` 的比例：新 15.3% / 14.3%，旧 19.9% / 19.7%。
- 起点预检让 `Matches` 与 `RefreshFlatBuf` 的调用减少约 39%（每次查找 1.76 → 1.08 个候选进入展开），`Matches` 3.6% → 3.1–3.6%，`RefreshFlatBuf` 约减半。
- 线程 CPU 噪声大于差别（同为“新”的两轮帧率差 4.5 FPS），以采样比例为准。

---

## 6. 与阶段 C 验收的距离

改动后 `GetGraphicsPipeline` 约占 GpuComm 的 11–12%（核显、外置显卡相近），其中 `GetProgram` 约 9%：

| 部分 | 占 GpuComm | 说明 |
|---|---|---|
| `Matches` | 约 3.1–3.6% | 读 sharp 并推导与代码生成相关的字段；buffer/image/sampler 的 `GetSharp` 都是外联调用 |
| 运行时信息比较 | 约 0.8–0.9% | 每次查找约 1.8 次 |
| `RefreshFlatBuf` | 约 0.5% | 预检之后每次查找约 1.1 次 |
| 其余 | 约 4% | `GetProgram` 自身、管线 key 计算（`RefreshGraphicsKey`）与管线表查找 |

剩下的工作每个 draw、每个阶段都要做一次：逐 draw 的 user data 几乎每次都变（此前统计：VS user data 与上一个 draw 相同的只占 0.7%），按“输入没变就跳过”的思路不适用。要降到 3% 以下，需要 spec 第 3.2–3.3 节的组合：

- 寄存器脏位图与管线 key 增量维护；
- 按代际门控 SRT 走表（阶段 D），flat buffer 与 sharp 未变时不重读；
- 特化只依赖 sharp 中与代码生成相关的位：把 buffer 格式移出特化 key（阶段 D 的 UBO 与 texel buffer）之后，每个 draw 要比较的字段会大幅减少。

---

## 7. 阶段 D

同一采样里 `ObtainBuffer` 约占 GpuComm 28–30%，其中写保护的设置（`RegionManager::UpdateProtection<1,0>`，绝大部分在 `NtProtectVirtualMemory`）约 6–8%：每帧被 CPU 重写的页上传后又被写保护，下一次写入再触发缺页。阶段 D 的流式页（spec 3.5.3）记录见 [gcn-stage-d-stream-pages-20261005.md](gcn-stage-d-stream-pages-20261005.md)。
