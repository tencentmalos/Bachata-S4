# 交接：A8XX `CP DDE BR opcode=0` / GPU 读到零（2026-10-18，10-19 更新）

完整过程与数据见 [a8xx-dde-opcode-zero-20261017.md](../validation/android-native-host/a8xx-dde-opcode-zero-20261017.md)：10-17～18 的过程在第 7–8 节，10-19 的定位在第 9 节（结论汇总 9.7），脱离 shadPS4 的最小复现在第 10 节。汇总报告（可直接对外说明）见 [a8xx-cp-zero-read-report-20261019.md](../validation/android-native-host/a8xx-cp-zero-read-report-20261019.md)。本文只列出接手所需的状态。

## 问题

Odin3（AYN，Adreno 830，gen8.0）上跑 GoW3（CUSA01740）时，偶发 `CP DDE BR opcode error | opcode=0x00000000`，随后 DeviceLost。KH3 的 #11（片头保护写、CP 从命令流 BO 某页起读到全零）和 `CP BV opcode error | opcode=0x70970001` 是同一个问题。同一签名在 Swan（Adreno 840）上已由我们推动高通更新 GPU 固件修复；手机固件预计没有修，只能在驱动或模拟器侧规避。

## 结论（10-19）

- **触发条件**：只读 BO 的第 0 页位于 64 KiB GPU VA 块的槽 6（第 1 页在槽 7），且同一 64 KiB 块低位的 BO 已被释放（解除映射）。CP 读槽 7 得到全零，不报缺页，CPU 侧内容正确。
- **证据**：已定位的历史零读（GoW3 14 次、KH3 4 次）全部在槽 6/7，而只读 BO 落在这个相位的比例只有 3–13%。用 Turnip 诊断 `TU_CS_HAZARD=<槽位>[:keep]` 刻意制造布局：

  | 实际布局 | 结果 |
  |---|---|
  | 槽 6，同块低位已释放 | 9/9 出错，多数在读档后 25 s 内 |
  | 槽 6，下方垫片保留 | 0/3 |
  | 槽 14，下方已释放 | 0/4 |
  | 槽 3，下方已释放 | 0/3 |
  | 不放置 | 0/4 |

- **读路径**：DDE 取 draw-state 组、BV 取 IB1、BR 取 IB2 都出现过零读。
- **规避**：只读 BO 用 64 KiB 对齐 VA。自然状态 GoW3 `csalign64k` 0/12（同批对照 1/6），KH3 8/8（对照 6 轮 5 次）。
- **KGSL 侧事实**（工作站 ROM 源码）：释放 BO 时 KGSL 对 GFX 用户、LPAC、GMU 三个域做 `iommu_flush_iotlb_all`，映射时不做 TLB 失效；非 SVM 区的 VA 按 `va_hint` 递增分配（释放最高的那块时 hint 退回到前一块末尾）；VA 对齐只支持 4K/64K/1M/2M；GPU SMMU 的 `qcom,actlr = 0x32B`（“+15 deep PF”），Odin3（sun）与 Swan（canoe）相同。
- **未确认**：机理（SMMU 深预取/部分解除映射后的 64 KiB 组翻译，还是 KGSL 解除映射流程）需高通确认；为什么只在块的前半出错；非 CP 读是否受影响。

## 最小复现（10-19 下午，第 10 节）

- `tools/a8xx-dde/minicase/kgsl_slot_repro.c` 只用 KGSL ioctl 构造“槽 6 + 同块低位已释放”的布局并让 CP 读它，adb shell 用户即可运行。
- 只有 CP 读 T（IB1/IB2 取指、`CP_MEM_TO_MEM`），再加并发分配释放、TLB 压力、跨进程高优先级提交，数百万次提交都不复现。
- 加上一个零页映射的 KGSL VBO（与 Turnip 的 sparse residency、shadPS4 的缓冲区 arena 相同）并让 CP 读它（落在 PRR 零页上）后复现：槽 7 整页读到零、槽 6 正确；IB1 读法下变成 GPU hang，SQE `$19=0xbad4`（与游戏同一陷阱）。逐项剥离后，第二个进程、TLB 压力、VBO 绑定活动、并发分配释放都不是必要条件，VBO 读是必要条件。
- 对照（停掉 xrgamenative 后，快速配置每组 60 s）：槽 0–15 扫描只有槽 6、槽 7 出现零读，读到零的永远是块内槽 7；垫片保留、只释放槽 5、64K 对齐、不放置、只从槽 7 开始读（槽 6 映射但不读）都不出现；VBO 读落在普通页（`--vbo-fill`）不出现，64 MiB 零页 VBO、每次提交只读 1 次都出现。
- `--t-rw`：零读期间写入槽 7 的标记被丢弃（2151 次，全部与零读同一提交），槽 6 不受影响，与“槽 7 的翻译落到 PRR 零页”一致。
- Swan（Adreno 840，当前固件 SQE `0x01510103`，Steam 在前台运行、无外部故障）上同一套测试结果完全相同：只有槽 6/7 布局出错、零在槽 7、写同时丢失；“不放置”组有一个 T 自然落在槽 6 也出错。之前“Swan 已由固件修复”的结论没有覆盖这个机制。
- 第一批时后台 xrgamenative 的 `steamwebhelper` 出了几次 GPU 故障（fault 12–14，含 BV `0x70970001` 与 DDE opcode=0 签名）；停掉它后 30 轮无任何故障。是否与复现程序有关尚未确定。

