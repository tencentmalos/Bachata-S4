# Swan 血源 GPU opcode 故障：固件 PC 映射与 L1 抢占恢复关联（2026-09-30）

本轮只离线分析 09-28～29 的老记录（devcd3/4/9/12/13/14，以及上一轮清单中待补的 devcd15），
没有跑新的游戏会话，也没有修改代码或驱动。

新增的输入是 `C:\workspace\bug_reports` 中的高通 GPU 参考：精确版本 SQE/AQE/GMU 固件的反汇编
与导航 IDB，以及 KGSL 参考源码和 snapshot/firmware 关联 skill。
devcd16–28 是用户为其他分析主动触发抓取的，只拉回并记录了哈希，不在本报告范围内。

上一轮报告：[swan-cp-opcode-20260929.md](swan-cp-opcode-20260929.md)。
本轮证据：[evidence/swan-cp-opcode-20260930/](evidence/swan-cp-opcode-20260930/)。

## 结论摘要

1. **两类故障是同一现象。** 固件 PC 映射显示：
   - `CP opcode error interrupt`（devcd3/4/9/12）是 BR 的 SQE 读到全零包头；
   - `CP DDE BR opcode error`（devcd13/14/15）是 DDE 读到全零包头。
   两者都经分发表第 5 项（非法包类型）进入同一个陷阱（`$19=0xbad4`），写 `RSCC_OVERRIDE` 后原地停机。
   这不是固件卡在 WFI、cache-clean 或 query 的完成等待（所有 pipe 的待完成计数都是 0），
   也不是 Turnip 编码出了非法包（CPU 侧内存在这些位置是合法包头）。
2. **两类都发生在 `CP_SET_DRAW_STATE` 这条链上。**
   - 类型 A：BR 最近处理的包是 `CP_SET_DRAW_STATE` + `CP_DRAW_INDX_OFFSET`。结合上一轮的 ROQ 对照，是跨页的
     `SET_DRAW_STATE` payload 所在那一页读成了 0，随后的包头也是 0。
   - 类型 B：BR 停在 `CP_SET_DRAW_STATE` 处理函数里等 DDE，DDE 在读 draw-state 数据时拿到了 0。
3. **三次有 trace 的 DDE 故障，全部发生在 rb2（shadPS4）从 level=1 抢占恢复后 80–115 µs 内。**
   rb2 被切出时只有约 9.6% 是 level=1，三次恰好都是，偶然概率约 0.1%。这是目前最强的因果线索。
4. **上一轮的 `kgsl_preempt_rb` 候选没有改变抢占粒度。** B2 请求并返回了 style=1，
   但切出 rb2 时仍是 `level=1`（B2 的 trace 中 level=1 共 2172 次）。所以它的失败不能用来否定“抢占恢复”假设。
5. **可以做一个只影响游戏自身的单变量实验。**
   - 这版 Gen8 固件的 SQE 实现了 `CP_SCOPE_CNTL`（A7XX 起复用 opcode 0x6c）的 `DISABLE_PREEMPTION`，
     处理函数确实读写 `@PREEMPT_ENABLE`，不是空实现。
   - Turnip 已经在性能查询中使用它（上游提交 `b8338dee392`）。
   - 用它在游戏 IB 中局部关闭中途抢占，不需要改全局 KGSL 设置（见“下一步”）。

## 身份

| 项 | 值 |
|---|---|
| 设备 / boot | Swan `PB3110PGL6240001G`，boot `20c5024e-15c5-4f6a-981f-7630a933df4c`（与 09-28～29 同一次开机） |
| ROM | `Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys` |
| SQE | 七份 snapshot 的 `SQE_VERSION` 均为 `0x01510103`。对应 bug_reports 归档中 raw SHA `1f1c7bae8050a9910902ec42b3d741ff3a68bc21114dea966a502ade2a9b606d`：反汇编 `qrisc-disasm_20260807`、导航 IDB `gen80200_sqe_01510103_qrisc_navigation.i64`。映射依据是版本号相同加 image-aware PC 映射；没有用案发 `SQE_UCODE_DBG` 字节逐段比对，置信度按 skill 约定降一档 |
| AQE / GMU | `0x01510026` / core 5.3.25、HFI 2.7.1，与归档条目一致 |
| KMD | 案发 `msm_kgsl.ko` Build-ID `37b31e3606d5e5827de3cb5154312f44d6827c95`；bug_reports 的 `references/msm_kgsl.ko` 是 `30a3d849…`，**不匹配**。KGSL 源码语义仍属 `semantic_only_target_build_unverified` |
| 快照 | 哈希见 [sqe-stat-pc.tsv](evidence/swan-cp-opcode-20260930/sqe-stat-pc.tsv)。devcd3/4 在本地 `build/gpu-snapshot/cp-opcode-20260928/`；devcd9/12/13/14/15 本轮从设备拉回，与设备端 SHA 一致 |

