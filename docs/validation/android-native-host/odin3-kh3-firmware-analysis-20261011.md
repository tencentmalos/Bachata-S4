# Odin3 KH3：精确 SQE 固件与 snapshot 交叉分析（2026-10-11）

## 概要

| 项 | 案发值与来源 |
|---|---|
| Spider | not present |
| Meego | not present |
| Slardar OS | not present |
| 设备 | Odin3 / `6edb0593`，snapshot chip `0x44050000`；设备 SoC 属性报 `CQ8725S` |
| ROM | `qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys`；10-11 读取，boot ID 与案发相同 |
| boot ID | `c322cd23-e6c8-46f8-8eb7-ff032d7162db` |
| 触发 | 用户报告王国之心 loading 完成后按确定；后续复测在到达相同步骤前也出现保护写错误，不能合并两种故障 |
| 原始 hang | 10-10 23:02:06 CST，kernel uptime `125329.385681`；PID 5490 / context 14 / ts 32414 |
| 保护写对照 | 10-10 23:29:24 CST，kernel uptime `126967.225152`；PID 15281 / context 14 / ts 5528 |
| SPR | unknown (source missing)，本次为 Odin3 2D 游戏会话，未用于归因 |
| XRRuntime | unknown (source missing)，未用于归因 |
| 出错进程 | `com.shadps4.android`；原始 PID 5490，保护写 PID 15281；案发 APK versionName/versionCode unknown (source missing)，不能用后来安装的 APK 回填 |
| KMD | 保存的 `msm_kgsl.ko` Build-ID `8ad43a33afb6affe54edeb6804aa14089dcfe7ce`，精确源码映射仍 unknown |
| UMD | 原始日志 Turnip `351a4847a0`；保护写时 `2b8ed3fc20`，SHA `ba53ae858c03dc7a0760d4cf6a81d6f32cca519514b8fb0900a5151d734c19b2`；见前日记录与各自 host/logcat |

延续 [前日现场记录](odin3-kh3-gpu-hang-20261010.md)。本轮只进行离线分析，以及用户授权的设备固件读取；没有启动游戏、切换驱动、强停进程或重启 adbd。

使用 bug_reports 的 `qualcomm-kgsl-snapshot-analysis`、`qualcomm-snapshot-firmware-correlation`、`gpu-issue-quick-triage` 与 Mesa IR3 方法。原始输入未修改。

## KGSL 错误证据

### 检索覆盖

证据根目录：`build/validation/odin3-kh3-gpu-hang-20261010/`，新增结果在 `firmware-20261011/`。扫描已有全部 4 份 dmesg，完整 57 个命中保存在 `kgsl-hits.txt`，pattern、各文件行数和 uptime 范围在 `kgsl-coverage.json`。检索包括 hang、Fault id、GMU/HFI timeout/reset、SMMU/IOMMU fault、protected mode、ring/IB、waittimestamp 和未触发 fence。

- 原始 `dmesg.txt`：uptime `121162.276996–125648.268801`，涵盖原始 hang 前超过 60 s、后超过 120 s。
- `current-driver/repro3/dmesg.txt`：`122429.596910–126972.127648`，保护写错误后只覆盖约 4.9 s；不能把后续恢复过程当作已完整覆盖。
- 未另取得案发 pstore/last_kmsg；Android logcat 仅补充启动/退出时间，不能代替 KMD。
- 原始同窗未发现 SMMU/IOMMU 页故障；未报告的 fault VA、read/write access、FAULTING BLOCK、FSR/ESR 为 unknown，不能据此排除所有资源访问问题。原始 ptbase `0x9a2ac9000`，对照 `0xb16402000`；MEMLIST 存在并不证明内容有效。
- Source 参考为本机 `swan_source/oak_vendor/vendor/qcom/opensource/graphics-kernel` 的 Gen8.0 表；不是与该 KMD Build-ID 已匹配的源码。`Fault id:2` 不扩写成未核实枚举。

### 原始错误与因果角色

来源 `dmesg.txt:14961–14967`，23:02:06 CST / uptime 125329.385681：

