# Odin3 KH3：`CP DDE BR opcode=0` 闪退（fault 53）与启动期保护写（fault 52）

**状态：未修复，根因未定位。** 本文汇总现场与已能确认的事实，供在有 bug_reports、固件反汇编工具和设备的环境中继续确认。

> **2026-10-12 续查更正**（详见文末“固件 PC 与实际执行点”）：
> - 固件确认 DDE 读到全零包头进入非法包陷阱（`$19=0xbad4`），与 Swan B 类相同；BR 只是在等 DDE。
> - BR 实际执行到 IB2 dword **814**（draw 806 之后的 `CP_SET_DRAW_STATE`），不是 1301；1327 只是 ROQ 预取位置。DDE 最后处理完的是 draw 806。
> - `0x4314…`、`0x40051c7…` 两块 draw-state 载荷**已被冻结**，内容合法；未冻结的只有 pipeline/着色器所在的 `0x4331…`、`0x42e6…`、`0x41b3…`，它们在故障时仍有映射。

## 环境

| 项 | 值 |
|---|---|
| 设备 | AYN Odin3 `6edb0593`，Adreno 830，chip `0x44050000`（gen8.0），boot ID `c322cd23-e6c8-46f8-8eb7-ff032d7162db` |
| ROM | `qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys` |
| 系统 GPU 驱动 / 固件 | `V@0800.72`（`GIT@3967b80d3b`），SQE 版本字见 [固件分析](../../docs/validation/android-native-host/odin3-kh3-firmware-analysis-20261011.md) |
| KMD | `msm_kgsl.ko` Build-ID `8ad43a33afb6affe54edeb6804aa14089dcfe7ce`，源码映射未确认 |
| Turnip | `c5a0d6fd990`（分支 `codex/a8xx-compute-state-reemit`，含 A8XX 计算状态重发修复），驱动 SHA-256 `bfc3af8b891de220c13190daf9386fd271b574e9df6656ce33161dd1360b57db`，`TU_DEBUG=gmem_warmup` |
| APK | `84d8c605cbf267b257b1d2437f3625a977ae1fc7258db87d3fb64074a36a87f6`（含 `MemoryManager::PoolCommit` 修改） |
| 游戏 | Kingdom Hearts III `CUSA15072` Ver 1.10 |

## fault 53：运行中闪退

会话 PID 28627，run UUID `c866d20b723b5027e51df22b32cd82cc`。新游戏 → “觉醒之所”教学场景，约 30 FPS 运行约 19 分钟；崩溃前正在进入新区域（日志里有一批新管线首次编译，`async_graphics_skip` 跳过少量 draw）。当时 DebugBus `upload_diag` 在记录纹理上传，只做统计。

时间线：

1. dmesg（uptime 171360.077）先报 GPU 故障，faultcount 52 → 53：

   ```text
   adreno-gen8-gmu 3d37000.qcom,gmu: CP DDE BR opcode error | opcode=0x00000000
   kgsl kgsl-3d0: Fault id:2 and GX is ON
   kgsl kgsl-3d0: shadps4.android[28627]: ctx 6 ctx_type ANY ts 195670 policy 2
   kgsl kgsl-3d0: status 00880005 gfx_status 00880004 gfx_br_status 00880004 gfx_bv_status 00880004
   kgsl kgsl-3d0: BR: rb 1b88/1c30 ib1 000000425F88B000/4b56 ib2 000000426BA05644/0317 ib3 0000000000000000/0000
   kgsl kgsl-3d0: BV: rb 1b8c/1c30 ib1 0000004038028000/0000 ib2 0001FF8002A36190/0000 ib3 0000000000000000/0000
   kgsl kgsl-3d0: GPU snapshot froze 7424Kb of GPU buffers
   ```

2. 宿主 21:00:21.192：Turnip `tu_knl_kgsl.cc:1797: bind submit failed: Protocol error (VK_ERROR_DEVICE_LOST)`，`BufferCache::SubmitPendingArenaBinds` 断言 `vkQueueBindSparse failed: ErrorDeviceLost`；随后 Present 线程 `PrepareLastFrame` 断言 “Device lost during waiting for a frame”，进程 abort（`logcat-crash.txt`）。两处断言是 DeviceLost 的后果，按既定决定不修。

### snapshot

root 一次性读取（`/data/local/tmp/malos_kh3_capture.sh`，读取时间 21:01:55）：17,069,616 字节，SHA-256 `7b2653a17148ba836149687793f46ff43a1d64ed6a0ec79019cc01e33396430d`，END 完整，1205 个 section。