固件反汇编属于高通私有内容，本报告只引用路径、SHA 和 PC 号，不转录代码。

## 固件 PC 映射

`CP_SQE_STAT` 的布局沿用 Mesa crashdec：`stat[0]` 是 PC，`stat[i]` 是 GPR `$i`。
BR 映射到 SQE image，DDE_BR 映射到 DDE image，使用 skill 的 `map_qrisc_pc.py`。

完整表：[sqe-stat-pc.tsv](evidence/swan-cp-opcode-20260930/sqe-stat-pc.tsv)。

| 快照 | 类型 | BR PC / `$01` | DDE_BR PC / `$01` | 故障 pipe 的 `$19` |
|---|---|---|---|---|
| devcd3、4、9、12 | CP opcode error interrupt | `0x03cd` / `0x00000000` | `0x01b1`（`waitin`）/ `0x7038800c` | BR `0xbad4` |
| devcd13、14 | CP DDE BR opcode error | `0x03e7`（`CP_SET_DRAW_STATE` 内）/ `0x7043802x` | `0x078f` / `0x00000000` | DDE `0xbad4` |
| devcd15 | CP DDE BR opcode error | `0x17b0` / `0x48823502` | `0x078f` / `0x00000000` | DDE `0xbad4` |

各 PC 的含义：

- **SQE `0x03cd`、DDE `0x078f`**：非法包陷阱。分发表项 `UNKN1/UNKN5/UNKN6/UNKN96` 在跳转的延迟槽里把 `$19`
  设为 `0xbad2`–`0xbad5`，写 `RSCC_OVERRIDE_START_ADDR`（触发 opcode error 中断），然后死循环。
  - DDE image 中，合法但 DDE 不处理的包（`CP_SCOPE_CNTL`、`CP_SET_MARKER` 等）走的是另一项，设 `$19=0xbad1`。
  - `0xbad4` 只由 `UNKN5` 设置。它紧挨 `PKT4`（第 4 项），且 `$01` 即当前包头为 0（type 0，既非 type4 也非 type7）。
    所以这是“读到类型非法的全零包头”。这个判断属于推断：Mesa 的 afuc 模拟器对非法包头直接退出，没有文档说明硬件把它送到哪一项，但 `$01=0` 与 `0xbad4` 的唯一来源相互印证。
- **SQE `0x03e7`**：`CP_SET_DRAW_STATE` 处理函数中轮询控制寄存器 `0x169` bit25 的循环，BR 在等 DDE 可用。
  DDE 已停机，所以这是连带结果，不是首因。
- **最近的包头记录**：类型 A 四份都是 `0x7043802x`（`CP_SET_DRAW_STATE`）和 `0x70380007`（`CP_DRAW_INDX_OFFSET`）。
  这与上一轮 devcd12 的 ROQ 分析一致：`SET_DRAW_STATE` 包头在页尾 `…6ffc`，39 字 payload 全在下一页、全为 0。
- **SQE 控制寄存器组的待完成计数**（`WFI/QUERY/CACHE_CLEAN_PEND_*`）：七份的所有 pipe 都是 0。
  见 [sqe-control-counters.tsv](evidence/swan-cp-opcode-20260930/sqe-control-counters.tsv)。
- **GMU LOG**：故障前的 error state 600 记录里，原始中断位分别是 `0x00010000`（devcd12）和 `0x00200000`（devcd13/14）；
  随后是 error 604 / 632，再发 `F2H_MSG_CONTEXT_BAD`。
  见 [gmu-log-fault-tail.tsv](evidence/swan-cp-opcode-20260930/gmu-log-fault-tail.tsv)。

