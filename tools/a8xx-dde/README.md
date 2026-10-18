# A8XX `CP DDE BR opcode=0` 试验与快照分析脚本

用于复现和分析 Odin3（Adreno 830）上 GoW3 的 `CP DDE BR opcode=0` 与相关 GPU 故障。过程与结论见
[a8xx-dde-opcode-zero-20261017.md](../../docs/validation/android-native-host/a8xx-dde-opcode-zero-20261017.md)
第 7.2–7.5 节。脚本来自 2026-10-17～18 的实际试验，默认值针对那台 Odin3（序列号 `6edb0593`）和
Windows 上的 Git Bash，换设备或机器时用环境变量覆盖。

## 目录

| 路径 | 用途 |
|---|---|
| `trial/dde_trial.sh TAG ARM STREAM WATCH_S` | 一轮试验：起独立 tracefs 实例、冷启动 GoW3（按 `ARM` 设 `debug.mesa.tu.debug`）、开 `upload_diag stream_submit STREAM`、读最新存档、观察 `WATCH_S` 秒的 KGSL 故障计数；出故障时冻结 trace、拉取快照与 dmesg。结果追加到 `$DDE_OUT/trials/trials.jsonl`，每轮目录 `$DDE_OUT/trials/TAG/` |
| `trial/dde_ab.sh` | 批量轮换示例（C/H/H × 6）；标签在不同批次之间不能重复 |
| `trial/gow3_ab.py` | `prep ARM`：检查前台、强停、设置并核对 Turnip 属性、记录 logcat 起点；`ARMS` 表定义各组的 `TU_DEBUG` |
| `trial/gow3.py` | 通过 adb 与 DebugBus 操作游戏：`launch`、`status`、`pad MASK [HOLD_MS]`、`shot NAME`、`bus …` 等 |
| `trial/rootsh-odin3.sh SCRIPT` | 把脚本推到设备并以 root 运行，输出结果；root 走 devices 仓库的 `rootctl` 桥 |
| `device/trace_start.sh` | 建 `shadps4dde` tracefs 实例（4 MiB/CPU），开 KGSL、抢占、内存、`arm_smmu` 页表/TLB 事件，`adreno_gpu_fault` 触发 traceoff |
| `device/trace_stop.sh`、`device/fault_collect.sh`、`device/stale_drain.sh`、`device/gpu_state.sh` | 收 trace 并删实例；收 KGSL dmesg 与快照；读掉残留快照（KGSL 只保留第一份未读快照）；记录 GPU 频率/温度/忙碌度 |
| `analysis/snapshot_call.py SNAPSHOT DB OUT_JSON [expert]` | 直接调用已安装的 gpu-snapshot MCP（读 `~/.claude.json` 中的配置），把快照解析成 SQLite |
| `analysis/dde_groups.py DB SNAPSHOT` | 列出 BR 硬件 draw-state 组表两个槽的地址与每个包头 |
| `analysis/dde_page.py DB SNAPSHOT` | 用 DDE 最近包头匹配组表，给出读到零的地址及其页内偏移 |
| `analysis/dde_sds_list.py DB SNAPSHOT IB_ADDR BO_ADDR` | 按顺序列出一个 IB 里每个 `CP_SET_DRAW_STATE` 引用的组、标志及在 BO 中的页 |
| `analysis/dde_sds_pages.py DB SNAPSHOT VB_ADDR…` | 找到引用给定 VB 组地址的 SDS，报告 BO 及此前引用过的页 |
| `analysis/ib_range.py DB SNAPSHOT IB_ADDR FROM TO` | 打印 IB 中一段 dword 范围的包 |
| `analysis/dde_table.py DB SNAPSHOT` | 组表槽位概览（页内偏移、首个包头） |
| `wsl/wsl-turnip-prepare.sh COMMIT`、`wsl/wsl-turnip-build.sh NAME` | 在 WSL 里取 Windows 检出的本地 Turnip 提交（无需推送）、用仓库的 `build-turnip-source` 编译，结果拷到 `build/turnip-wsl-NAME`，供 Windows host 构建用 `SHADPS4_TURNIP_PREBUILT` 绑定。需要 WSL 中的 `/opt/mesa`、NDK 与 meson 环境（见 CLAUDE.md 中 Turnip 构建条目）；完整提交哈希（短哈希 fetch 会失败） |

## 环境变量

- `ANDROID_SERIAL`：设备序列号，默认 `6edb0593`。
- `DDE_OUT`：结果目录，默认仓库下 `build/validation/a8xx-dde`（被 gitignore）。
- `ROOTCTL`：root 桥脚本，默认 `/d/workspace/devices/tools/android-root/rootctl-win`。
- `GOW3_OUT`：`gow3.py`/`gow3_ab.py` 的截图与临时脚本目录；`dde_trial.sh` 会把它设成 `DDE_OUT`。

## 读快照的要点

- DDE `$1a` 是 `$sp`（本批剩余 draw-state 组数），`$13` 是 draw 序号；`CP_SQE_STAT` 第 `0x24..0x26` 字是最近三个包头（新到旧）。BR 的 `$13` 通常比 DDE 大 1。
- `GEN8_CP_DRAW_STATE`（BR）是硬件组表，每组 8 个字，即 A、B 两个槽（地址低/高、大小）。组按组号顺序执行。
- `CP_WAIT_REG_MEM` 不出现在 SQE 包头历史里；BR 的位置用 `cp_execution_positions` 的取指位置减去 `roq_states` 中未消费的 dword 数估算。
- 试验中途停止后，记得在设备上删除 `shadps4dde` 实例和 `/data/local/tmp` 下本组脚本产生的文件，并恢复 `debug.mesa.tu.debug`。
