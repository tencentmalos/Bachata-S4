# Odin3 KH3 loading GPU hang：A8XX 计算管线寄存器丢失

**状态：已在 Turnip 修复并通过 loading。10-12 在 Odin3 上做了对照：关闭重发即复现；`skipsaverestore=0` 不能避免；关闭中途抢占（`preempt_level=0`）后不重发也通过。原因确定为 L1（中途）抢占在 bin 边界切出后，SP 的计算程序寄存器不被恢复。** 完整分析过程见 [odin3-kh3-compute-state-20261011.md](../../docs/validation/android-native-host/odin3-kh3-compute-state-20261011.md)，本目录保存其现场证据。

## 摘要

| 项 | 值 |
|---|---|
| 设备 | AYN Odin3 `6edb0593`，Adreno 830，chip `0x44050000`（gen8.0），系统驱动 `V@0800.72`，KMD Build-ID `8ad43a33afb6affe54edeb6804aa14089dcfe7ce` |
| 游戏 | Kingdom Hearts III `CUSA15072` Ver 1.10 |
| 现象 | 新游戏确认后的 loading 开始约 52 s 必挂：`MISC: GPU hang detected`，status `00EC2405`，随后 DeviceLost（fault 42/43/45/46/47/51 同一位置） |
| 挂死点 | SQE 停在 BR IB1 dword 5270 的 `CP_WAIT_FOR_ME`；未完成的是 dword 5084 的 `CP_EXEC_CS 104×1×1`。该计算管线在 dword 1697 绑定，之后约 23 个 render pass、blit、LRZ 清除，没有重新绑定 |
| 寄存器证据 | 挂死时 SP 的 `SP_CS_CONFIG`、`INSTR_SIZE`、`CNTL_0/1`、`CONST_CONFIG`、`PVT_MEM_SIZE`、`0xa9db` 全为 0，只有 `SP_CS_BASE` 保留；`NDRANGE` 与工作组数是 5084 的值 |
| 判断 | RB 在 GMEM pass 的 bin 边界被 L1 抢占切出，切回后 SP 的计算程序寄存器为 0（Turnip 自身没有把这些寄存器写 0 的路径；`skipsaverestore` 的取值不影响）。shadPS4 10-07 起默认跳过重复 pipeline 绑定，正好形成“绑定 → GMEM pass → 再 dispatch”的模式 |
| 对照 | `pipeline_cache bind_skip off`（每次 dispatch 前重新绑定）loading 通过；10-12 的 `nocsreemit` 三组对照见“对照实验（2026-10-12）” |
| 修复 | Turnip `2d96f4bc287`：A8XX 上若计算状态最后一次发出后又录入了其它命令，dispatch 前重发 CS shader 状态与 load state；`c5a0d6fd990` 加 `TU_DEBUG=nocsreemit` 关闭开关、设备创建时一次性日志与注释；`fd6ba8f106b` 精简 fork 的 `AGENTS.md`。10-12 `1efc1d3f5c9`：改为在缓存刷新和间接参数复制（两者都含 WFI）之后、紧挨常量/描述符加载与 `CP_EXEC_CS` 重发，A8XX 每次 dispatch 都发，只在上次发出后命令流里一个包都没有时跳过（见“对修复的影响”） |
| 分支 | `tencentmalos/mesa-mirror` 的 `codex/a8xx-compute-state-reemit`，基于 `92c96071dc5`；父仓 `references/mesa-turnip` 引用 `c5a0d6fd990`。`1efc1d3f5c9` 目前只在本机子仓提交，未推送，父仓引用未改 |
| 验证 | `2d96f4bc287`（APK `52110f66…`）与 `c5a0d6fd990`（驱动 `bfc3af8b…`，APK `6cdd9e54…`）默认配置 loading 通过、进入剧情过场，faultcount 保持 51；`c5a0` 的 logcat 有 `I TU : A8XX: compute state is re-emitted after intervening commands (shadPS4 workaround, TU_DEBUG=nocsreemit disables it)`。10-12 `1efc1d3f5c9`（驱动 `127cbf2a…`，宿主 `e6c7a1b6…`，APK `6fd84310…`，主干 `2821fb4d6` 构建）：默认抢占设置、`TU_DEBUG` 为空，loading 15:24:40 开始，约 2 分 20 秒内出现“開始遊戲”，6 分钟内 faultcount 不变（68），进入开场影片（约 56 FPS）后正常停止；logcat 有 `A8XX: compute state is re-emitted right before every dispatch`。同一包的第一次启动在片头出了一次 #11 保护写（fault 68，与本问题无关） |