```text
[125329.385681] [ T1988] adreno-gen8-gmu 3d37000.qcom,gmu: MISC: GPU hang detected
[125329.385697] [ T1116] kgsl kgsl-3d0: Fault id:2 and GX is ON
[125329.385701] [ T1116] kgsl kgsl-3d0: shadps4.android[5490]: ctx 14 ctx_type ANY ts 32414 policy 2
[125329.385703] [ T1116] kgsl kgsl-3d0: shadps4.android[5490]: cmdline: com.shadps4.android
[125329.385732] [ T1116] kgsl kgsl-3d0: status 00EC2405 gfx_status 00E80004 gfx_br_status 00E80004 gfx_bv_status 00880004
[125329.385734] [ T1116] kgsl kgsl-3d0: BR: rb 159d/15d4 ib1 00000041BAA5B000/55da ib2 00000041402BA980/0000 ib3 0000000000000000/0000
[125329.385736] [ T1116] kgsl kgsl-3d0: BV: rb 159d/15d4 ib1 0000004038828000/0000 ib2 0000000000000000/0000 ib3 0000000000000000/0000
```

这是最早已记录的原始 incident detector；之前 GPU 何时停止进展没有连续采样。KMD 报告的上下文为 14，归属 PID 5490。`ErrorDeviceLost` 在 23:02:08.446 出现，是后续传播。queued/consumed/retired `32419/32414/32413` 表明有已消费、未退休的工作，不证明某个 shader 首先出错。

来源 `current-driver/repro3/dmesg.txt:15982–15988`，23:29:24 CST / uptime 126967.225152：

```text
[126967.225152] [ T1988] adreno-gen8-gmu 3d37000.qcom,gmu: CP | Protected mode error | WRITE | addr=0x00000 | status=0x00600000
[126967.225166] [ T1116] kgsl kgsl-3d0: Fault id:2 and GX is ON
[126967.225171] [ T1116] kgsl kgsl-3d0: shadps4.android[15281]: ctx 14 ctx_type ANY ts 5528 policy 2
[126967.225174] [ T1116] kgsl kgsl-3d0: shadps4.android[15281]: cmdline: com.shadps4.android
[126967.225219] [ T1116] kgsl kgsl-3d0: status 00880005 gfx_status 00880004 gfx_br_status 00880004 gfx_bv_status 00880004
[126967.225220] [ T1116] kgsl kgsl-3d0: BR: rb 02c0/0368 ib1 00000041861BE000/36da ib2 000000418E4D6E88/0000 ib3 0000000000000000/0000
[126967.225223] [ T1116] kgsl kgsl-3d0: BV: rb 02c0/0368 ib1 00000041861BE000/02da ib2 0001FF8002A36190/0000 ib3 0000000000000000/0000
```

这是独立 incident 的 CP 寄存器保护写错误，地址 0 不是已证明的 GPU 内存 VA。q/c/r 为 `5529/5528/5527`。本轮发现其 SQE 已在错误处理过程中，详见后文。

同 boot 还有 23:19:56、23:23:47、23:25:20 的不同 occurrence，完整命中保留；不拿冻结的旧 snapshot 解释新错误。23:31:14 的外部强停是后续 recorder-off 对照受干扰的原因，不能回投为 23:29:24 的直接触发。

### 保护写 snapshot 与 mesh 测试的区别

文件为 `current-driver/repro3/kgsl-snapshot.bin`，SHA `1104a0ab9b970c5fa1c9b351a370e2ee1e08da36ffe52f33ce24f7c33bdc55f7`。

`repro3/logcat.txt:12888` 记录 23:28:40.562 启动 PID 15281，`:13121` 记录 23:28:42.029 进入 `CUSA15072`；`repro3/android-host.log:1647` 记录加载同一 title 的 pipeline cache。snapshot OS TID 15352 对应此会话 Guest-1，kernel 明确绑定 shadps4 context。因此不是把一个直接属于 mesh CTS context 的 snapshot 误认为 KH3。**仍未证明这一分钟内完全没有其他 GPU 测试，也不能排除跨进程/驱动恢复干扰**；本轮没有读取或干预隔壁任务。

## 固件身份与永久归档

原始文件按完整 SHA 放在 bug_reports 的 `qualcomm_gpu_driver/firmware/odin3-gen8-0/<filename>/<sha>/raw/`，已同步 `catalog.json`，每份 `identity.json` 记录设备路径、ROM、boot、设备端与本地 checksum、反汇编工具与结果散列。归档配置 Git LFS；仅落盘，未提交或推送。

| 文件 | 字节数 | SHA-256 |
|---|---:|---|
| `gen80000_sqe.fw` | 111892 | `9c8f009bea112d7c06dd2ccb67e889999459e53b92f0f4859f5ea0db067ad6b5` |
| `gen80000_aqe.fw` | 25340 | `f04cb97e1e629b9003a26c5670b8f7e5b7bfeca95d35b2abd581fd8672efed20` |
| `gen80000_gmu.bin` | 88896 | `1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5` |