## 抢占时间线

devcd15 这一段来自上一轮留在设备上、由故障触发冻结的 tracefs instance，本轮收回（trace SHA 与覆盖范围见
[b2-trace-summary.json](evidence/swan-cp-opcode-20260930/b2-trace-summary.json)，摘录见
[devcd15-preempt-before-fault.txt](evidence/swan-cp-opcode-20260930/devcd15-preempt-before-fault.txt)）。
devcd13/14 来自上一轮已提交的 trace 摘录。

| 故障 | rb2 切出 | 离开时长 | 切回 | 切回后到 `adreno_gpu_fault` |
|---|---|---|---|---|
| devcd13（sds_page_align 开） | 99105.715459，rb2→rb0，**level=1** | 4.9 ms | 99105.720388，rb1→rb2 | 80 µs |
| devcd14（默认） | 103751.200642，rb2→rb0，**level=1** | 4.7 ms | 103751.205350，rb1→rb2 | 115 µs |
| devcd15（请求 RB style） | 104470.356602，rb2→rb0，**level=1** | 8.2 ms | 104470.364754，rb1→rb2 | 113 µs |

三次的结构完全相同：rb2 被高优先级的 rb0（XR runtime）以 level=1 抢走，再经 rb1 转回 rb2，约 0.1 ms 后出错。

统计：
- B2 trace 中 rb2 共被切出 22634 次，其中 level=0 占 20456、level=1 占 2172、level=3 占 6。
  这是整份 trace 的计数，部分 CPU 早期有环覆盖。
- 三次故障前的最后一次切出都是 level=1，按约 9.6% 的基率，巧合概率约 0.1%。
- rb2 平均每 10–20 ms 被切一次（trace 早期只有部分 CPU 有记录，计数偏低），故障又都落在切回后 0.12 ms 内，
  单看这一条的偶然概率也很低。

设备上 `preempt_info` 的累计值为 L0 `0x1e823b7`、L1A `0x1a67ee`、L1B `0x84d5a3`，L1 分两类。
trace 中的 `level=1` 对应哪一类尚不清楚。

类型 A（devcd3/4/9/12）没有同窗 trace，不能确认同样发生在抢占恢复之后。
rb2 的抢占记录 `info` 字段在七份中都是 `0x80000011`（devcd15 为 `0x80000091`），其他环是 `0x80000000`。
该字段是固件私有语义，又缺少无故障对照，本轮不据此下结论。

## 已排除或降级的解释

- **被限流吞掉的 SMMU 页故障（读返回 0）。** 设备的 `ft_pagefault_policy=0x0`，即不 GPUHALT：SMMU 故障后事务被 terminate、GPU 继续执行，
  理论上可能表现为“整页读成 0”。但：
  - B2 trace 启用了 `kgsl:kgsl_mmu_pagefault`，故障前约 222 秒的共同保留窗口内一条都没有。
  - 参考 KGSL 源码中，这个 tracepoint 在打印限流判断之前无条件触发。
  - 所以至少对 devcd15，不支持这个解释。前提是目标 KMD 的调用顺序与参考源码相同，这点未核实。
  - 故障后 319 ms 那次 `GPU PAGE FAULT`（前有 `77 callbacks suppressed`）属于恢复阶段。
- **draw-state 表残留的失效地址。**
  - devcd13/14/15：BR `CP_DRAW_STATE` 表中所有已启用组都落在存活分配内；已捕获部分的包头全部合法。
    未捕获的几组在 128 KiB 的管线状态 BO 中。
  - devcd12 确实存在残留：`INPUT_ATTACHMENTS_GMEM` 组指向 MEMLIST 之外的地址；`VS_BINNING`、`GS_BINNING`、`DESC_SETS_LOAD`、`FS_PARAMS`
    指向一块 128 MiB 的非 Turnip 分配。这说明 Turnip 会让已释放 VA 残留在 CP 的 draw-state 表里，但这些多为 binning/GMEM 专用组，
    且 DDE 故障的三份里没有此现象，**与本故障的关联未建立**。
  - 见 [draw-state-table.txt](evidence/swan-cp-opcode-20260930/draw-state-table.txt)。
- **GPU 的 CCHE。** 在 Turnip 中只用于着色器读取，与 CP/DDE 取命令无关。