| 项 | 值 |
|---|---|
| BR IB1 | `0x425f88b000`，共 27734 dword，剩余 19286（与 dmesg `0x4b56` 一致） |
| BR IB2 | `0x426ba05644`，共 2118 dword，剩余 791（与 dmesg `0x317` 一致），已取 1327；snapshot 内为 section 1182，payload 文件偏移 5030448 |
| BR ROQ | IB2 原始值 `0x02000080`，补偿方式未验证 |
| DDE_BR ROQ | 原始值 `0x000d000d` |
| BV | 空闲（IB1 剩余 0） |
| 抢占记录 | 故障环（第三条记录，rptr 7048 = dmesg 的 `0x1b88`）`info=0x80000091`，其它环 `0x80000000`，含义未核实 |

IB2 解码（`ib2-0x426ba05644-decoded.txt`）中游标附近：

| IB2 dword | 包 |
|---:|---|
| 1255 | `CP_SET_DRAW_STATE`，1 组（id 11，`0x431449adf8`，5 dword） |
| 1259 | `CP_SET_DRAW_STATE`，11 组（id 10 起，`0x431449ad74` 等） |
| 1293 | `CP_DRAW_INDX_OFFSET`（TRIANGLE_LIST，DMA 索引） |
| **1301** | **`CP_SET_DRAW_STATE`，11 组（id 0 起：`0x433182ae80` 25 dword、`0x4331829c00`、`0x4331829cb0`、`0x40051c7000`…）——已取位置 1327 落在该包内** |
| 1335 / 1339 | 下一组 `CP_SET_DRAW_STATE` |

~~draw-state 载荷（`0x4331…`、`0x4314…`、`0x40051c7…`）都不在 snapshot 冻结的 GPU 内存里~~（10-12 更正：`0x43142b3000` 起 4 MiB 与 `0x40051c7000` 起 128 KiB 已冻结，见文末；未冻结的是 `0x4331…`、`0x42e6…`、`0x41b3…`）。上表“已取位置 1327 落在 1301 包内”只是 ROQ 预取位置，实际执行点见文末。

### 解析边界

- gpu-snapshot 工具对 `0x44050000` 没有已验证的 PM4 组合解码 profile，MVC 游标寄存器映射为 semantic-only，工具不输出根因（`analyze.json` 的 warnings）。
- 由于 ROQ 预取，真正执行的包可能早于 1301；本次没有用固件 PC 定位 DDE 的执行点。
- 能确定的：故障类别（dmesg）；MVC 中 BR IB1/IB2 的地址与剩余量和 dmesg 逐项一致；停止区域在一条管线 draw-state 的 `CP_SET_DRAW_STATE` 附近。

## fault 52：启动期保护写（同一 boot，较早）

会话 PID 28009，片头阶段：

```text
adreno-gen8-gmu 3d37000.qcom,gmu: CP | Protected mode error | WRITE | addr=0x00000 | status=0x00600000
kgsl kgsl-3d0: shadps4.android[28009]: ctx 6 ctx_type ANY ts 5593 policy 2
kgsl kgsl-3d0: status 00880005 gfx_status 00880004 gfx_br_status 00880004 gfx_bv_status 00880004
kgsl kgsl-3d0: BR: rb 00b3/015b ib1 000000418FBAE000/33d3 ib2 000000418F9F6E88/0000 ib3 0000000000000000/0000
```

snapshot 7,074,740 字节，本轮未逐包分析。IB2 页内偏移 `0xe88`、剩余 0，与 [固件分析](../../docs/validation/android-native-host/odin3-kh3-firmware-analysis-20261011.md) 中“保护写对照：BAD4 路径已在重读 IB”一节记录的保护写签名相同（固件在 `$19=0xbad4` 恢复路径中重读 IB）。