## 待确认

1. ~~**负对照**~~：10-12 已完成，`nocsreemit` 下 loading 复现挂死（fault 62），见“对照实验”。
2. **固件**：10-12 已查（见文末）。bug_reports 没有 gen8.0 丢失计算寄存器的记录；但 Swan（gen8.2，SQE `0x01510103`）10-08 的 ASTRO snapshot 也出现同样的清零，所以规避**不应收窄到 gen8.0**，也不能指望换固件解决。
3. **合入 fork 主分支**：`codex/shadps4-xr-turnip` 上另有 LRZ 修复 `04e1d665b27`、`fa9873b2cab`，尚未与本分支合并。
4. ~~**重发位置**~~：10-12 已在 `1efc1d3f5c9` 实现并在 Odin3 默认设置下过 loading，见“对修复的影响”。待做：推送到 fork 并更新父仓引用（需用户确认），以及上面第 3 项的合并。

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

| 文件 | 说明 |
|---|---|
| `tools/cs_state_survey.py` | 只读 snapshot：找当前 BR IB1 中最后一次写 `SP_CS_CONFIG` 的位置、其后的 GMEM pass / A2D blit / dispatch、同一 IB1 的中途切出记录，并给出故障时 MVC 中的 CS 寄存器值。最后一次写入离取指位置不足 600 dword 时（可能还没到达 SP），另报更早的一次（与 gpu-snapshot MCP 的寄存器来源判定一致）；IB 同时从 GPU_OBJECT_V2 和 IB_V2 段查找 |
| `analysis-20261012/cs_state_survey.txt` | 上述脚本对 Odin3 fault 51/52/53、10-12 的 fault 60–66 与 Swan 历史 snapshot 的输出（Swan snapshot 与 fault 60/61/64–66 不在仓库，本机位于 `build/gpu-snapshot/cp-opcode-20260928/`、`build/validation/swan-gpu-hang-20260930/raw/snapshots/`、`build/validation/astro-swan-20261008/gpu-snapshots/`、`build/validation/odin3-kh3-preempt-20261012/`） |
| `evidence/fault62-nocsreemit/` | 对照 A（只开 `nocsreemit`）的 loading 挂死：snapshot（SHA-256 `f305283d…7daf5`）、读取记录、KGSL dmesg |
| `evidence/fault63-nocsreemit-noskip/` | 对照 B（再加 `skipsaverestore=0`）的 loading 挂死：snapshot（SHA-256 `21c87372…bba08`）、读取记录、KGSL dmesg |
| `evidence/fault64-level0-opening/` | 对照 C（`preempt_level=0`）中的一次片头保护写（KH3 #11，与本问题无关）：读取记录、KGSL dmesg；snapshot（SHA-256 `11a7e8b3…6fa`）留在本机 build 目录 |

三份 `acquisition.txt` 里的 `local_validation_rc=2` 是读取脚本在 Windows 上把 MSYS 路径交给 Python 校验器失败（`C:\c\workspace\...`），不是文件损坏；文件已人工核对（magic `0x504d0002`、有 END 段），gpu-snapshot MCP 解析完整。

## 寄存器是怎么丢的：L1 抢占（2026-10-12）

结论：计算管线寄存器是在 RB 被**中途切出（L1 抢占）后切回**时丢失的，不是 render pass、GMEM pass 或 A2D blit 本身清掉的。gen8.0（Odin3）和 gen8.2（Swan）都会发生。Turnip 的计算状态只在绑定时直接写进命令流，既不在 draw-state 组里，也不在 bin preamble 里，切回后没有任何机制恢复它。10-12 的对照实验确认了因果：关闭中途抢占后不重发也不再挂死；而让 CP 做保存/恢复（`skipsaverestore=0`）仍然挂死。

### 对照实验（2026-10-12）

设备 Odin3 `6edb0593`，驱动为 `c5a0d6fd990`，`debug.mesa.tu.debug=nocsreemit`（宿主再并入 `gmem_warmup`）。抢占参数经 root 写 KGSL debugfs `preemption/{preempt_level,skipsaverestore}`，每组都从 snapshot 的 MVC 读回 `CP_CONTEXT_SWITCH_CNTL`（0x8c0，bit 6–7 = level，bit 8 = usesgmem，bit 9 = skipsaverestore）确认硬件实际取值。流程同上：新游戏 → 确认 → loading。

