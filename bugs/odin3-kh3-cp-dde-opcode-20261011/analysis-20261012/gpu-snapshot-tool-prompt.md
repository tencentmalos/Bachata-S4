# gpu-snapshot 解析工具改进任务（给另一个 AI）

## 背景

- **工具位置**：仓库 `C:\workspace\my_mcp_tools`（当前 `master` `f309a13`），代码在 `dev_tools\mcp\mcp_service_layer\Features\GpuSnapshot\`。主要文件：`GpuSnapshotParser.cs`、`GpuSnapshotDatabase.cs`、`GpuPm4Decoder.cs`、`KgslSnapshotNativeStructs.cs`、`GpuSnapshotAnalysisService.cs`。
- **这轮分析没用上工具**：对 Odin3（Adreno 830，gen8.0，chip `0x44050000`，SQE `0x01500081`）的 KH3 故障，这次只能用临时 Python 脚本，因为工具缺下面几项能力。参考实现（只读，按其逻辑移植，不要直接调用）：
  - `C:\workspace\emulations\shadps4\bugs\odin3-kh3-cp-dde-opcode-20261011\tools\ctxt_record.py`
  - `C:\workspace\emulations\shadps4\bugs\odin3-kh3-cp-dde-opcode-20261011\tools\dde_state.py`
  - `C:\workspace\emulations\shadps4\bugs\odin3-kh3-compute-state-20261011\tools\cs_state_survey.py`
- **期望输出**：
  - `C:\workspace\emulations\shadps4\bugs\odin3-kh3-cp-dde-opcode-20261011\analysis-20261012\ctxt_record.txt`
  - `C:\workspace\emulations\shadps4\bugs\odin3-kh3-cp-dde-opcode-20261011\analysis-20261012\dde_state.txt`
  - `C:\workspace\emulations\shadps4\bugs\odin3-kh3-compute-state-20261011\analysis-20261012\cs_state_survey.txt`
- **测试数据**（KGSL snapshot，均为 gen8.0）：
  - fault 51：`C:\workspace\emulations\shadps4\bugs\odin3-kh3-compute-state-20261011\evidence\fault51\snapshot.bin`
  - fault 52、53：`C:\workspace\emulations\shadps4\bugs\odin3-kh3-cp-dde-opcode-20261011\evidence\fault52\snapshot.bin`、`...\fault53\snapshot.bin`
  - gen8.2（Swan，Adreno 840，SQE `0x01510103`）对照：`C:\workspace\emulations\shadps4\build\validation\astro-swan-20261008\gpu-snapshots\kgsl-*.bin`、`C:\workspace\emulations\shadps4\build\gpu-snapshot\cp-opcode-20260928\kgsl-*.bin`（本机文件，不在仓库）
- **仓库规则**：遵守 my_mcp_tools 自身的规则。凡是按代码推断、没有文档依据的字段含义，在输出中标明证据等级（例如 `inferred`），不能写成已证实。

## 缺陷与需求

### 1. `CP_SET_MARKER` 解码错误（必须修）

**位置**：`GpuPm4Decoder.cs` 约 680–692 行。

**现状**：
- 枚举名错误：3 写成 `RM6_GMEM`，4 写成 `RM6_BYPASS`，5 写成 `RM6_ENDVIS`；7、8 显示为 `UNKNOWN_7/8`。
- 没有按芯片区分字段布局。

**正确定义**：以 Mesa `src/freedreno/registers/adreno/adreno_pm4.xml` 的 `enum a6xx_marker` 与 `domain CP_SET_MARKER` 为准，本机副本在 `C:\workspace\emulations\shadps4\references\mesa-turnip\src\freedreno\registers\adreno\adreno_pm4.xml` 第 1815–1900 行。

| 值 | 名称 |
|---|---|
| 1 | `RM6_DIRECT_RENDER` |
| 2 | `RM6_BIN_VISIBILITY` |
| 3 | `RM6_BIN_DIRECT` |
| 4 | `RM6_BIN_RENDER_START` |
| 5 | `RM6_BIN_END_OF_DRAWS` |
| 6 | `RM6_BIN_RESOLVE` |
| 7 | `RM6_BIN_RENDER_END` |
| 8 | `RM6_COMPUTE` |
| 12 | `RM6_BLIT2DSCALE` |
| 13 | `RM6_IB1LIST_START`（只由 KGSL 写） |
| 14 | `RM6_IB1LIST_END`（只由 KGSL 写） |
| 15 | `RM7_BIN_VISIBILITY_END` |
| 32 | `RM8_DEPTH_PASS_START`（A8XX） |
| 33 | `RM8_DEPTH_PASS_END`（A8XX） |
| 34 | `RM8_SET_RENDER_TARGET`（A8XX） |
| 35 | `RM8_PGMEM_ON`（A8XX） |
| 36 | `RM8_PGMEM_OFF`（A8XX） |

**字段布局**：
- **A6XX/A7XX**：MODE = bit0–3，USES_GMEM = bit4；bit8 = MARKER_MODE，置位时 bit0 是 IFPC_MODE（0 enable / 1 disable）。A7XX 另有 bit9 `SHADER_USES_RT`、bit10 `RT_WA_START`、bit11 `RT_WA_END`。
- **A8XX（gen8，chip 主版本 8）**：MODE = bit0–6，USES_GMEM = bit7；bit8 = MARKER_MODE（同上）。

**实现要求**：解码器按 snapshot 的 chip id 选择布局（gen8.0 `0x44050000`、gen8.2 `0x44050a31` 都是 A8XX）。输出 mode 名称、uses_gmem、ifpc_mode，以及 RT 位（仅 A7XX）。

**回归用例**（fault 51，BR IB1 `0x418e052000`）：

| IB1 dword | 原始值 | 应解为 |
|---|---|---|
| 1716 | `0x00000008` | `RM6_COMPUTE` |
| 1979 | `0x00000084` | `RM6_BIN_RENDER_START`，uses_gmem=1（不是 BYPASS） |
| 2075 | `0x00000007` | `RM6_BIN_RENDER_END` |
| 255 | `0x00000001` | `RM6_DIRECT_RENDER` |

KGSL 每次提交外包的 `0x0d` / `0x0e` 应解为 IB1LIST_START / END。

### 2. CP 上下文记录（preemption record）正文未解析

**现状**：
- `ParsePreemptionRecord` 只读了头部（`struct gen8_cp_preemption_record`：magic `0xae399d6e`、info、errno、data、cntl、rptr、wptr、rptr_addr、rbase、counter、bv_rptr_addr）。
- 正文中 CP 私有的 `0xfadebacN` 块完全没解析。
- Odin3 是 gen8.0，记录的 `source_profile_status` 却写成 `gen8_2_0_exact`（见下面第 4 项）。

**需求**：
1. 扫描整个记录（每份 `0x24000` 字节）中所有 `0xfadebacN`（N=0..f）标记，入库为“块偏移、块编号、到下一块之间的原始 dword”。
2. 解析以下块。字段含义都是按内容推断，必须标 `inferred`：
   - **`0xfadebac1`（IB 位置）**：`[1]` flags，`[2..3]` 本 pipe 正在执行的 IB1 base，`[4]` IB1 大小（dword），`[5]` **保存的 IB1 偏移（切出点）**，`[6..7]` 另一个 IB1 base（fault 51 中与 dmesg 的 BV IB1 `0x41dfdfe000` 一致），`[8]` 其大小。
     - 记录的 `+0x07480` 处为 BR，`+0x18dc4` 处为 BV（fault 51 中 BR 保存偏移 `0x81d`=2077，BV 为 `0x849`）。
   - **`0xfadebac2`**：`[1]` flags，`[2..3]` 最后调用的 IB2 base，`[4]` 大小。
   - **`0xfadebac9`（DDE 保存块）**：布局来自 DDE 固件 `0x01500081` 中 `0x05f4` 的保存子程序（`PREEMPT_COOKIE=0x1e`）。依次为：标记、控制寄存器 `0xc9`/`0xca`/`0xcb`、`0xc7`、`0xc6`、`APERTURE_CNTL`、`0x190`、`IB_LEVEL`、`0x011`（`IB_LEVEL==4` 时存 0）、`0x1f2`。
     - `APERTURE_CNTL` 为 `0x1000` 时是 DDE_BR，为 `0x2000` 时是 DDE_BV。
     - 恢复子程序 `0x061c` 先核对该标记，不符就以 `$19=0xbad9` 停机。
     - 新固件 `0x01500103` 在保存和恢复中都多了控制寄存器 `0x130`，块长度会多一个 dword。
     - 其它带同一标记、但不符合该布局的块按原样输出。
   - **`0xfadebacd` / `0xfadebacf`（寄存器写 FIFO）**：每项 5 dword（value、aux、hdr、hdr2、tag）。tag `0x18` 表示 value 是寄存器地址，紧随的 tag `0x10` 项是写入值；tag `0x11`、`0x14`、`0x17` 含义未知，原样输出。参考 `ctxt_record.py` 与 `build\validation\kh3-dde-20261012\rec2_fifo.txt` 的格式。
3. **info 字段的解读（推断）**：
   - `0x80000000` 表示没有中途切出（RB 空闲或在 IB 边界切换）。
   - 否则低字节是切出点类型（依据 SQE `0x01500081` 的 `IN_PREEMPT` 处理程序，`0x2360` 起）：
     - `0x07`：在 bin 边界切出（`RM6_BIN_RENDER_END` 之后），走 `0x23bd` 起的专用路径，该路径把 7 写入 SQE 暂存寄存器 `0x441`。
     - `0x11` / `0x91`：在 WFI、WAIT_FOR_ME 等同步点切出，走 `0x2508` 起的通用路径，在 `0x441` 中按位拼出类型。bit0 恒为 1；bit4 来自 `CONTEXT_SWITCH_CNTL` bit8（usesgmem）；bit7 来自控制寄存器 `0x13b` bit9 的分支；bit3 来自 MARKER bit5 或控制寄存器 `0x278` bit6。
     - `0x39`：其它通用路径组合。
   - 一个 RB 只有一份记录，每次切出都会覆盖，所以记录只代表**最后一次**切出。
4. **把记录与故障关联起来**：
   - 若记录的 IB1 base 与当前 CP_IB1_BASE 相同，且记录 rptr 与该 RB 当前 rptr 相差不超过约 8 dword，标为“同一次提交中途切出”。
   - 在冻结的 IB1 上定位保存偏移：给出该处的包、前几个包、之前最近的 `CP_SET_MARKER` 及距离。
   - 统计从保存偏移到故障 IB2 调用（或到 IB1 取指位置）之间的 IB1 dword 数和 IB2 调用数。
   - 期望值：fault 51 的保存偏移 2077 紧跟 2075 的 `RM6_BIN_RENDER_END`；fault 53 的保存偏移 4601 是 `CP_WAIT_FOR_ME`，到故障 IB2 `0x426ba05644`（在 IB1 dword 8212 调用）共 3611 dword、39 次 IB2 调用。

### 3. MVC 寄存器与命令流关联分析（新增）

1. **CP 执行位置**：A8XX 的 CP pipe 寄存器在 MVC 的 BR pipe、cluster 0 中可以读到：`0x900`/`0x901` CP_IB1_BASE、`0x902` CP_IB1_REM_SIZE、`0x903` CP_IB1_INIT_SIZE。`INIT - REM` 是**取指位置**，执行位置在它之前。fault 51 的值：base `0x418e052000`、INIT `0x7ff1`、REM `0x6a71`，取指位置 5504。在报告中直接给出这个位置。
2. **寄存器来源分析**：对指定寄存器（或命名寄存器组，例如下面的 SP_CS 组），在当前 IB1 及其调用的已冻结 IB2 中：
   - 找出最后一次写入，包括 PKT4 和 `CP_CONTEXT_REG_BUNCH`（opcode `0x5c`，载荷为“寄存器、值”交替）；IB2 内的写入记在调用它的 IB1 dword 上。
   - 列出写入之后发生的 GMEM pass（USES_GMEM / BIN_RENDER_START / BIN_RENDER_END）、`CP_BLIT`（`0x2c`，A2D）、`CP_EXEC_CS`（`0x33`），以及同一 IB1 的中途切出记录。
   - 与 MVC 中 BR pipe 的值（按 HW context c0/c1 和 slice 分列）比较，分类为“保持写入值”“被清零”“变成其它值”“写入点离取指位置不足约 600 dword，可能尚未到达 SP”。
3. **MVC 查询接口**：
   - 按 pipe（BR/BV/LPAC）、cluster、HW context、slice 过滤。
   - 默认只给 BR pipe。LPAC 副本经常是 0，不能拿来判断丢失。
4. **验收**：用 `cs_state_survey.py` 列出的 snapshot 复现 `cs_state_survey.txt` 中的结论。
   - fault 53：最后一次 CS 状态在 7578 写入，之后有 `CP_BLIT`（8095），故障时 SP_CS_CONFIG 仍为 `0x400105`。
   - ASTRO `1791366278-devcd3`：切出类型 `0x07`（2706），SP_CS_CONFIG 为 0。
   - fault 51 需要特殊处理：工具会在取指位置附近（5249 的 IB2、5438）看到更新的 CS 写入，但它们还没执行。真正生效的是 1697 那次写入（`0x105`），MVC 中为 0，只有 `SP_CS_BASE` 保留 `0x4055a0980`。

### 4. 版本标签与已知遗留

- **版本标签**：所有 `source_profile_status = 'gen8_2_0_exact'` 都是写死的（`GpuSnapshotParser.cs` 645、651、723 行，`GpuSnapshotDatabase.cs` 1742 行），应按 chip id 区分 gen8.0 与 gen8.2。gen8.0 的布局是否与 gen8.2 完全相同未证实，未核对的字段标为“按 gen8.2 布局推断”。
- **唯一约束错误**：`f309a13` 修复的 `program_candidate_mappings` 唯一约束错误还没有进入已安装的发布包，本机发布版解析 fault 51 仍然失败，需要重新打包发布（这一项不改代码）。
- **DDE ROQ 截断**：KGSL 对 gen8.0 的 DDE ROQ 只转储 `0x100` dword，实际为 `0x200`（`adreno_gen8_0_0_snapshot.h:1809`，只有 gen8_2_0 修了），报告中要注明 DDE ROQ 可能不完整。
- **draw-state 载荷未冻结**：KGSL 只冻结带 DIRTY 或无 flags 的 SDS 组载荷，所以 pipeline/着色器组常常没被冻结。报告应区分“未冻结”和“未映射”（MEMLIST 中有没有）。

### 5. 可选：固件 PC 映射

- **SQE 与 DDE 的陷阱码**：报告应能识别非法包陷阱，并把 `$19` 映射成下表含义。

  | 固件 | 停机 PC | `$19` 含义 |
  |---|---|---|
  | gen8.0 DDE `0x01500081` | `0x717/0x718`（`0x715/0x716` 先向控制寄存器 `0xaa` 写 `1<<8`） | `0xbad1`：分发表中指向 `0x70c` 的 95 个入口，含 UNKN0<br>`0xbad2`：UNKN96<br>`0xbad3`：UNKN1<br>`0xbad4`：UNKN5，即全零包头<br>`0xbad5`：UNKN6<br>`0xbad9`：恢复时标记不符（`0x0707`） |
  | gen8.0 BR | `0x0350` | 非法包 |
  | gen8.2 BR | `0x03cd` | 非法包 |
  | gen8.2 DDE | `0x078f` | 非法包 |

- **BR 等 DDE 的循环**：BR 在 `CP_SET_DRAW_STATE` 中等待 DDE 的循环位于 gen8.0 SQE `0x38d–0x392`。循环读控制寄存器 `0x169`，直到 bits[5:4] 等于 bits[13:12]，按推断是 BR 与 DDE 之间的 2 位计数。

## 附：这一轮识别出的寄存器和解读（供工具内置说明使用，均需标注证据等级）

| 寄存器或项 | 内容 | 证据 |
|---|---|---|
| SP_CS 寄存器组 | `0xa9b0` CNTL_0、`0xa9b1` CNTL_1、`0xa9b3` PROGRAM_COUNTER_OFFSET、`0xa9b4/5` BASE、`0xa9b6–8` 私有内存参数与基址、`0xa9b9` PVT_MEM_SIZE、`0xa9bb` CONFIG、`0xa9bc` INSTR_SIZE、`0xa9bd` PVT_MEM_STACK_OFFSET、`0xa9be` HYSTERESIS、`0xa9c2/3` WIE_CNTL_0/1、`0xa9c5` VGS_CNTL、`0xa9cd` CONST_CONFIG、`0xa9d4–0xa9da` NDRANGE_0..6、`0xa9db` WGE_CNTL、`0xa9dc–de` KERNEL_GROUP_X/Y/Z、`0xa9df` NDRANGE_7、`0xa9e8` 起 CS_BINDLESS_BASE。在 MVC 中位于 BR pipe 的 cluster 7，有 2 个 HW context、3 个 slice | Mesa `a6xx.xml` |
| `0xa9bf` SP_A2D_OUTPUT_INFO | A2D blit 使用，位于 CS 寄存器区间内 | Mesa；snapshot 中有值 |
| 计算状态丢失的现象 | L1 切出后切回时，CS 程序寄存器（CNTL_0/1、CONFIG、INSTR_SIZE、PVT_MEM_SIZE、CONST_CONFIG、WIE、WGE）被清零，BASE 保持最后写入者的值。同一 cluster 中只在命令缓冲开头写过一次的 `TPL1_CS_BORDER_COLOR_BASE`（`0xb180`）、`TPL1_MODE_CNTL`（`0xb309`）完好 | 多份 snapshot 对照；机制为推断 |
| 控制寄存器 `0x130` | 位于 `CONTEXT_SWITCH_CNTL`（`0x131`）、`PREEMPT_COOKIE`（`0x132`）、`PREEMPT_ENABLE`（`0x133`）旁边，名称未知。旧 DDE 固件只在初始化时写 1；`0x01500103` 在每处写 `0x1f2` 时同时写它，并加入抢占保存/恢复 | 固件对比 |
| 控制寄存器 `0x1f2` | DDE 的 dispatch/draw 状态：`CP_EXEC_CS` 开始时写 `$18^1`，结束时写 1；DDE 抢占时保存与恢复 | 固件代码 |
| 控制寄存器 `0x190` | DDE 在 `CP_EXEC_CS` 开始时写 1、结束时写 0；保存块中也存它 | 固件代码 |
| 控制寄存器 `0x169` | BR 与 DDE 的握手计数（见缺陷 5） | 固件代码，推断 |
| 控制寄存器 `0xaa` | 陷阱停机前写 `1<<8`（bad1–bad5）或 `1<<10`（bad9） | 固件代码 |
| 控制寄存器 `0x441`、`0x444` | SQE 暂存寄存器：`0x441` 为切出类型；`0x444==4` 时进入 bin 边界路径 | 固件代码，推断 |
| 控制寄存器 `0x1a8–0x1ab`、`0x1af` | gen8.2 SQE 初始化时向 `0x1a8–0x1ab` 写 4 个头部 dword，再向 `0x1af` 连写 25 项寄存器范围。每项低 16 位是起始寄存器，第三字节可能是数量。表项包括 VPC_SO、`PC_DRAW_INITIATOR`、`SP_VS/HS/DS/GS/PS/CS_BASE`、sampler/UAV/bindless base、`RB_BUFFER_CNTL`。gen8.0 的 `0x01500080/81/103` 都没有这张表；用途未知 | 固件字节；用途未证实 |
| 新 DDE 固件 `0x01500103` 的 `CP_EXEC_CS_INDIRECT` | NDRANGE 全局尺寸改用 32 位乘法；组数为 0 时先写 NDRANGE 与 KERNEL_GROUP 再跳过 | 固件对比 |
| KGSL gen8 抢占配置 | `preempt_level=1`、`skipsaverestore=1`、`usesgmem=1`；Turnip 未指定优先级的 context 落在 RB2 | KGSL 源码 `gfx-kernel.lnx.15.0.r12-rel` `2974f90`；Odin3 debugfs 实读 |
| A8XX opcode | `0x13` CP_WAIT_FOR_ME、`0x26` CP_WAIT_FOR_IDLE、`0x2c` CP_BLIT、`0x33` CP_EXEC_CS、`0x3f` CP_INDIRECT_BUFFER、`0x43` CP_SET_DRAW_STATE、`0x46` CP_EVENT_WRITE7、`0x5c` CP_CONTEXT_REG_BUNCH、`0x65` CP_SET_MARKER | Mesa |
| A7XX+ 事件号 | `0x18` CCU_INVALIDATE_DEPTH、`0x19` CCU_INVALIDATE_COLOR、`0x1a` CCU_RESOLVE_CLEAN、`0x20` CCU_CLEAN_DEPTH、`0x21` CCU_CLEAN_COLOR、`0x26` LRZ_CACHE_FLUSH、`0x28` LRZ_CACHE_INVALIDATE、`0x31` CACHE_CLEAN、`0x33` CACHE_INVALIDATE7 | Mesa |
| snapshot 格式 | GPU_OBJECT_V2 头部为 `<iQQQ>`（type、gpuaddr、ptbase、dword 数），28 字节；MEMLIST_V2 每项 20 字节（`<QQI>`）；MVC_V3 数据按 `gen8_legacy_snapshot_mvc` 排列 | KGSL 源码 |

## 交付要求

- **测试**：每项修复附回归测试，用上面的 snapshot 与期望值；Swan snapshot 不在仓库时测试应能跳过。
- **兼容性**：不改变已有表的语义，新增表或视图（例如 `preemption_record_blocks`、`preemption_switch_outs`、`register_provenance`）。
- **报告措辞**：推断字段在 review 报告和 JSON 中带 `inferred`，并给出依据（固件版本与 PC、KGSL 源码行号、Mesa XML）。
