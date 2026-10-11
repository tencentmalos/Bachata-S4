# Odin3 KH3：`CP DDE BR opcode=0` 闪退（fault 53）与启动期保护写（fault 52）

**状态：未修复，根因未定位。** 本文汇总现场与已能确认的事实，供在有 bug_reports、固件反汇编工具和设备的环境中继续确认。

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

draw-state 载荷（`0x4331…`、`0x4314…`、`0x40051c7…`）都不在 snapshot 冻结的 GPU 内存里（只冻结了 7424 KiB），无法核对 DDE 实际读到的内容。

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

## 与已有记录的关系

- Swan 上未解决的 B 类：BR 停在 `CP_SET_DRAW_STATE` 等 DDE，DDE 读到全零包头（固件 PC 映射到非法包陷阱 `$19=0xbad4`），三次有 trace 的故障都在 RB2 从 level=1 抢占恢复后 80–115 µs 内。见 [Swan 固件报告](../../docs/validation/android-native-host/swan-cp-opcode-firmware-20260930.md)、[Swan CP 深挖](../../docs/validation/android-native-host/swan-cp-opcode-20260929.md)。
- Odin3 10-08 曾记录一次 `CP DDE BR opcode=0`（见仓库根 `AGENTS.md` 的 10-09 条目）。
- 修改前后都会出现，不是 `PoolCommit` 修改或计算状态重发修复引入的。用户说明：高通驱动约 855.0.9 前后固件一同更新后这类问题大幅减少；Odin3 当前为 `V@0800.72`。

## 待确认（需要相应环境）

1. **固件 PC**：用该 snapshot 的 UCODE 块和精确 SQE/DDE 固件反汇编，把 BR/DDE_BR 的 PC 与寄存器映射到处理函数，确认是否走非法包陷阱、DDE 是否读到全零包头。方法见固件分析文档；上一次的反汇编证据在另一台机器的 `build/validation/odin3-kh3-gpu-hang-20261010/firmware-20261011/`。
2. **bug_reports 对照**：查 gen8.0 固件在上下文切换/抢占恢复时 DDE 读 draw-state 出错的记录，以及哪一版固件修复；判断 Odin3 能否换固件验证。
3. **抢占关联**：同窗采集 KGSL 抢占 ftrace（`swan-kgsl-debugger` 技能，独立 tracefs instance），看故障是否紧跟抢占恢复；可做 `debug.mesa.tu.debug` 加 `cmd_no_preempt` 的开/关对照（Swan 上各 12–13 分钟未能区分）。
4. **draw-state 内容**：KGSL snapshot 不冻结这些 BO。可用 `TU_DEBUG=rd` 记录提交及其 BO（开销大，只在复现窗口短时开启），或对照 `sds_page_align`，确认 DDE 读到 0 时载荷本身是否完好。
5. **复现**：KH3 新游戏进入“觉醒之所”后正常游玩；fault 53 出现在会话约 19 分钟、进入新区域时，fault 52 出现在片头。没有稳定复现步骤，每次需要 root 立即读取 snapshot（读一次即清空）。

## 重新分析

- snapshot：gpu-snapshot MCP `gpu.snapshot_analyze`（`snapshotPath` 指向 `evidence/fault53/snapshot.bin`），数据库可重建。已发布的 MCP 解析该机 snapshot 会报 `UNIQUE constraint failed: program_candidate_mappings`；本机开发树 `my_mcp_tools/dev_tools/mcp/mcp_service_layer/Features/GpuSnapshot/GpuSnapshotDatabase.cs` 的 `FindMemoryIndex` 已改为先按引用身份查找（未提交）。
- IB2 解码：`gpu.pm4_decode`，`inputPath` 为 snapshot，`byteOffset` 5030448，`dwordCount` 2118，`baseGpuAddress` `0x426ba05644`。

## 文件

| 文件 | 说明 |
|---|---|
| `evidence/dmesg-kgsl-faults.txt` | fault 52、53 的 KGSL 记录及前后上下文（行号对应 `fault53/dmesg.txt`） |
| `evidence/fault53/snapshot.bin` | KGSL snapshot |
| `evidence/fault53/dmesg.txt` | 完整 dmesg（同时含 fault 52） |
| `evidence/fault53/android-host.log` | 宿主日志（20:41 会话启动至崩溃） |
| `evidence/fault53/logcat-crash.txt`、`logcat-main-tail.txt` | crash buffer 与主 buffer 尾部 |
| `evidence/fault53/analyze.json`、`snapshot-review.md` | gpu-snapshot 解析结果与自动 review |
| `evidence/fault53/ib2-0x426ba05644-decoded.txt` | BR IB2 全量解码 |
| `evidence/fault53/*.txt`（boot_id、date、uptime、faultcount、snapshot-timestamp-*） | 读取身份 |
| `evidence/fault52/snapshot.bin`、`android-host.log`、身份文件 | fault 52 现场 |

更多上下文：[性能剖析与闪退记录](../../docs/validation/android-native-host/odin3-kh3-performance-20261011.md)。