| 组 | 设置 | 0x8c0 实读 | 结果 |
|---|---|---|---|
| A | 默认：level 1、skipsaverestore 1 | `0x340` | loading 中挂死（fault 62，`00EC2405`，宿主 14:23:46 报 DeviceLost；本轮未记 loading 开始时间）。此前两次启动在片头出保护写（fault 60/61，见下） |
| B | level 1、**skipsaverestore 0** | `0x140` | loading 14:27:05 开始，约 70 秒后挂死（fault 63，`00EC2405`） |
| C | **level 0**（只在提交边界切换）、skipsaverestore 1 | `0x300`（来自 fault 64） | 前三次启动在片头出保护写（fault 64–66），第四次进入 loading：14:37:56 开始，约 3 分 40 秒后出现“開始遊戲”，按键进入开场影片与室内场景，5 分钟内 faultcount 不变（66）；随后正常停止（`user_stop`） |

两次挂死的现场与 fault 51 是同一结构（`cs_state_survey.py` 与 gpu-snapshot MCP `preemption_switch_outs`、`gpu_snapshot_registers` 一致）：

| | fault 51 / 62 | fault 63 |
|---|---|---|
| 计算管线绑定（`SP_CS_CONFIG=0x105`） | IB1 dword 1697 | 2127 |
| 同 IB1 的 dispatch（7168×1，正常完成） | 1728 | 2158 |
| GMEM pass（`BIN_RENDER_START` 带 USES_GMEM → `BIN_RENDER_END`） | 1979 → 2075 | 2409 → 2505 |
| BR 中途切出（info `0x80000007`，bin 边界） | **2077** | **2507** |
| 挂死的 dispatch（104×1） | 5084 | 5573 |
| 故障时 SP | CONFIG/INSTR_SIZE/CNTL/CONST_CONFIG 等为 0，BASE 保留绑定值 | 同左（BASE `0x55b4c80` = 2127 写入值） |

- fault 62 的偏移与 fault 51 完全相同，IB1 布局在这个 loading 中是确定的；fault 63 是另一份布局，但顺序相同。
- 两份 snapshot 里 BV 的记录也是同一 IB1 的 bin 边界切出（2121 / 2551）。
- 取指位置附近（5438 / 5963）还有一次新的计算绑定，它只被预取、没有到达 SP；MCP 把它标为 `latest_write_may_not_have_reached_sp`，比较对象取更早的 1697 / 2127。
- **B 说明 `skipsaverestore` 不是原因**：即使 CP 在切出时做保存/恢复，SP 的计算程序寄存器也不在其中。之前把 KGSL 的 `skipsaverestore=1` 当作主因的推测不成立。
- **C 说明中途抢占是原因**：level 0 时 RB 只在提交边界切换，同一命令缓冲内的“绑定 → GMEM pass → dispatch”不会被打断，loading 不重发也通过。level 0 是整机设置（也影响 XR 合成器等系统 context 的响应），只用于验证，不作为修复。
- **片头保护写与本问题无关**：fault 60/61/64/65/66 都是 `CP Protected mode error | WRITE | addr=0`，BR IB1 取指位置都在 960，IB2 页内偏移都是 `0xe88`、剩余 0，与 KH3 问题 #11 同一签名；故障时 SP_CS 完好（`0x105`），同一 IB1 没有切出记录。它在 level 0 下照样出现（3 次），所以不依赖中途抢占。记录见 [KH3 问题汇总 #11](../kh3-cusa15072/README.md#11-启动期保护写-addr0)。

### fault 51：窗口内有一次 bin 边界切出

- **marker 解码订正**：此前的反汇编工具按 A6XX 格式解 `CP_SET_MARKER`。A8XX 上 MODE 是 bit0–6，USES_GMEM 是 bit7。所以 1979 的 `0x84` 是 `RM6_BIN_RENDER_START|USES_GMEM`（不是 BYPASS），2075 的 `0x07` 是 `RM6_BIN_RENDER_END`，`0x08` 是 `RM6_COMPUTE`。窗口 1697–5084 里因此有一个 GMEM pass（1979–2075）。
- **RB2 的切出点**：snapshot 中 RB2 的 CP 上下文记录（工具见 DDE bug 目录的 `ctxt_record.py`）显示：
  - 切出发生在本 IB1 的 dword 2077，紧跟 2075 的 `RM6_BIN_RENDER_END`；
  - 记录头 `info=0x80000007`。