SQE 五个版本字 `01500081 / 01006d44 / 018bc081 / 01003238 / 0152eb01` 全部匹配；AQE `01500022` 匹配。GMU snapshot 为 core `50010019` / power `50000112` / HFI `20070001`。

版本之外的字节核实：

- SQE image PC `[0x749,0x74e)` 的 20 字节、`[0x8c7,0x8d0)` 的 36 字节，在原始 snapshot 的 BR 指令缓存中逐字节找到，debug index 分别 `0xb49`、`0x8c7`。缓存下标不是 PC，也不能把整块 mutable UCODE hash 当作 firmware hash。
- GMU container 中加载到 `0x4000` 的 **88100 字节**与 snapshot section 15 全部相同，range SHA `8a007071374b98ce1011eaf795084d32570db4b51e8dd55c45eaaa9114203c92`；`0x0` 的 492 字节也相同。`0x10007fdc` 是已变化的数据范围，不宣称整个运行内存等同原始文件。详见 `gmu-blocks-match.json`。
- QRisc 反汇编工具 SHA `6cc2d5f2d9c25e43cd97b77c982812ea47152dfd6f0d29e6b77ccd275a283d34`；工具 catalog 标记精确构建 commit 未证明，源码 checkout candidate `c1c734d006f3c579978312850e834142322ba5ea`。SQE/BV/LPAC/DDE 分 image 解码；AQE 仅归档 raw 反汇编。

Gen8.2 skill 的计数提取器按设计拒绝本案：Odin3 UCODE block 是 `0x8000`，不是 `0xa000`。保持数据库 `selector_known_shape_mismatch`，未伪造 exact 状态。独立对照 Gen8.0 snapshot 源表的 selector `0x8f8/0x8f9`、长度 `0x8000`，再用本机实际固件控制寄存器访问核实语义。所有 pipe 的原始值及门控保存在 `sqe-control-comparison.json`，不相加 BR 与 DDE 的计数。

## 原始 hang：等待点已定位，取指尾部不是执行点

BR SQE PC `0x74a`，当前 packet `0x70138000`（`CP_WAIT_FOR_ME`），`$05=5`，返回地址候选 `0x8cc`，与下面实际固件吻合：

```text
0749: b80500b2  cread $05, [$00 + WFI_PEND_CTR]
074a: c0a00000  brne $05, 0, 074a
074b: b80500b2  cread $05, [$00 + WFI_PEND_CTR]  ; branch delay slot
...
08c7:           CP_WAIT_FOR_ME
08c8: a80500c9  set PM4_WFI_STALL
08c9: a80000b0  increment WFI pending
08ca: d4000749  call 0749
08cb: 731d0081  WFI_PEND_DECR to pipe             ; call delay slot
08cc:           return continuation
```

原始 BR `WFI=5 / QUERY=0 / CACHE_CLEAN=1`，`PERFCNTR_CNTL=0x08000008` 的 PM4 WFI stall 位与 handler 相符。DDE_BR PC `0x1ea` 在 `CP_REG_RMW` 之后的 `waitin`，不是卡在执行 shader 的固件循环；BV PC `0x185` 在双方同步轮询。这些是后端工作未完成的等待链，不能据此命名首先停下来的硬件块。

`0x7008` 原始控制值 `0x1497=5271`；固件 BAD4 恢复代码确实以 `0x005/0x006/0x007/0x008 + (IB level−1)*4` 读出 IB base/size/已消费 offset。当前 header、调用返回点和 IB1 字节共同把等待对应到 **IB1 dword 5270 / VA `0x41baa60258`**。这比先前按 MVC/ROQ 观察到的预取 5504 dword 更精确。

| IB1 dword | 内容 | 边界 |
|---:|---|---|
| 5031 | draw IB `0x41baa9ff7c`，269 dwords | 捕获、严格解码；RectList 辅助 HS/DS 仍是该段图形状态，不能当作 fault shader |
| 5039 | WFI | 后端完成事件，SQE 可以继续发包 |
| 5084 | `EXEC_CS 104×1×1` | 已被 SQE 发出；SP NDRange 的 `0xff` 与此 local size 相符 |
| 5093 / 5160 / 5190 | WFI；其间是 A2D 拷贝 | 待进一步区分最先不完成的 work/cache 操作 |
| 5249 | compute pipeline state IB `0x41402ba980` | 绑定状态不等于执行 dispatch |
| 5261 | CACHE_CLEAN | 与 pending clean=1 相容，不能仅用计数证明其为唯一首因 |
| 5269 | WFI | 后端事件 |
| **5270** | **WAIT_FOR_ME** | **SQE 实际停在这里** |
| 5294、5357、5395、5433、5465 | 后续 compute dispatch | 此时尚未由 BR SQE 执行，排除其作为本次原始 hang 的直接触发命令 |