## 对上一轮实验的重新解读

- `sds_page_align`：devcd13 的故障 IB2 本身不需要填充，而本轮确认它发生在 level=1 抢占恢复之后。
  所以这个候选只针对类型 A 的“跨页”表象，没有触及抢占恢复。
- `kgsl_preempt_rb`：返回的 style=1 并未阻止 rb2 被 level=1 抢占。GMU 是否按 context 的 style 位决定抢占级别，从现有证据看是否定的。
  所以这组对照没有检验“抢占恢复”假设。
- `kgsl_preempt_fg`（未上机）：同理，它只改 context style 请求，预计同样改变不了 level。上机价值低于下面的方案。

## 下一步（需用户确认后再上机）

1. **首选：Turnip 默认关闭的调试开关，用 `CP_SCOPE_CNTL(disable_preemption)` 包住游戏的命令缓冲。**
   - 做法：在每个主命令缓冲 IB 开头发 `disable_preemption=true, scope=INTERRUPTS`，结尾发 `false`。
     这样游戏的 IB 只能在 IB 边界（L0）被抢占。
   - 只能出现在 BR 执行的 IB 中，不能放进 draw-state 组：DDE 对该包走 `0xbad1` 陷阱。
   - 同一 APK、同一驱动下按开关交替跑：开启组多轮无 DDE/SQE opcode 故障、关闭组复现，即可确立因果。
   - 副作用：XR runtime 只能等游戏的 IB 结束才能抢到 GPU，可能出现合成延迟或重投影抖动，需要同时看画面。仅用于诊断。
2. **可选（改全局设置，需用户明确同意）**：临时把 `ft_pagefault_policy` 打开 GPUHALT，让任何 SMMU 故障都停住并留下快照，
   彻底排除“读返回 0 来自页故障”。这是整机设置，会影响其他应用，完成后必须恢复为 `0x0`。
3. 若第 1 步确认因果：
   - 用 bug_reports 的导航 IDB，读这版固件 DDE 的抢占保存/恢复入口（`DDE_new_AQE_preempt_handoff_A/B`，
     “DDE PM4 与高地址抢占状态传递”）；
   - 对照高通 UMD 在 sysmem 直接渲染里如何标记可抢占点、处理 draw-state；
   - 最终方案可能是只在 sysmem render pass 内关闭抢占，而不是整条命令缓冲。

## 现场恢复（上一轮的 CleanupPending）

完整记录见 [cleanup-and-identity.json](evidence/swan-cp-opcode-20260930/cleanup-and-identity.json)。

- 设备在同一次开机中，游戏进程不在。
- 冻结的 trace instance 已收回，然后置 `tracing_on=0` 并 `rmdir`。
- `debug.mesa.tu.debug` 从 `kgsl_preempt_rb` 恢复为空。
  09-28 我方 DumpLayer 留下的 `debug.spruntime.etfr.subsample=0` 也已恢复为空。
- 缓存：
  - 当前活动缓存（B2 加上之后 XR 会话积累的，共 1711 个文件）整体移到 `files/host/cp-preempt-b2-plus-xr-cache-20260930`；
  - 原缓存移回 `files/host/cache`，搬动前后 1264 个文件逐个 SHA 相同。
  - 09-29 的 `before-cache-hashes.txt` 基线在另一台主机上，本轮未与之比对。
- 10 个编码截图临时文件和 1 个 XML：拉回本地、核对 SHA 后从设备删除。
- 存档：基线在另一台主机上，未比对，也没有回滚或外部编辑。
- 设备上的 APK 仍是 XR 会话安装的 `4ec9b9a6…`，本轮未改。未改 KMD、固件、全局抢占设置和 `ft_pagefault_policy`。

## 边界

- 抢占关联是统计与时间上的强相关，还没有经过开关实验证明因果。
- 类型 A 没有同窗 trace，它与抢占的关系未知。
- “`0xbad4` 对应全零包头”是由分发表位置和 `$01=0` 推出的，未查到硬件文档。
- 固件语义基于同版本号的归档反汇编，没有逐段核对案发 `SQE_UCODE_DBG`；KGSL 语义基于非目标构建的参考源码。
- 没有新的游戏运行，GPU hang 未修复。