- **info 低字节的含义**：低字节表示切出点的类型（由 SQE `IN_PREEMPT` 处理程序写入，按代码推断）。
  - `0x07`：bin 边界切出，走固件 `0x23bd` 的专用路径。
  - `0x11` / `0x91`：在 WFI、WAIT_FOR_ME 等同步点切出。
- **为什么是最后一次**：记录只保存每个 RB 最近一次切出，所以 2077 是 RB2 在故障前的最后一次切出。

### 不是整个上下文被清零

fault 51 的 BR pipe MVC 中，下列寄存器都在 IB1 开头写过一次、之后没再写：

| 寄存器 | 由谁写 | 故障时 |
|---|---|---|
| 0x980b、`PC_MODE_CNTL` 0x9b00、0xae0f、`VFD_MODE_CNTL` 0xa009、`GRAS_MODE_CNTL` 0x8213、0x8130 | `tu6_init_static_regs`（bin preamble 也会重写） | 完好 |
| `TPL1_CS_BORDER_COLOR_BASE` 0xb180、0xb302、0x9814/0x9815、`TPL1_MODE_CNTL` 0xb309 | `tu6_init_hw` 其它部分（bin preamble 不写） | 完好，其中 0xb180、0xb309 与 SP_CS_* 同在 cluster 7 |
| SP_CS_CNTL_0/1、CONFIG、INSTR_SIZE、PVT_MEM_SIZE、CONST_CONFIG、WIE/WGE | dword 1697 的计算状态 IB | 全为 0 |
| SP_CS_BASE | 同上 | 保留 |

所以丢失只发生在 SP 的计算程序寄存器上，不是“恢复时把所有没保存的寄存器清零”。

### 多份 snapshot 的对照

数据来自 `tools/cs_state_survey.py`。Swan 的 ASTRO snapshot（10-08，Turnip 尚无重发修复）在一次绑定后跟着 blit 和多个 GMEM bin，是现成的对照。

| snapshot | 计算状态写入之后 | 同一 IB1 记录的切出 | 故障时 SP_CS_CONFIG |
|---|---|---|---|
| Odin3 fault 52 | dispatch | 无 | 完好 |
| Odin3 fault 53 | dispatch、sysmem pass、A2D blit（8095） | 无 | 完好 |
| Swan `1790668481-devcd17`（血源） | dispatch、42 次 A2D blit | 无 | 完好 |
| Swan ASTRO `1791368040-devcd5` | dispatch、3 次 blit、3 个 GMEM bin | 无 | 完好 |
| Swan ASTRO `1791362212-devcd2`、`1791364557-devcd2`、`1791373107-devcd6` | blit、多个 GMEM bin | `0x11` | 完好 |
| Swan ASTRO `1791364523-devcd1`、`1791366333-devcd4`、`1791376239-devcd7` | blit、多个 GMEM bin | `0x11` | **0** |
| Swan ASTRO `1791366278-devcd3` | blit、11 个 GMEM bin | **`0x07`（bin 边界，2706）** | **0** |
| Odin3 fault 51、62（10-12 对照 A） | GMEM pass、A2D blit、sysmem pass | **`0x07`（bin 边界，2077）** | **0** |
| Odin3 fault 63（10-12 对照 B，`skipsaverestore=0`） | GMEM pass、A2D blit | **`0x07`（bin 边界，2507）** | **0** |
| Swan `1790598724-devcd15` | 4 次 dispatch、8 次 A2D blit | `0x91`（同步点，678，绑定后 20 dword） | 完好 |

（Swan devcd15 在 10-12 修正工具后才看出这一行：取指位置附近 1653 处还有一次只被预取的重新绑定，SP 里仍是 658 处写入的值。）