## 早期已排除或无效的方向（10-17～18）

- 已排除：CPU 写缓冲未排空（`cs_dsb` 下仍出故障）；SMMU 缺页（KMD 任何策略都会发 `kgsl_mmu_pagefault` 并打 `GPU PAGE FAULT`，均未出现）；故障前页表被改写；`cs_recycle` 清零；HTW LLC（KH3，`gpuhtw_llc_slice_enable=0` 仍出现）。
- 无效的规避：`cmd_no_preempt`、`cs_recycle`、mesa `c88b6ebd383`、`cs_dsb`、`dde_touch`。
- 不能把问题归到提交中途的抢占（fault 51 期间无抢占事件）。
- 第 7.5 节末尾提出的 CP 读值转存、别名映射、不接 USB 的对照没有做，已被布局实验部分取代；别名映射仍可用来区分“该 VA 的翻译出错”与“物理内存过期”。

## 代码与分支

- 主仓：`feature/malos/swan_performance`，已对齐远端 `ce8111f2f`，本地改动未提交（合并前的 stash `pre-sync-1019 worktree` 仍保留）。Foundation 引用 `d3f06642`。
- Turnip 本地分支 `codex/a8xx-dde-merge`（未推送，父仓引用仍是 `ff5d4093fa5`）：
  - `d60b40d2709`：合并 `ff5d4093fa5`（`cs_recycle`、`cs_dsb`、`cs_skip_head`、`dde_touch`、`dde_check`）与 KH3 诊断分支 `d39dca430ca`（`cmdaudit`、`csbocache`、`TU_DRAW_CS_PAD_DW`、`ib2pagesplit`、`csalign64k`、`lrznoreuse`）；
  - `9fa8f70827a`：`TU_DEBUG=roalign64k`（所有 `GPU_READ_ONLY` 分配 64K 对齐，含 pipeline 子分配块）；
  - `a0a119410a1`：`TU_CS_HAZARD=<槽位>`；
  - `68dafb89a26`：`TU_CS_HAZARD=<槽位>:keep`。
  - 以上开关全部默认关闭；经 `debug.mesa.tu.debug` / `debug.shadps4.tu_env` 导出。
- fork（`tencentmalos/mesa-mirror`）上已推送的是 `codex/a8xx-cs-recycle`（`ff5d4093fa5`）。推送 fork 时需补 `Generated-by: LLM` 尾注。
- 主机侧：`upload_diag stream_submit <draws>`（默认 0）显著提高复现率。

## 设备状态（Odin3，序列号 `6edb0593`，10-19 14:10）

APK `4171cbbe…`（Turnip `68dafb89a26`，新开关默认关闭）；`debug.mesa.tu.debug`、`debug.shadps4.tu_env` 已清空；GoW3 逐游戏配置为用户设定的倍率 0.75（实验中临时改 1.0 后已逐字节恢复）；tracefs 实例与临时文件已清理。10-19 晚经用户同意停掉了 xrgamenative（Steam 客户端），未重新启动；最小复现留在 `/data/local/tmp/a8xxmini/`，KGSL 故障计数 14。

## 复现与分析

- 游戏内：[`tools/a8xx-dde`](../../tools/a8xx-dde/README.md) 的 `trial/dde_trial.sh` 跑一轮，`trial/gow3_ab.py` 定义各组（K/L/P/Q/N 为布局实验组）；`analysis/*` 解析快照，`analysis/trace_neighbors.py` 在 trace 中找出错页所在 BO 及其上下邻居。需要 devices 仓库的 root 桥（本机 `ROOTCTL=/c/workspace/devices/tools/android-root/rootctl-win`）、已安装的 gpu-snapshot MCP、GoW3 存档在冥界存档点。
- 最小复现：`tools/a8xx-dde/minicase/`（第 10 节），只用 KGSL ioctl，不需要游戏、Mesa 或 root（收快照才需要 root）。最小配置 `kgsl_slot_repro --mode m2m --churn-us 0 --vbo 2048 --vbo-reads 64 --life-ms 3000`；`m2m` 读法下零读是静默的，不会让 GPU 出故障。
- 各轮快照、trace、截图在原机器的 `build/validation/a8xx-dde/` 与 `build/validation/preempt-ab-20261017/`，没有入库。高通固件、反汇编与内部对比文档按要求不入库。

## 建议的下一步

1. 把最小复现（`tools/a8xx-dde/minicase`，第 10 节）连同游戏数据交给高通，注明 Odin3（SQE `0x01500081`）与 Swan（SQE `0x01510103`）都复现；可补 IB1/DDE 读法对照。
2. 评估第二种规避：sparse VBO 不映射零页（`KGSL_MEMFLAGS_VBO_NO_MAP_ZERO`）或保证 CP 不读未绑定区，需要先确认对 sparse residency 语义的影响，再在游戏中 A/B。
3. GoW3 上跑 `roalign64k` 对照；无故障则改成 A8XX 默认开启，推送 Turnip 并推进父仓引用（需用户确认）。
4. 非 CP 读（着色器取数）是否受影响：可在最小复现之外用 Vulkan 计算着色器读同样布局的缓冲（`minicase/*.comp` 是这一方向的草稿，未接入）。