10-12 计算状态对照实验中又出现 5 次同一签名（fault 60、61、64、65、66），其中 64–66 是在 `preempt_level=0`（不做中途抢占）下出现的，fault 64 的 CP 上下文记录里也没有中途切出。所以这一类（下表的 BR 类）不依赖 L1 中途抢占，和 DDE 类的抢占关联不同。详见 [KH3 问题汇总 #11](../kh3-cusa15072/README.md#11-启动期保护写-addr0)。

## 与已有记录的关系

- Swan 上未解决的 B 类：BR 停在 `CP_SET_DRAW_STATE` 等 DDE，DDE 读到全零包头（固件 PC 映射到非法包陷阱 `$19=0xbad4`），三次有 trace 的故障都在 RB2 从 level=1 抢占恢复后 80–115 µs 内。见 [Swan 固件报告](../../docs/validation/android-native-host/swan-cp-opcode-firmware-20260930.md)、[Swan CP 深挖](../../docs/validation/android-native-host/swan-cp-opcode-20260929.md)。
- Odin3 10-08 曾记录一次 `CP DDE BR opcode=0`（见仓库根 `AGENTS.md` 的 10-09 条目）。
- 修改前后都会出现，不是 `PoolCommit` 修改或计算状态重发修复引入的。用户说明：高通驱动约 855.0.9 前后固件一同更新后这类问题大幅减少；Odin3 当前为 `V@0800.72`。

## 待确认（需要相应环境）

1. ~~**固件 PC**~~：10-12 已完成，见文末。
2. **bug_reports 对照**：查 gen8.0 固件在上下文切换/抢占恢复时 DDE 读 draw-state 出错的记录，以及哪一版固件修复；判断 Odin3 能否换固件验证。10-12 在本机 bug_reports 中检索 `DDE BR opcode`，只命中 gen8.2（sm8850）固件反汇编与通用说明，没有同类故障记录。同日找到 sm8850 驱动包里更新的 gen8.0 固件 `0x01500103`，DDE 差异见文末；是否适用于 Odin3 未确认。
3. **抢占关联**：10-12 由 snapshot 中的 CP 上下文记录确认 DDE 类故障与“同一提交中途 L1 切出”强相关（见文末），但只是统计关联。仍需同窗采集 KGSL 抢占 ftrace（`swan-kgsl-debugger` 技能，独立 tracefs instance）；对照实验候选见文末“可做的对照”。
4. **draw-state 内容**：已冻结的组都合法（见文末）；剩下的是 pipeline/着色器组（`0x4331…`、`0x42e6…`、`0x41b3…`），KGSL 在故障时有映射却没有冻结。可用 `TU_DEBUG=rd` 记录提交及其 BO（开销大，只在复现窗口短时开启），或在 DeviceLost 时由 Turnip 转储包含指定 VA 的 BO，核对 draw 806 / SDS 814 引用的这几组。
5. **复现**：KH3 新游戏进入“觉醒之所”后正常游玩；fault 53 出现在会话约 19 分钟、进入新区域时，fault 52 出现在片头。没有稳定复现步骤，每次需要 root 立即读取 snapshot（读一次即清空）。

## 重新分析

- snapshot：gpu-snapshot MCP `gpu.snapshot_analyze`（`snapshotPath` 指向 `evidence/fault53/snapshot.bin`），数据库可重建。此前发布的 MCP 解析该机 snapshot 会报 `UNIQUE constraint failed: program_candidate_mappings`，my_mcp_tools `master` `f309a13` 已修复（`FindMemoryIndex` 先按引用身份查找，附回归测试）；在其之前构建的发布包仍会报此错，需用新版本。
- IB2 解码：`gpu.pm4_decode`，`inputPath` 为 snapshot，`byteOffset` 5030448，`dwordCount` 2118，`baseGpuAddress` `0x426ba05644`。

## 文件

| 文件 | 说明 |
|---|---|
| `evidence/dmesg-kgsl-faults.txt` | fault 52、53 的 KGSL 记录及前后上下文（行号对应 `fault53/dmesg.txt`） |
| `evidence/fault53/snapshot.bin` | KGSL snapshot |
| `evidence/fault53/dmesg.txt` | 完整 dmesg（同时含 fault 52） |
| `evidence/fault53/android-host.log` | 宿主日志（20:41 会话启动至崩溃）；被 `.gitignore` 的 `*.log` 排除，仓库里没有，SHA 见 `SHA256SUMS` |
| `evidence/fault53/logcat-crash.txt`、`logcat-main-tail.txt` | crash buffer 与主 buffer 尾部 |
| `evidence/fault53/analyze.json`、`snapshot-review.md` | gpu-snapshot 解析结果与自动 review |
| `evidence/fault53/ib2-0x426ba05644-decoded.txt` | BR IB2 全量解码 |
| `evidence/fault53/*.txt`（boot_id、date、uptime、faultcount、snapshot-timestamp-*） | 读取身份 |
| `evidence/fault52/snapshot.bin`、`android-host.log`、身份文件 | fault 52 现场（`android-host.log` 同样未入库） |
| `tools/dde_state.py` | 只读 snapshot，输出 BR/DDE 的 SQE 状态、BR ROQ 对应的 IB2 范围、BR draw-state 表、DDE_BR ROQ、各 draw-state 组的冻结内容与 MEMLIST 映射 |
| `analysis-20261012/dde_state.txt` | 上述脚本对 fault 53 的输出 |
| `tools/ctxt_record.py` | 只读 snapshot，解码各 RB 的 CP 上下文记录（头部、`0xfadebacN` 块、DDE 保存块），把保存的 IB1 偏移映射回冻结的 IB1，并计算到故障 IB2 的距离 |
| `analysis-20261012/ctxt_record.txt` | 上述脚本对 fault 53 的输出（`--fault-ib2 0x426ba05644`） |

更多上下文：[性能剖析与闪退记录](../../docs/validation/android-native-host/odin3-kh3-performance-20261011.md)。

## 固件 PC 与实际执行点（2026-10-12）

复现：`python -I tools/dde_state.py evidence/fault53/snapshot.bin`（输出存于 `analysis-20261012/dde_state.txt`）。

**固件**：用户提供的 `odin3-gen8-0.zip`（SHA-256 `5aa381cd…ced1`，内含 bug_reports 的 qrisc-disasm 反汇编）。其中 `gen80000_sqe.fw` SHA-256 `9c8f009b…d6b5`，与 10-09 从 Odin3 拉取的固件文件相同，版本字 `0x01500081`。固件与反汇编不入库，本机副本在 `build/validation/kh3-dde-20261012/`。

### DDE_BR：读到全零包头，进入非法包陷阱

| 项 | 值 | 含义 |
|---|---|---|
| PC | `0x717` | DDE 映像 `0x717/0x718` 的原地跳转，停机循环 |
| `$01` | `0x00000000` | 当前包头为 0 |
| `$19` | `0xbad4` | 只在分发表 `UNKN5`（`0x712`）跳转的延迟槽 `0x713` 赋值 |
| `$02` | `0x100` | `0x715/0x716` 写入控制寄存器 `0xaa` 的值 `1<<8` |

分发表中指向 `0x70c` 的 95 个入口（含 `UNKN0`）置 `0xbad1`，`UNKN96` 置 `0xbad2`，`UNKN1` 置 `0xbad3`，`UNKN5` 置 `0xbad4`，`UNKN6` 置 `0xbad5`，之后都进入同一段：写 `0xaa` 后原地停机。`$01=0` 与 `0xbad4` 一致，与 [Swan B 类](../../docs/validation/android-native-host/swan-cp-opcode-firmware-20260930.md)（固件 `0x01510103`，DDE `0x078f`）是同一类故障。“`UNKN5` 对应类型非法的全零包头”沿用 Swan 报告的推断，没有硬件文档。

### BR：在 `CP_SET_DRAW_STATE` 里等 DDE

BR PC `0x392` 是 `CP_SET_DRAW_STATE` 处理函数第一个等待循环（`0x38d–0x392`）的延迟槽。循环读控制寄存器 `0x169`，直到 bits[5:4] 等于 bits[13:12] 才继续：

- 当前 `$03=0`（bits[5:4]）、`$04=3`（bits[13:12]）。
- `$02=0x1e` 是 `cmp` 的“小于”结果。

这些字段的含义按处理函数推断为 BR 与 DDE 之间的计数，没有文档。`$01=0x70438021`、`$1c`（rem）`=0x21`：包头已读，33 个载荷 dword 一个都没读。BR 只是在等停住的 DDE。

### 实际执行点：IB2 dword 814，最后一个 draw 是 806

三处独立证据一致：

1. **BR ROQ**：`CP_ROQ_DBG` pipe 1 中 IB2 用一个 512 dword 的环。环里是 IB2 dword [815, 1327)，最老的 815 正是 dword 814 处 `CP_SET_DRAW_STATE` 的第一个载荷 dword。最新的 1326 与 dmesg 的 IB2 剩余 `0x317` 对应，所以 1327 是预取位置，不是执行位置。
2. **BR draw-state 表**：`CP_DRAW_STATE_DBG` pipe 1 每组两个槽，里面只有 dword 663/697/701（draw 729 用）和 737/771/775（draw 806 用）这几个 `CP_SET_DRAW_STATE` 写入的地址，没有 814 及之后的。
3. **DDE_BR ROQ**：`CP_ROQ_DBG` pipe 6 前 0x60 dword 是 24 个四元组（值、标记地址低/高、父包头）。
   - 最新一批是内部包 `0x70430002`（`0x11, 0x4`），后接转发的 draw `0x7038800c` 及 12 个 dword。
   - 这些 dword 含 1152（`0x480`）个索引、索引基址 `0x400a391400`、上限 `0x02b33600`，只与 IB2 dword 806 的 draw 吻合（TRIANGLE_LIST，32 位索引）。
   - 更早一批是 draw 729 的尾部。
   - 标记地址分别等于 dword 701、775 的 17 号组地址。

所以：DDE 完整读完了 draw 806 的转发包，下一个包头读到 0；BR 已读入 dword 814 的包头，正等 DDE。

### draw-state 载荷

- snapshot 冻结了 `0x43142b3000` 起 4 MiB（命令缓冲的动态状态组）和 `0x40051c7000` 起 128 KiB（HS/DS/GS 空状态），type 2 与 type 4 两份内容相同。
  - dword 697/701/771/775/814/848/852 中落在这两块里的组（9、10、11、13、17、27、28、29 及 3、4、5）全部可按 PM4 解析，长度与组计数一致，没有全零包头。
  - DDE_BR ROQ 第二块（pipe 6 的 `0xe8–0xf3`）里 HS/DS/GS 三组的内容与内存逐字相同，每组按 4 dword 补零对齐（布局为推断）。其前的 `0xb8–0xe7` 正好 48 dword，与 draw 806 的 VS 组长度一致，但该 BO 未冻结，无法比对。
- 未冻结的是 pipeline/着色器组：0 PROGRAM_CONFIG、1 VS、2 VS_BINNING、7 VPC、8 FS、18、26。
  - 它们位于 `0x4331827000`、`0x42e609b000`、`0x41b3aca000` 起的 128 KiB BO。
  - MEMLIST_V2 显示故障时三者都仍有映射，不支持“BO 已被释放”的解释；未冻结的原因未查。
  - shadPS4 运行中只在构建失败或着色器热重载时才删除 pipeline（`vk_pipeline_cache.cpp`），这一侧也没有释放路径。
- DDE ROQ 中 GS 之后的 `0xf4–0xfb`（`c8220002 1 c8220003 1 c6700004 1 70dc800a 0000a013`）未能解读：按组顺序应是 8 号（FS）组，但 `0xc8220002` 不是合法包头，可能是另一种布局或元数据，不作结论。

### 仍未确定

- DDE 那个 0 包头来自哪里：可能是未冻结的 pipeline 组内容、BR→DDE 的内部通道，或获取时读到尚未可见的数据。现有 snapshot 无法区分。
- `0x169`、DDE ROQ 状态字（`0x60–0x7f`）、`$1d=0xf000054b`、`$1f` 的含义均无文档。
- 下一步：在 DeviceLost 时转储 draw 806 / SDS 814 引用的 pipeline 组（或用 `TU_DEBUG=rd`），以及与抢占 ftrace 同窗复现。

## 抢占与 DDE：KGSL / Turnip / 固件（2026-10-12）

复现：`python -I tools/ctxt_record.py evidence/fault53/snapshot.bin --fault-ib2 0x426ba05644`（输出 `analysis-20261012/ctxt_record.txt`）。

- KGSL 源码：CodeLinaro `graphics-kernel` 的 `gfx-kernel.lnx.15.0.r12-rel`（`2974f90`，2026-08-29），未与 Odin3 的 `msm_kgsl.ko` 精确对应。
- Turnip：故障时所用的 `c5a0d6fd990`，`TU_DEBUG=gmem_warmup`，未开 `sds_page_align`。

### KGSL：游戏在 RB2 上，可被中途切出，L1 切出跳过寄存器保存/恢复

- **gen8_0_0 的抢占配置**：
  - 启用 `ADRENO_PREEMPTION`，`preempt_level = 1`（`adreno-gpulist.h:2954` 起）。
  - probe 时把 `skipsaverestore`、`usesgmem` 设为 true（`adreno_gen8.c:2392-2396`）。`adreno.h:388` 的注释：skipsaverestore 即“L1 抢占时跳过 save/restore”。
  - hwsched 下这三项经 `HFI_FEATURE_PREEMPTION` 交给 GMU（`adreno_gen8_hwsched_hfi.c:1826-1838`），抢占由 GMU 发起。
  - Odin3 debugfs `preemption/` 下实读 `preempt_level=1`、`skipsaverestore=1`、`usesgmem=1`。
- **Turnip 的 context 落在 RB2**：
  - 建 context 时（`tu_knl_kgsl.cc:51-95`）只带 `SAVE_GMEM | NO_GMEM_ALLOC | PREAMBLE`，不指定优先级，也不指定抢占方式。
  - KGSL 把优先级补成 MED（8，`adreno_drawctxt.c:477-485`）；level = 8/4 = 2，即 RB2（`adreno.h:1861`）。
  - flags 原样并入 HFI 注册（`adreno_gen8_hwsched_hfi.c:2684`），抢占方式为 DEFAULT/ANY。
  - 系统合成等 RB0/RB1 上的高优先级工作可以随时切出 RB2。
- **Turnip 不为上下文切换准备任何恢复内容**：
  - PREAMBLE/POSTAMBLE 都设为地址 0（`tu_cmd_buffer.cc:2565-2586`，`c5a0d6fd990`）。唯一会填充它们的是 autotune 抢占延迟统计，在 KGSL 上不可用。
  - BIN_PREAMBLE 只恢复静态寄存器、CCU、VPC 和 tile 寄存器（`tu_emit_bin_preamble`，`tu_cmd_buffer.cc:2381`），不含 draw-state 和计算状态。
  - 提交只用 `KGSL_CMDLIST_IB`，不用 `CTXTSWITCH_PREAMBLE`。
  - 结论：draw-state 和寄存器状态在切换后能否恢复，完全取决于 CP/DDE 固件。
- **snapshot 能看到的范围**：只转储每个 RB 一份 CP 上下文记录（GLOBAL 对象，每份 `0x24000` 字节，头部为 `struct gen8_cp_preemption_record`，`adreno_gen8.h:202`）。非 RB0 上下文另有 `user_ctxt_record`（`adreno_drawctxt.c:355-380`），不在 snapshot 里。
- **相关参考**：bug_reports 的 XRRuntime CTS 报告（`analysis/reports/xrruntime_cts_855_preempt_style_ringbuffer_ab_plan_20260721.md`，Adreno 840）。855.0.4 的 GL 合成器 context 被强制为 RINGBUFFER 抢占方式，切回后固定卡在 A2D resolve；清掉该位、改用 DEFAULT/ANY 后，780 秒不复现。那是另一种 workload 和抢占方式，但同样指向“Gen8 上 GMU/固件的 save/restore 在某些路径下不完整”。

### fault 53：同一次提交里，RB2 在计算屏障处被切出过

RB2 的上下文记录（`0x1ff8000fc2000`）：

| 字段 | 值 | 说明 |
|---|---|---|
| info | `0x80000091` | 低字节是切出点类型（按 SQE `IN_PREEMPT` 代码推断）：`0x11`/`0x91` 为 WFI、WAIT_FOR_ME 等同步点，`0x07` 为 bin 边界；Swan devcd15 相同 |
| rptr / wptr | `0x1b88` / `0x1bbf` | rptr 等于故障时 RB2 的 rptr，切出就发生在故障所在的这次 IB1 调用中 |
| `0xfadebac1` | IB1 `0x425f88b000`，27734 dword，保存偏移 4601 | 字段按内容推断 |
| `0xfadebac2` | IB2 `0x40051e4f00`，`0x2c` dword | 切出前最后调用的 IB2 |
| DDE 保存块 `0xfadebac9` | DDE_BR：`IB_LEVEL=0`、`0x1f2=1`；DDE_BV 相同 | 布局取自固件 `0x05f4` 的保存子程序；DDE 当时在顶层，不在任何 draw-state 组里 |

IB1 保存点附近，是 Turnip 在 dispatch 前发出的屏障：

```
4580 CP_INDIRECT_BUFFER 0x40051e4f00 x0x2c     计算状态
4584–4594 CP_EVENT_WRITE7 CCU_CLEAN_COLOR / CCU_CLEAN_DEPTH / CCU_INVALIDATE_COLOR /
          CCU_INVALIDATE_DEPTH / CACHE_CLEAN / CACHE_INVALIDATE7
4596 PKT4 0xab1f
4598 CP_EVENT_WRITE7 CCU_RESOLVE_CLEAN
4600 CP_WAIT_FOR_IDLE
4601 CP_WAIT_FOR_ME          <== 保存偏移
4602 CP_LOAD_STATE6_FRAG ...
4608 PKT4 0xa9e8（SP_CS_BINDLESS_BASE）
```

- **记录里保存的其它状态**：DDE 写队列（`0xfadebacd/0xfadebacf`）共 125 项，是计算状态（`SP_CS_*`，CS 基址 `0x41b3acb080`）和事件。DDE_BR 保存的 `$01=0x70a10002`（`CP_REG_RMW`）。
- **故障点与保存点的距离**：故障 IB2 在 IB1 dword 8212 被调用，即保存点之后又执行了 3611 个 IB1 dword、39 次 IB2 调用（约 20 个 render pass）。
  - 每个 RB 只有一份记录，每次切出都会覆盖，因此这是 RB2 在故障前的最后一次切出。若 CP 还把 L1 状态写进了未转储的 `user_ctxt_record`，不能完全排除之后还有切出。
  - 所以 fault 53 不是“恢复后立刻出错”，与 Swan trace 中恢复后 80–115 µs 即出错不同。
- **被卡住的包**：dword 814 的 33 dword、11 组 `CP_SET_DRAW_STATE` 与 `tu_CmdBindPipeline` 的形状一致（组 0–8、18，以及 26 = 动态 SAMPLE_LOCATIONS），只在 render pass 内第一次 draw 之后才会发出。DDE 出错时还在处理上一个 pipeline 留下的组（draw 806）。

### 18 份 CP opcode snapshot 的对照

数据来自 Swan devcd1–18（09-28 至 09-30，固件 `0x01510103`）和 Odin3 fault 52/53（`0x01500081`）。devcd19–28 不是这两类签名，不计入。

| 类 | 识别 | snapshot | 同一提交中途切出（记录 rptr 与故障 rptr 相差 ≤7 dword） |
|---|---|---|---|
| DDE 类 | DDE_BR 停在非法包陷阱（Swan `0x078f`、Odin3 `0x0717`），`$19=0xbad4` | devcd1/2/5/7/13/14/15/17、fault 53 | **8/9**（devcd1 例外） |
| BR 类 | BR 停在非法包陷阱（Swan `0x03cd`、Odin3 `0x0350`），`$19=0xbad4` | devcd3/4/6/9/10/12/16/18、fault 52 | **1/9**（devcd10） |

- **DDE 类的切出点都在 Turnip 屏障附近**：
  - devcd2/5/7/13/14：在 `CP_EVENT_WRITE7 → CP_WAIT_FOR_IDLE → CP_WAIT_FOR_ME` 之后的下一个 PKT4。
  - devcd15：在其后的 `CP_INDIRECT_BUFFER`。
  - devcd17：在 `CP_EVENT_WRITE7`。
  - fault 53：在 `CP_WAIT_FOR_ME`。
- **Odin3 fault 51 也有中途切出**：这次是计算状态丢失导致的 loading hang（已用 CS 重发规避），记录 info `0x80000007`，rptr `0x124b`，故障时 `0x1248`。`c5a0d6fd990` 的提交说明当时就推测 gen8.0 上下文切换不保留 `SP_CS_*`。
- **结论边界**：DDE 类与“同一提交中途 L1 切出”强相关，BR 类基本没有；这是统计关联，没有直接证明恢复路径出错。
- **BR 类不依赖抢占**：10-12 在 `preempt_level=0` 下出现 3 次 BR 类（片头保护写），记录里没有中途切出，与上表的 1/9 一致。
- **与计算状态丢失的关系**：计算寄存器丢失（fault 51）对应的是 bin 边界切出（info `0x07`），DDE 类对应的是同步点切出（`0x11`/`0x91`），是两种不同的切出点。计算状态一侧 10-12 已用对照实验证实是中途抢占造成（`preempt_level=0` 通过、`skipsaverestore=0` 仍挂死），DDE 类还没有做过同样的对照。计算状态那一侧的对照见 [计算状态 bug 的“寄存器是怎么丢的”](../odin3-kh3-compute-state-20261011/README.md#寄存器是怎么丢的l1-抢占2026-10-12)。

### 已排除的 Turnip 侧原因

- **4 GiB 回绕**：Turnip 注释（`tu_device.h:741-744`）说所有已知固件在抢占后重取 IB 时，若 IB 跨 4 GiB 边界，会取错内容。KGSL 的 `FORCE_32BIT` 在 Turnip 里被 32 位 flags 截掉，从未生效。
  - `92c96071dc5`（已含在 `c5a0d6fd990` 中）改为分配后校验并重试，覆盖所有 `NO_32B_ROLLOVER` 的 BO，包括 IB 和 pipeline/着色器子分配。
  - fault 53 的 MEMLIST 共 3183 项，只有 `0x40bf796000 +0x80000000` 一项跨 4 GiB，不含任何 IB、draw-state 或 CS 地址；30 个冻结对象均不跨。
  - 注意：当前子仓检出的 `fa9873b`（LRZ 修复线）不含 `92c96071dc5` 和计算状态重发；两条线合并前，不要从该检出出包给 Odin3。
- **pipeline 内存提前释放**：Turnip 对 pipeline 的 draw-state 内存没有 GPU 生命周期跟踪，`vkDestroyPipeline` 立即释放子分配（`tu_pipeline.cc:4484-4492`）。
  - shadPS4 运行中只在构建失败或着色器热重载时删除 pipeline（`vk_pipeline_cache.cpp:542,1053`），也不用 graphics pipeline library。
  - MEMLIST 显示那三块 pipeline BO 在故障时仍有映射。
- **draw-state 长度与内容不符**：预先定长的 draw-state 只由 assert 检查，release 构建若少写，尾部会是 0。
  - 但这种错误每次用到该 pipeline 都会出现，而不是偶发且与抢占相关。
  - 已冻结的组都能按长度完整解析；pipeline/着色器组由 `tu_cs_end_draw_state` 按实际长度封口。
  - 未冻结的组无法核对，此项不能完全排除。

### 固件 `0x01500081` 与 `0x01500103` 的 DDE 差异

- **新固件来源**：bug_reports `qualcomm_gpu_driver/packages/sm8850/v855.0.5-add-FW-change.zip` 中的 `gen80000_sqe.fw`，SHA-256 `5420d9cc…8d3e`，版本字 `0x01500103`。
  - 包属 sm8850，是否随 SM8750（Odin3）驱动发布未确认。
  - qrisc-disasm 无法解码新版（`unmatched instruction: 0xb0628000`），只能按原始指令字对齐比较 DDE 段（本机 `build/validation/kh3-dde-20261012/dde_diff.txt`，不入库）。BR（SQE）段未比较。
- **旧版 DDE 的抢占保存/恢复子程序**：
  - 保存（`0x05f4`）：`PREEMPT_COOKIE=0x1e`，写标记 `0xfadebac9`，依次存 `0xc9–0xcb`、`0xc7`、`0xc6`、`APERTURE_CNTL`、`0x190`、`IB_LEVEL`、`0x011`、`0x1f2`。
  - 恢复（`0x061c`）：`PREEMPT_COOKIE=0x62`，标记不符则跳到 `0xbad9` 停机（fault 53 是 `0xbad4`，不是这里）；之后按序写回。
- **改动 1：控制寄存器 `0x130` 加入保存与恢复**：
  - 新版在保存和恢复中各多一项 `0x130`。
  - 旧版 `0x130` 只在初始化时写 1（`0x0024`）。新版在每处写 `0x1f2` 的地方同时写 `0x130`：`CP_EXEC_CS`（旧 `0x0213`）、旧 `0x00e2`、`0x0173`、`0x0301` 以及抢占处理尾部。
  - `0x130` 紧挨 `CONTEXT_SWITCH_CNTL`（`0x131`）、`PREEMPT_COOKIE`、`PREEMPT_ENABLE`，名称未知。
  - 这项新增的保存/恢复也可能只是配合新版对 `0x130` 的动态使用，单凭它不能证明旧版恢复会丢状态。
- **改动 2：`CP_EXEC_CS_INDIRECT`（shadPS4 的间接 dispatch 走这条路径）**：
  - NDRANGE 全局尺寸的乘法从 `mul16` 改为调用 32 位乘法子程序（旧 `0x0242/0x0246/0x024a`）。
  - 组数为 0 时（旧 `0x0252`），新版先写 `SP_CS_NDRANGE_*`、`SP_CS_KERNEL_GROUP_*` 再跳过；旧版直接跳过。
- **改动 3：其它**：
  - 一批控制/scratch 寄存器偏移后移（如 `0x44b→0x46a`、`0x451→0x46f`、`0x453→0x471`）。
  - 陷阱入口前新增一段读 `0x13a` 的子程序。
  - 新增一段写 PC 寄存器 `0x9e15` 的处理代码。
- **结合用户说明**：855.0.x 固件更新后这类故障大幅减少。新版恰好改动了 DDE 的抢占保存/恢复和计算 dispatch 路径，与上面的抢占关联方向一致，但还没有用新固件在 Odin3 上验证。

### 推断的机制（未证实）

L1 中途切出后，DDE 或 BR↔DDE 之间的某个状态没有被正确恢复（候选：`0x1f2/0x130` 一类的 dispatch/切换状态，或 BR 等待循环所读 `0x169` 中 BR 与 DDE 之间的 2 位计数）。这个不一致不一定立刻暴露，要等 DDE 某次从内部队列或组载荷取包时读到尚未写入的位置（0），进入 `0xbad4` 陷阱。fault 51 的 `SP_CS_*` 丢失可能是同一类“恢复不完整”在计算状态上的表现。

能说明这一点的证据，以及目前缺的部分：

| 方面 | 现有证据 | 缺口 |
|---|---|---|
| 与抢占的关联 | DDE 类 8/9 有同一提交中途切出；新版固件改动的正是 DDE 的保存/恢复 | 只是统计关联 |
| 时间关系 | Swan trace：恢复后 80–115 µs 出错 | fault 53 距最后一次切出约 20 个 render pass，且没有同窗 trace |
| 状态是否真的丢失 | — | 状态寄存器含义无文档，旧固件的 `0x130` 是否承载需要保存的状态也无法确认 |

### 可做的对照（都需要用户同意；KGSL 项会影响整机）

| 对照 | 方式 | 说明 |
|---|---|---|
| 关闭 L1 | Odin3 debugfs `preemption/preempt_level` 写 0 | 只保留 L0（IB 边界切换）；hwsched 上会让 GMU 重新上电，系统合成延迟可能变差 |
| 保留 L1，不跳过保存/恢复 | debugfs `preemption/skipsaverestore` 写 0 | 直接检验“跳过 save/restore”这一项 |
| 关闭抢占 | sysfs `/sys/class/kgsl/kgsl-3d0/preemption` 写 0 | 作用最大，作为上限对照 |
| 只影响游戏 | `debug.mesa.tu.debug` 加 `cmd_no_preempt` | 主命令缓冲包在 `CP_SCOPE_CNTL(disable_preemption)` 内，只在 IB 边界切换；Swan 上曾各跑 12–13 分钟，未能区分 |
| 换固件 | 部署 `0x01500103` | 需改 `/vendor`，风险最高，且需先确认该固件适用于 SM8750 |
| 补证据 | DeviceLost 时由 Turnip 转储 draw 806 / SDS 814 引用的 pipeline BO；同窗 KGSL 抢占 ftrace | 不改变行为 |

每项都需要足够长的 KH3 会话（fault 53 在约 19 分钟出现），并在每次故障后立即一次性读取 snapshot。