- **没有切出的现场，计算状态都完好**。所以 A2D blit、sysmem pass、GMEM pass 本身都不会清除它（fault 53 的 blit、ASTRO `1791368040-devcd5` 的 GMEM bin 都是这种情况）。
- **清零的现场都有切出记录**。两次 bin 边界切出（`0x07`）都清零了。
- **`0x11` / `0x91` 有清零有不清零**：记录只留最后一次切出，清零的那几份之前可能还有被覆盖的 bin 边界切出，这一点无法从 snapshot 确认。Swan devcd15 是同步点切出后状态完好的直接例子。
- **Swan 清零现场的 BASE**：`SP_CS_BASE` 是 `0x15ea3a00` / `0x15ea3700`，既不是我们写入的值也不是 0，很可能是抢占它的高优先级 context（XR 合成器）留下的。Odin3 fault 51 保留的是我们自己的 BASE。两者一致的解释是：切回后计算程序寄存器既不保存也不恢复，CONFIG 等被清成 0，BASE 停在最后一个写它的 context 留下的值。

### 与 KGSL、Turnip、驱动的对应

- **KGSL 配置**：gen8 默认 L1 抢占 + `skipsaverestore=1` + `usesgmem=1`（`adreno_gen8.c:2392-2396`，Odin3 实读 1/1/1，硬件 `CP_CONTEXT_SWITCH_CNTL=0x340`）。bug_reports 记录 Swan 的 `ctx_switch_cntl=0x341`，L1 抢占次数在百万级。10-12 对照 B 把 `skipsaverestore` 改为 0（硬件读回 `0x140`）仍丢失，所以丢失与是否跳过保存/恢复无关，只与中途切出有关。
- **Turnip 的依赖**：上游 `tu_emit_bin_preamble` 的注释写明，L1 + skipsaverestore 切回时 CP 不保存寄存器，只运行 bin preamble 恢复静态寄存器和 CCU，draw-state 在下一个 bin 重放。上游已为 bin size、LRZ、VSC、`PC_TESS_BASE` 分别补过重发（`b9d9fdb6d12`、`acdbfe9812f`、`423d472a4ec`、`12416c9fca5`），计算状态没有被覆盖到。对照 B 进一步说明：即便不跳过保存/恢复，CP 的保存范围也不含 SP 的计算程序寄存器，所以不能指望 KMD 侧改配置解决，只能由 UMD 在切回后重新提供（draw-state 组或 dispatch 前重发）。
- **高通驱动的做法**：bug_reports 记录的 0855.0.9 Vulkan 驱动把整组 SP_CS 寄存器放进每次 dispatch 的状态 IB（`CP_CONTEXT_REG_BUNCH`）。GLES 的计算 IB2 先设 `RM6_COMPUTE`，在每次 `CP_EXEC_CS` 前重发 bindless base、draw-state 组（含 0x10 组）和 NDRANGE（`qualcomm_gpu_driver/docs/mesh_pm4_aqe_protocol_0855_0_9.md`）。高通驱动不依赖计算状态跨命令保留，所以不受影响。
- **固件**：
  - gen8.2 SQE（`0x01510103`）在初始化时把一张 25 项的寄存器范围表写入控制寄存器 0x1a8–0x1af。表项包括各 stage 的 `SP_*_BASE`（含 `SP_CS_BASE`）、sampler/UAV/bindless base、VPC_SO、`PC_DRAW_INITIATOR`、`RB_BUFFER_CNTL`。
  - gen8.0 的 `0x01500080`、`0x01500081`、`0x01500103` 都没有这张表。
  - 表的含义未知，也解释不了 Odin3 保留 BASE 的现象，只记录为两代固件的差异。
  - Swan 用的已经是重做过抢占保存/恢复的 `0x01510103`，仍会清零，所以新固件不能替代 UMD 侧的处理。

### 对修复的影响