五个 WFI token 与后段四次 WFI 加本次 WAIT_FOR_ME 的 token 相容。但固件还可能在其他 handler 内发 token，snapshot 各 section 不是原子采样；**不把这种计数配对写成已证明 5039 必然完成或 5084 必然是首因**。

`104×1×1` 的 CS 候选为之前 IB1 dword1697 绑定的 `0x400609bc00`：已检查的命令中未见其到该 dispatch 之间重绑，SP MVC 的 base 也相同，但没有执行 wave 证据完成最终归属。其 captured program state 给 `CONFIG=0x105 / INSTR_SIZE=2`；256 字节 IR3 SHA `69143b64102a528ef32c84545ce4a2a7e51dc99437d75f6a682625efc4f5fdda`。解码显示短小的有界 buffer 写入：计算 invocation index、与常量长度比较、满足条件执行 store，21 号指令 end；没有后跳循环或线程组 barrier。不能据此排除 descriptor/地址或硬件指令问题。

需继续核实的异常：SP MVC 的 CS base 仍是该值，但 CONFIG/INSTR_SIZE 为 0，与 program state 的 `0x105/2` 不一致。由于目前缺执行 wave 和目标机私有状态解释，不能判断这是实际管线状态丢失、读回时不同阶段的状态，还是状态采样限制，更不能直接据此打“强制重绑”修复。

## 保护写对照：BAD4 路径已在重读 IB

该份 SQE PC `0x362`，`$19=0xbad4`、`$03=$rem=0xa9`，`$04/$05=0x418e4d6e88`、`$0b=0x130`。WFI/QUERY/CACHE_CLEAN 均 0，明显不同于原始 hang。

精确固件 `0x333–0x362`：保存现场 → 检查 BAD4 → 读取 IB level/base/size/offset → 重新写入该 IB → 设置 `$rem=offset−1` → `(rep)mov $00,$data` 重读并丢弃此前 dword。此次 rem=169，对应 IB2 中大块 `CP_CONTEXT_REG_BUNCH`（dword38、130 payload dwords）之后的边界；dword169 是 `PKT4 REG_0x808e`。

因此 `IB2 remaining=0` 和 ROQ 内的 305 dwords 不能视为该 draw 已执行完成的证据；SQE 正在恢复过程中重新获取 IB。dword177/205 的 draw-state 和296的 draw 排在该边界之后。此前强调“缺失三个 program draw-state 块”仍是捕获缺口，但**不是据此锁定它们导致 addr=0 的证据**。

逐字节核对 dword38 的 65 对 register/value，未发现 register 0，包头 `0x70dc8082` 与后续 `0x40808e02` 严格有效。这只能说明落入 snapshot 的内存副本是合法包，不能证明 GPU 首次获取时内容相同，也不能把 BAD4 这个私有标签直接翻译成某个未核实的硬件 fault 类别。

## 当前结论与下一次最小验证

**还未确定根因，没有提交 GPU 行为补丁。** 此轮确定了精确固件、真实 SQE 等待位置，并纠正了把预取尾部/恢复状态当作执行位置的风险。

原始 hang 的下一步优先检查 `104×1×1` dispatch 到 dword5270 之间的管线状态、buffer 访问和 cache 完成；后面的 detile dispatch 已降出直接嫌疑范围。保护写单独沿 CRB/恢复边界分析，不与原始 hang 合并成一个 shader 问题。

后续若恢复独占设备复测：先取得带提交/命令 ordinal 的 Vulkan 录制及 pipeline/descriptor 身份，围绕上述小段做有序完成检查；对 CS 重绑仅作诊断 A/B，再检查 bind-skip、recording 和资源生命周期。不要直接移除 WAIT_FOR_ME/cache flush，也不要把未受控的 CTS 并行结果当作修复验证。

本轮固件读取时三个文件本来即可由 adb shell 读取；`su` 不存在，但无需重启 adbd 获取 root。没有为此更改设备权限或服务状态。
