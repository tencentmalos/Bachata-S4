# Odin3 KH3 loading GPU hang：A8XX 计算管线寄存器丢失

**状态：已在 Turnip 修复并通过 loading；负对照与固件层面的确认待做。** 完整分析过程见 [odin3-kh3-compute-state-20261011.md](../../docs/validation/android-native-host/odin3-kh3-compute-state-20261011.md)，本目录保存其现场证据。

## 摘要

| 项 | 值 |
|---|---|
| 设备 | AYN Odin3 `6edb0593`，Adreno 830，chip `0x44050000`（gen8.0），系统驱动 `V@0800.72`，KMD Build-ID `8ad43a33afb6affe54edeb6804aa14089dcfe7ce` |
| 游戏 | Kingdom Hearts III `CUSA15072` Ver 1.10 |
| 现象 | 新游戏确认后的 loading 开始约 52 s 必挂：`MISC: GPU hang detected`，status `00EC2405`，随后 DeviceLost（fault 42/43/45/46/47/51 同一位置） |
| 挂死点 | SQE 停在 BR IB1 dword 5270 的 `CP_WAIT_FOR_ME`；未完成的是 dword 5084 的 `CP_EXEC_CS 104×1×1`。该计算管线在 dword 1697 绑定，之后约 23 个 render pass、blit、LRZ 清除，没有重新绑定 |
| 寄存器证据 | 挂死时 SP 的 `SP_CS_CONFIG`、`INSTR_SIZE`、`CNTL_0/1`、`CONST_CONFIG`、`PVT_MEM_SIZE`、`0xa9db` 全为 0，只有 `SP_CS_BASE` 保留；`NDRANGE` 与工作组数是 5084 的值 |
| 判断 | gen8.0 固件在上下文切换时未保留计算管线寄存器（Turnip 自身没有把这些寄存器写 0 的路径）。shadPS4 10-07 起默认跳过重复 pipeline 绑定，正好形成“绑定 → 大量图形工作 → 再 dispatch”的模式 |
| 对照 | `pipeline_cache bind_skip off`（每次 dispatch 前重新绑定）loading 通过 |
| 修复 | Turnip `2d96f4bc287`：A8XX 上若计算状态最后一次发出后又录入了其它命令，dispatch 前重发 CS shader 状态与 load state；`c5a0d6fd990` 加 `TU_DEBUG=nocsreemit` 关闭开关、设备创建时一次性日志与注释；`fd6ba8f106b` 精简 fork 的 `AGENTS.md` |
| 分支 | `tencentmalos/mesa-mirror` 的 `codex/a8xx-compute-state-reemit`，基于 `92c96071dc5`；父仓 `references/mesa-turnip` 引用 `c5a0d6fd990` |
| 验证 | `2d96f4bc287`（APK `52110f66…`）与 `c5a0d6fd990`（驱动 `bfc3af8b…`，APK `6cdd9e54…`）默认配置 loading 通过、进入剧情过场，faultcount 保持 51；`c5a0` 的 logcat 有 `I TU : A8XX: compute state is re-emitted after intervening commands (shadPS4 workaround, TU_DEBUG=nocsreemit disables it)` |

## 待确认

1. **负对照**：`debug.mesa.tu.debug` 设为 `nocsreemit`（与 `gmem_warmup` 并存时由宿主合并），同一流程预期在 loading 恢复挂死，证明是规避本身起作用。
2. **固件**：在 bug_reports 中查 gen8.0 固件上下文切换丢失计算寄存器的记录。用户说明高通驱动约 855.0.9 前后固件一同更新，此类问题大幅减少；需要对应的驱动/固件/SQE 版本号，判断能否按固件版本把规避收窄（目前对所有 A8XX 生效）。8 Gen 5（Swan 等）可用 `nocsreemit` 做 A/B。
3. **合入 fork 主分支**：`codex/shadps4-xr-turnip` 上另有 LRZ 修复 `04e1d665b27`、`fa9873b2cab`，尚未与本分支合并。

## 证据

| 文件 | 说明 |
|---|---|
| `evidence/fault51/snapshot.bin` | KGSL snapshot（root 一次性读取，END 完整，SHA-256 `4db25a0aee867b7c7f75226abf2b7ec55058c87685cdb231e2475a1a5409b791`） |
| `evidence/fault51/dmesg.txt`、`dmesg-kgsl-fault51.txt` | 完整 dmesg 与 fault 51 摘录 |
| `evidence/fault51/ib1-dw0-5400-decoded.txt` | BR IB1 前 5400 dword 解码 |
| `evidence/fault51/ib2-0x4032891b80-decoded.txt` | 下一个计算管线状态 IB2 |
| `evidence/fault51/mvc-br-sp-cs-registers.txt` | 挂死时 SP 的 CS 寄存器 |
| `evidence/fault51/cs-state-ib-0x40055a0a80-raw.txt` | dword 1697 绑定的计算管线状态原始字 |
| `evidence/fault51/live-command-cursors.txt` | MVC 游标 |
| `evidence/fault51/analyze.json`、`snapshot-review.md` | gpu-snapshot 解析结果 |
| `evidence/fault51/logcat-crash.txt`、`android-host-excerpt.log` | 崩溃记录；宿主日志（26 MB）中驱动身份、断言与最后 2000 行 |
| `evidence/fault51/*.txt`（boot_id、date、uptime、faultcount、snapshot-timestamp-*） | 读取身份 |
| `evidence/screens/before-fix-loading-nocb.png` | 修复前 `nocb` 轮进入 loading（随后挂死） |
| `evidence/screens/after-fix-c5a0-cutscene.png` | `c5a0d6fd990` 版本 loading 完成后进入开场过场 |

fault 51 的会话：PID 12286，run UUID `103ecfdd61b4b72982ca062621a37cf5`，`TU_DEBUG=nocb,gmem_warmup`，Turnip `92c96071dc5`，19:31:30 进入 loading，19:32:22 faultcount 50 → 51。