- **保持规避的范围**：`c5a0d6fd990` 的重发对所有 A8XX 生效，应当保持。Swan 同样受影响，`nocsreemit` 的 A/B 不应用来“证明 Swan 不需要”。
- **`c5a0d6fd990` 留下的窗口**：它的顺序是“重发 → `tu_emit_cache_flush`（含 WFI / WAIT_FOR_ME，属于 L1 同步点）→ 常量与描述符 → `RM6_COMPUTE` → `CP_EXEC_CS`”，而且只在“之间录入过其它命令”时才重发。在重发和缓存刷新之间发生同步点切出时是否安全，证据只有 fault 53 一例（4580 重发、4601 切出 `0x91`、随后的 dispatch 正常）。
- **10-12 的实现（`1efc1d3f5c9`，方案 A）**：A8XX 上 dispatch 的顺序改为：
  1. `tu_emit_cache_flush`；
  2. 若是偏移未 16 字节对齐的间接 dispatch，先做驱动参数到对齐临时槽的复制（自带 WFI）；
  3. 重发计算 shader 状态 IB；
  4. 常量、驱动参数加载、描述符和 load state（状态 IB 里的 `SP_UPDATE_CNTL` 会作废计算常量和 UAV 状态，所以这些必须在它之后）；
  5. `RM6_COMPUTE`、NDRANGE、`CP_EXEC_CS`。

  重发之后到 `CP_EXEC_CS` 之间只有 `CP_LOAD_STATE`、寄存器写入、`CP_SET_MARKER` 和间接缓冲调用，没有 WFI、WAIT_FOR_ME、事件写入或 bin 边界，snapshot 里见过的切出点（bin 边界、WFI、WAIT_FOR_ME、`CP_EVENT_WRITE7`）都落不进去；切出点只出现在这几处是统计推断。
  - **每次都发**：连续两次 dispatch 之间的缓存刷新也是同步点，所以不再用 c5a0 的“之间录入过其它命令”判断。只在上次发出状态后命令流里一个包都没有时跳过（例如绑定后紧接着 dispatch），那种情况下中间不可能有切出点。fault 68 证实了这一点：片头“绑定（dword 174）后紧跟 dispatch”的包序列与旧驱动完全相同。
  - **为拆分间接参数改了上游函数**：`tu_emit_compute_driver_params` 新增 `TU_DP_COPY` / `TU_DP_LOAD` 两个阶段，非 A8XX 和 `nocsreemit` 时走原来的 `TU_DP_ALL`，行为不变。
  - **仍未覆盖**：非对齐间接路径（BVH 构建，`CP_RUN_OPENCL`）在重发之后还有 `CP_WAIT_FOR_ME`；shadPS4 不用这条路径。
  - **代价**：除“绑定后紧跟 dispatch”外，每次 dispatch 多一次状态 IB 调用（约 44 dword），并重新加载描述符；没有做帧率对比。
- **方案 B（未做）**：像高通驱动那样，把计算状态放进 draw-state 组，由 CP 在每次 `CP_EXEC_CS` 前从组里重放（A8XX 的 DDE 处理 `CP_EXEC_CS`）。需要确认组号不与图形组冲突、`DISABLE_ALL_GROUPS` 与 render pass 之间的交互，以及 DDE 在计算路径上的行为；只在方案 A 仍有问题时考虑。
- **上游**：只要计算管线绑定和 dispatch 之间有 GMEM pass、又在 bin 边界被切出，上游 Turnip 在 A7XX/A8XX 上同样会丢计算状态。是否反馈给上游由用户决定（fork 规则不在 GitLab 参与）。

### 证据边界与待做

- **记录只保留最后一次切出**：无法看到同一 IB1 里更早的切出。`0x11` 类切出为何有时清零、有时不清，需要同窗的 KGSL 抢占 trace（`adreno_preempt_*` 带 level）来确定。
- **info 字节是推断**：其含义来自 SQE `IN_PREEMPT` 的代码路径（`0x23bd` 写 7，`0x2508` 起按位拼出 `0x11`/`0x91`/`0x39`），没有文档。
- **前一轮全局关闭抢占的实验**：fix-experiments 文档记录过 `pkt4-global-no-preempt`，用的是试验性的 PKT4 驱动，失败类型是 `gpu timeout`；原始证据在另一台机器的 `build/validation/odin3-kh3-fix-20261011/`，本机没有。10-12 的对照 C 用的是正式的 `c5a0d6fd990`（只加 `nocsreemit`），level 0 下 loading 通过，两者驱动不同，不矛盾。
- **对照的样本量**：A、B 各一次挂死，C 一次通过（另三次启动在片头被 #11 打断，没走到 loading）。挂死在修复前是必现的（fault 42–51 同一位置），所以 C 的一次通过已有区分度，但没有重复。
- **为什么 CP 保存/恢复不含这些寄存器**：只能看到结果（B 仍丢失）；`skipsaverestore=0` 时固件保存哪些寄存器、为何 BASE 保留而 CONFIG 等不保留，没有文档或固件层面的确认。
- **已恢复的设备状态**：对照结束后 `preempt_level=1`、`skipsaverestore=1`，`debug.mesa.tu.debug` 已清空，临时脚本已删除。
