# 交接：A8XX `CP DDE BR opcode=0` / GPU 读到零（2026-10-18）

完整过程与数据见 [a8xx-dde-opcode-zero-20261017.md](../validation/android-native-host/a8xx-dde-opcode-zero-20261017.md)（第 7.2–7.5 节、第 8 节）。本文只列出接手所需的状态。

## 问题

Odin3（AYN，Adreno 830，gen8.0）上跑 GoW3（CUSA01740）时，偶发 `CP DDE BR opcode error | opcode=0x00000000`，随后 DeviceLost。同一签名在 Swan（Adreno 840）上已由我们推动高通更新 GPU 固件修复；手机固件预计没有修，只能在驱动或模拟器侧规避。

## 已确认的事实

- **位置**：已定位的 12 次 DDE 故障中，10 次在 2/4 MiB `sub_cs` 命令流 BO 的第 1 页读到零，2 次在 128 KiB BO 的起点。逐个核对过的 fault 43/44/45/49–52，都是**本次提交中第一次读该 BO 第 1 页**时出错。典型情形是：组 9（顶点缓冲）在第 0 页，组 11 的包头在第 1 页；组 11 在组 9 之后执行时，读到的包头为零。CPU 侧（KGSL 快照）内容始终正确。
- **不只是 DDE**：fault 53 中 BR 在一个新 IB2 的第一个包上读到零（`pc=0x362`，`$19=0xbad4`），这是本次提交第一次读该 256 KiB BO 的第 0 页；快照里 ROQ 中的数据与内存一致，但 BR 执行时读到的是零。fault 42、46 的 BR 也停在零包头上，地址未定位。
- **持续而非短暂**（fault 54，`TU_DEBUG=dde_check`）：BR 用 `CP_WAIT_REG_MEM`（`WRITE_NE`）轮询第 1 页的组 11 包头 `0x4160167218`，从提交后约 12 ms 起连续约 10 秒都读到零；快照中该地址的值是 `40ab0a02`，最后表现为抢占超时。由此推断是 GPU 对这一页的视图有误（翻译或 GPU 侧缓存），而不是写入时序问题；两者尚未区分。
- **已排除**：CPU 写缓冲未排空（`cs_dsb` 下仍出故障）；SMMU 缺页（KMD 在任何策略下都会发 `kgsl_mmu_pagefault` 并打 `GPU PAGE FAULT`，均未出现）；故障前页表被改写（fault 49、54 所在 2 MiB 区域的 L3 页表在 BO 分配后没有回收或重建）；`cs_recycle` 清零（复用时不清内容）。
- **无效的规避**：`cmd_no_preempt`、`cs_recycle`、mesa `c88b6ebd383`、`cs_dsb`、`dde_touch`（BR 先用 mask 0 读一次该页）。
- **抢占**：fault 51 从提交到故障约 0.41 ms，其间 trace 中没有抢占事件；fault 54 的这次提交与上一次之间做过一次 rb0↔rb2 抢占切换（level 0）。不能把问题归到提交中途的抢占。
- **线索（未证实）**：fault 54 中，GPU 区域 `0x4160000000` 的 L3 页表物理页 `97996e000`，此前 73 秒反复被 USB 控制器（`a600000.dwc3`，即 ADB）的 IOMMU 域用作页表，最后一次释放 83 ms 后成为 GPU 的页表。之后 GPU 正确读了这个 BO 约 83 秒，是否相关未知。

## A/B 汇总（读档进冥界存档点，stream submit 200，看 180 s）

| 组（`debug.mesa.tu.debug`） | 轮数 | 故障 |
|---|---|---|
| `cs_recycle`（对照 C） | 24 | 6 |
| `cs_recycle,cs_skip_head` | 6 | 0 |
| `cs_recycle,dde_touch` | 6 | 2 |
| `cs_recycle,dde_check` | 5 | 2（都变成抢占超时，见上） |
| `cs_recycle,cs_dsb` / `cs_recycle,syncdraw` | 各 4 | 各 1 |

复现率随时间漂移（早期 7 轮 4 次，后期很低），单批 4–6 轮不足以区分各组。

## 代码与分支

- 主仓：`feature/malos/swan_performance`（已合入 `malos/main`），Turnip 引用 `ff5d4093fa5`，Foundation 引用 `1e63c23f`。
- Turnip fork（`tencentmalos/mesa-mirror`）分支 `codex/a8xx-cs-recycle`，基于 `codex/shadps4-xr-turnip` 的 `65b63f66bf5`，全部默认关闭：
  - `bbb3e82bc8d`：cherry-pick 上游 `c88b6ebd383`（bindless descriptor invalidate）；
  - `ce8f89d4e43` `TU_DEBUG=cs_recycle`：保留退役的命令流 BO 复用，不交还 KGSL；
  - `84a5c7ac917` `TU_DEBUG=cs_dsb`：录制完命令缓冲、建完管线后执行 `DSB SY`；
  - `69bf236e9de` `TU_DEBUG=cs_skip_head`（≥64 KiB 的命令流 BO 前 8 KiB 不用）与 `TU_DEBUG=dde_touch`；
  - `ff5d4093fa5` `TU_DEBUG=dde_check`。
  - 这些提交推送前补过 `Generated-by: LLM` 尾注，树与日志里出现的旧哈希 `ca038cc10f1`、`10f23b2bdc0`、`84db376cfde`、`f610504c89c` 相同。
- 主机侧：`upload_diag stream_submit <draws>`（默认 0）会显著提高复现率，用来加速复现。

## 设备状态（Odin3，序列号 `6edb0593`）

APK `94003880`（驱动 `44f215fc…`，含上述全部开关），`debug.mesa.tu.debug=cs_recycle,cs_skip_head`（故障最少的组合；重启后失效），app 停在桌面。设备上的 tracefs 实例与临时文件已清理，KGSL 故障计数为 54。

## 复现与分析

脚本见 [`tools/a8xx-dde`](../../tools/a8xx-dde/README.md)：

- `trial/dde_trial.sh` 跑一轮；
- `analysis/*` 解析快照；
- `wsl/*` 在 WSL 编 Turnip。

需要：

- devices 仓库的 root 桥（`rootctl`）；
- 已安装的 gpu-snapshot MCP；
- GoW3 存档在冥界存档点。

各轮的快照、trace、截图只在原机器的 `build/validation/preempt-ab-20261017/`，没有入库。高通固件、反汇编与内部对比文档按要求不入库。

## 建议的下一步

1. **CP 读值转存**：在 `dde_check` 的 WAIT 之前，用 `CP_MEM_TO_MEM` 把 CP 在故障页几个偏移（页首、目标包头、页内其它处）及相邻页读到的值，写进同一 BO 中能正常读取的页，让快照抓到。整页为零指向翻译问题，单行为零指向缓存行问题。
2. **别名映射**：同一物理页再映射一个 GPU 地址（dma-buf 或 user-mem 导入），出问题时比较两处的读值。
3. **USB 对照**：不接 USB 调试（无线 adb）时统计复现率；在新故障的 trace 中检查故障区域的 L3 页表页之前是否属于 `a600000.dwc3` 域。
4. 若确认是翻译问题：评估每次提交前触发 GPU 上下文 TLB 失效的代价（例如分配并释放一个小 BO），或避免页表页跨 IOMMU 域复用（属于 KMD 层面）。
