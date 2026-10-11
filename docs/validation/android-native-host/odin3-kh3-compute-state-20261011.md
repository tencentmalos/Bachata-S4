# Odin3 KH3 loading GPU hang：A8XX 计算管线寄存器丢失与 Turnip 修复（2026-10-11）

延续 [固件交叉分析](odin3-kh3-firmware-analysis-20261011.md) 与 [修复实验](odin3-kh3-fix-experiments-20261011.md)。前两份确定了 SQE 停在 `CP_WAIT_FOR_ME`、但没能定到首先不完成的工作；本轮定位到具体 dispatch，找到原因并在 Turnip 中修复，修复版本在 Odin3 上通过 loading。

## 概要

| 项 | 值 |
|---|---|
| 设备 | AYN Odin3 `6edb0593`，Adreno 830，chip `0x44050000`（gen8.0），boot ID `c322cd23-e6c8-46f8-8eb7-ff032d7162db` |
| ROM | `qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys` |
| 系统 GPU 驱动 | `OpenGL ES 3.2 V@0800.72 (GIT@3967b80d3b, Ica4ce9bea6, 1770914544) (Date:02/12/26)`；固件随该驱动版本（SQE 版本字见固件分析文档） |
| KMD | `msm_kgsl.ko` Build-ID `8ad43a33afb6affe54edeb6804aa14089dcfe7ce`，源码映射未确认 |
| 游戏 | Kingdom Hearts III `CUSA15072`，标题 Ver 1.10 |
| 现象 | 新游戏确认后的 loading 中 GPU hang（`MISC: GPU hang detected`，status `00EC2405`），随后 DeviceLost |
| 根因（判断） | 计算管线绑定后，中间录入 render pass、blit、LRZ 清除，再次 dispatch 时 SP 的 CS 配置寄存器已为 0（`SP_CS_BASE` 保留），该 dispatch 永不完成。推断为 gen8.0 固件在上下文切换时未保留这些寄存器 |
| 修复 | Turnip `2d96f4bc287`：A8XX 上若上次发出计算状态之后又录入了其它命令，dispatch 前重发 CS shader 状态与 load state；`c5a0d6fd990` 加 `TU_DEBUG=nocsreemit` 关闭开关、一次性日志与注释 |
| 验证 | 修复前同流程 loading 开始约 52 s 必挂（本轮 fault 51，此前 fault 42/43/45/46/47 同一位置）；修复后默认配置 loading 通过，过场动画正常，240 s 以上无新 fault |
| 待补 | bug_reports 中关于 gen8.0 固件上下文切换问题的记录（用户说明：约在高通驱动 855.0.9 前后固件一同更新，此类问题大幅减少），见文末“待补充” |

## 1. 关闭 concurrent binning 的对照：仍挂死（fault 51）

先排除 BV/BR 并发分箱：`debug.mesa.tu.debug=nocb`，宿主日志确认 `Turnip TU_DEBUG=nocb,gmem_warmup`。

| 项 | 值 |
|---|---|
| APK / 驱动 | APK `d70cbac9f246bc9e…`，Turnip `c22932ba06470e585373db12095153b8d4ab2e27b9d73319705f8dbc742fcff0`（`92c96071dc5` 范围检查版） |
| 会话 | PID 12286，generation 1，run UUID `103ecfdd61b4b72982ca062621a37cf5` |
| 过程 | 片头、标题、难度、确认均正常（60 FPS，字体完整）；19:31:30 进入 loading，19:32:22 faultcount 50→51 |

`nocb-fault51/dmesg-plain.txt`（kernel uptime）：

```text
[166081.901928] adreno-gen8-gmu 3d37000.qcom,gmu: MISC: GPU hang detected
[166081.902003] kgsl kgsl-3d0: Fault id:2 and GX is ON
[166081.902060] kgsl kgsl-3d0: shadps4.android[12286]: ctx 21 ctx_type ANY ts 104988 policy 2
[166081.902117] kgsl kgsl-3d0: status 00EC2405 gfx_status 00E80004 gfx_br_status 00E80004 gfx_bv_status 00880004
[166081.902128] kgsl kgsl-3d0: BR: rb 124b/12f3 ib1 000000418E052000/6a71 ib2 0000004032891B80/0000 ib3 0000000000000000/0000
[166081.902143] kgsl kgsl-3d0: BV: rb 124b/12f3 ib1 00000041DFDFE000/02b6 ib2 0000000000000000/0000 ib3 0000000000000000/0000
```

签名与 fault 46/47 相同；`nocb` 不是修复。按 A8XX `RBBM_STATUS` 位定义，`00EC2405` 为 CP、UCHE、UFC 及 slice 忙，BV 侧 slice 不忙。宿主随后在 `Presenter::PrepareLastFrame` 断言退出（DeviceLost 后的下游，按此前决定不修）。

snapshot 用 root 一次性读取：15,844,268 字节，SHA-256 `4db25a0aee867b7c7f75226abf2b7ec55058c87685cdb231e2475a1a5409b791`，END 完整，读取前 sysfs timestamp 286269、读取后 0，faultcount 51。

### snapshot 工具的唯一约束错误

已安装的 gpu-snapshot MCP 解析该机 snapshot 仍报 `UNIQUE constraint failed: program_candidate_mappings`。原因是 `GpuSnapshotDatabase.FindMemoryIndex` 先做值相等比较，MEMLIST 中值完全相同的多条记录都会落到第一条。本机开发树 `my_mcp_tools/dev_tools/mcp/mcp_service_layer/Features/GpuSnapshot/GpuSnapshotDatabase.cs` 改为先按引用身份查找、再按值回退（与另一台机器上已做的修改一致），Debug 构建后用本地 stdio 客户端离线分析，0 警告 0 错误。随后提交到 my_mcp_tools `master` `f309a13`，附回归测试（旧代码上报同样的唯一键错误）；已安装的发布包需重新构建才包含该修复。

## 2. 挂死点：IB1 dword 5270，未完成的是 dword 5084 的 dispatch

`v_live_command_cursors`：BR IB1 `0x418e052000` 共 32753 dword、剩余 27249（已取 5504）；IB2 `0x4032891b80` 44 dword、剩余 0；BV IB1 `0x41dfdfe000`。fault 46、47 的 IB1 同样是已取 5504 dword，三次挂在同一命令序列。IB1 有界解码（`nocb-fault51/ib1-dw0-5400-decoded.txt`）：

| IB1 dword | 内容 |
|---:|---|
| 1697 | `CP_INDIRECT_BUFFER 0x40055a0a80`（44 dword，计算管线状态） |
| 1728 | `CP_EXEC_CS 7168×1×1`（该管线第一次 dispatch） |
| 1733–5039 | 约 23 个 sysmem render pass（`RM6_DIRECT_RENDER`）、1 个 GMEM pass、1 次 A2D blit（2751）、多次 LRZ 清除；没有其它 dispatch，也没有重新绑定计算管线 |
| 5031 | draw IB `0x41dcc32b8c`（269 dword） |
| 5039 | `CP_WAIT_FOR_IDLE` |
| 5057–5070 | 计算常量 IB、`CP_LOAD_STATE6`（CS UBO）、`SP_CS_BINDLESS_BASE`、`SP_UPDATE_CNTL=0` |
| 5084 | `CP_EXEC_CS 104×1×1`（同一管线，未重新绑定） |
| 5089–5093 | CCU clean/invalidate，`CP_WAIT_FOR_IDLE` |
| 5094–5214 | 四次 A2D 缓冲拷贝（`FMT6_32_UINT`），其间 WFI 5160、5190 |
| 5218–5247 | A2D 拷贝 `FMT6_32_32_32_32_UINT` 2816×1，源 `0x41b441b000`，目的 `0x406347f000` |
| 5249 | `CP_INDIRECT_BUFFER 0x4032891b80`（下一个计算管线的状态） |
| 5253–5267 | CCU/CACHE clean 与 invalidate、`CCU_RESOLVE_CLEAN` |
| 5269 | `CP_WAIT_FOR_IDLE` |
| **5270** | **`CP_WAIT_FOR_ME`，SQE 停在这里** |
| 5294 | 下一次 dispatch `4×2×8`，尚未执行 |

固件分析已确认 SQE 在 `CP_WAIT_FOR_ME` 循环读 `WFI_PEND_CTR`，pending 为 5；`CP_WAIT_FOR_ME` 自身会再加 1 个 token。5 个 token 对应 5093、5160、5190、5269 与 5270 自身，即 5039 的 WFI 已完成，**5039 与 5093 之间的工作没有完成：5084 的 `EXEC_CS 104×1×1`**。fault 47 在 5247 后多加了一个 WFI，pending 正好变为 6，与此一致。前几轮把怀疑放在 5247 的 A2D 拷贝上，A2D 发生在该 dispatch 之后，不是首个未完成工作。

## 3. 该 dispatch 执行时 SP 的 CS 配置寄存器为 0

MVC（cluster 7、BR pipe，两个 hardware context、三个 slice 取值一致）中 SP 的 CS 寄存器（`nocb-fault51/mvc-br-sp-cs-registers.txt`），与 dword 1697 绑定的管线状态 IB（snapshot 中 `0x4005597000` 对象偏移 `0x9a80`，原始字见 `cs-state-ib-0x40055a0a80-raw.txt`）对比：

| 寄存器 | 管线状态写入值 | 挂死时 SP 值 |
|---|---|---|
| `SP_CS_CNTL_0` 0xa9b0 | `0x80104080` | 0 |
| `SP_CS_CNTL_1` 0xa9b1 | 1 | 0 |
| `SP_CS_BASE` 0xa9b4/b5 | `0x40055a0980` | `0x40055a0980` |
| `SP_CS_PVT_MEM_SIZE` 0xa9b9 | `0x80000000` | 0 |
| `SP_CS_CONFIG` 0xa9bb | `0x105` | 0 |
| `SP_CS_INSTR_SIZE` 0xa9bc | 2 | 0 |
| `SP_CS_CONST_CONFIG` 0xa9cd | `0x304` | 0 |
| 0xa9db | `0x44400afc` | 0 |
| `SP_CS_NDRANGE_0/1/…` 与 `KERNEL_GROUP_X/Y/Z` | dispatch 时写入 | `0xff/0x1a00/…`、`104/1/1`，与 5084 一致 |

NDRANGE 与工作组数正是 5084 的值，说明 SP 状态对应这次 dispatch；管线状态里除 `SP_CS_BASE` 外的寄存器全部为 0。CONFIG 为 0 意味着 CS 未使能、指令长度 0，dispatch 无法完成。5249 的下一个管线状态（`SP_CS_BASE=0x4032890b00`、`CONFIG=0x105`）尚未进入 SP，两个 hardware context 都还是旧 BASE。固件分析文档中已记录 fault 46 有相同现象（“SP MVC 的 CS base 仍是该值，但 CONFIG/INSTR_SIZE 为 0”），当时未能解释。

MVC 是故障后的采集值，不是执行瞬间的直接观测；它与下面的对照一起才构成证据。

## 4. Turnip 与 shadPS4 的相关行为

- Turnip 的 `tu_CmdBindPipeline(COMPUTE)` 每次都把管线状态 IB 发进 `cmd->cs`，不做去重；`tu_dispatch` 不使用 draw state，只在调试用的寄存器踩踏（`TU_DEBUG_STALE_REGS_*`）开启时才在 dispatch 前重发 CS 状态，源码注释写明“绑定的管线可能被 render pass 或 blit 的寄存器踩踏覆盖”。正常路径假设计算寄存器跨 render pass 和 blit 保持不变。
- 上游先例 `048afdd4389`（2024）：3D blit 夹在计算管线绑定与 dispatch 之间时，若失效 CS 状态就会丢失 `CP_LOAD_STATE` 加载的内容，修法是 blit 不再置 `SP_UPDATE_CNTL` 的 CS 位。A8XX 的 `SP_UPDATE_CNTL`(0xab1f) 只剩各阶段 STATE 位，IB1 中除开头一次 `0x3f` 外都写 0，不是本次的来源。
- 图形与 blit 路径写的是 VS/FS 等阶段寄存器（`tu6_emit_xs_config` 对图形不写 CS），Turnip 本身没有把 CS 寄存器写 0 的路径；寄存器踩踏调试写的是 `0xffffffff`，与观测到的 0 不符。因此丢失更可能发生在硬件或固件的上下文切换中。
- shadPS4 自 10-07 起默认开启 B2“跳过重复 pipeline 绑定”（`Pipeline::Bind` 按调度器记录的 `bound_pipeline` 去重，`pipeline_cache bind_skip on|off`）。同一命令缓冲内同一计算管线不再重复 `vkCmdBindPipeline`，正好形成“绑定 → 大量图形工作 → 再 dispatch”的模式。KH3 的挂死在 10-10 首报。

## 5. 对照：每次 dispatch 前重新绑定即通过 loading

保持 `nocb`，只在会话开始时执行 `pipeline_cache bind_skip off`（统计 0 次被跳过），其余流程相同。

| 项 | 值 |
|---|---|
| 会话 | PID 17351，generation 1，run UUID `596354ce34c315d1214726fa06191f3b` |
| 结果 | 19:51:33 进入 loading，240 s 无 fault；随后开场过场、进入“觉醒之所”可操作场景；到 20:06 换装 APK 为止 faultcount 保持 51 |

同一 APK、同一驱动、同为 `nocb`，唯一差别是重复绑定。重新绑定时 Turnip 会在 dispatch 前重发管线状态 IB，与第 3 节的寄存器证据相互印证。该轮帧率很低（过场约 15 FPS、场景约 6.5 FPS，CPU 与 GPU 负载都不高），与 `nocb` 和高频重复绑定有关的部分未拆分，作为下一项工作单独处理。

## 6. Turnip 修复

子仓 `references/mesa-turnip`，分支 `codex/a8xx-compute-state-reemit`（基于 `92c96071dc5`），已推送到 `tencentmalos/mesa-mirror`，父仓引用 `c5a0d6fd990`：

| 提交 | 内容 |
|---|---|
| `2d96f4bc287` | `tu_cmd_state` 新增 `compute_state_end`，记录计算状态最后一次发出（绑定或 dispatch）后 `cmd->cs` 的写位置。A8XX 的 `tu_dispatch` 若当前写位置与之不同（期间录入过任何命令），先重发 `shader->state` 并置 `TU_CMD_DIRTY_COMPUTE_DESC_SETS` 重发 load state；连续 dispatch 不重发 |
| `fd6ba8f106b` | 按用户要求精简 Mesa fork 的 `AGENTS.md`：保留不在 Mesa GitLab 参与、AI 提交标注、不改 asahi 三条；允许代码注释、提交说明与日志，要求注明改动的服务对象、原因和证据，规避提供 `TU_DEBUG` 开关并在生效时记录日志 |
| `c5a0d6fd990` | `TU_DEBUG=nocsreemit`（bit 44，避开 fork 主分支上 `lrzfc` 占用的 bit 43）关闭该规避；A8XX 设备创建时 `mesa_logi` 一次；代码注释说明来源与证据 |

代价：仅在两次 dispatch 之间录入过其它命令时多发一个约 44 dword 的状态 IB 及描述符状态，连续 dispatch 无额外开销。规避对所有 A8XX 生效；若确认 8 Gen 5（Swan 等）固件不受影响，可用 `nocsreemit` 做 A/B 后再收窄到 gen8.0。

构建：WSL Ubuntu 24.04 用仓库 `build-turnip-source build` 编译（`/opt/mesa` 需先按分支名 fetch，按缩写 SHA 取不到），`2d96f4bc287` 驱动 SHA-256 `6fa6ecb7806e49c96f621ec3e34c8324c98bab57ddf11db6e4baa9f61a20575c`，宿主以 `SHADPS4_TURNIP_PREBUILT` 绑定（`HOST_LINK_PASS`），APK SHA-256 `52110f66c44cb85880ddd47a987c29ab647813691d369b537db2107563030ac3`，包内 `assets/native-turnip-mainline/vulkan.ad07xx.so` 与驱动一致。

## 7. 修复验证

APK `52110f66…` 安装后 `base.apk` 哈希一致；`debug.mesa.tu.debug` 清空，`bind_skip` 为默认开。

| 项 | 值 |
|---|---|
| 会话 | PID 21065，generation 1，run UUID `55b71e2871db0ae7eb4a743f8ab6f326` |
| 驱动 | logcat：`Selected Vulkan driver: … Mesa 26.3.0-devel (git-2d96f4bc28) … sha256=6fa6ecb7…`；宿主日志 `Turnip TU_DEBUG=gmem_warmup` |
| 结果 | 片头、标题、难度、确认正常；20:10:29 进入 loading，240 s 无 fault（`bind_skip on`，跳过重复绑定 1,001,227 次）；开场过场与歌词动画正常播放，约 24–40 FPS；之后持续运行 faultcount 保持 51 |

最终版 `c5a0d6fd990`：驱动 SHA-256 `bfc3af8b891de220c13190daf9386fd271b574e9df6656ce33161dd1360b57db`，APK SHA-256 `6cdd9e5418d56b37847c0dca13276994c65b2ec815ff722f887ce0c4fa14bfb6`，安装后 `base.apk` 哈希一致。

| 项 | 值 |
|---|---|
| 会话 | PID 23372，generation 1，run UUID `6e91aa93cd640e8c180fb92bf50e133f`；`TU_DEBUG=gmem_warmup`，`bind_skip on` |
| 驱动与日志 | logcat `Mesa 26.3.0-devel (git-c5a0d6fd99) … sha256=bfc3af8b…`；设备创建时 `I TU : A8XX: compute state is re-emitted after intervening commands (shadPS4 workaround, TU_DEBUG=nocsreemit disables it)` |
| 结果 | 20:21:08 进入 loading，本轮 loading 较慢（155 s 时进度约 40%），约 4 分钟内完成并进入剧情过场（棋盘场景约 20 FPS、GPU 约 70%）；全程 faultcount 保持 51 |

`TU_DEBUG=nocsreemit` 负对照（预期恢复挂死）尚未执行，见第 9 节。

## 8. 关于“文字不显示”修复（`92c96071dc5`）是否合理

背景：上游 `118ec7f45b1`（2026-08）为固件的 IB 32 位回绕缺陷（抢占恢复 IB 预取时高 32 位不进位）给命令 BO 加 `TU_BO_ALLOC_NO_32B_ROLLOVER`，本意是 IB 不跨 4 GiB 边界：DRM 后端用 VMA 堆 `nospan_shift = 32` 实现；KGSL 后端写成 `KGSL_MEMFLAGS_FORCE_32BIT`，注释自认“过度”，而且 `kgsl_gpumem_alloc_id.flags` 是 32 位字段，`1ULL << 32` 被截断，实际从未生效。截至 10-10 的上游 main 仍是如此。

此前 IB32 实验改用 `GPUOBJ_ALLOC` 让 `FORCE_32BIT` 真正生效，命令 BO（含 `pipeline_suballoc`，其中有 shader 代码与常量）全部落到低 4 GiB，结果字体复制计算着色器读回错误、缺字；Swan 上的 alloc64 候选也出现黑屏与着色器写地址 0。`92c96071dc5` 不再强制低地址，只检查实际分配不跨 4 GiB 边界，跨界时保留占位有限重试再释放。

结论：方向合理，与上游 DRM 后端语义一致，回到了 KGSL 上实际一直使用的高地址布局，并首次真正满足了上游要的防回绕约束。需要注意：

- 它修的是本 fork 自己引入的回归，不解释低 4 GiB 为何会让着色器出错。Odin3 上 `KGSL_PROP_UCHE_GMEM_VADDR` 为 `0x4000000`、GMEM 12 MiB，IB32 实验的 IB 在 `0x4cb2d80`、`0x50fb000`，紧贴 GMEM 窗口上沿，说明 KGSL 的 32 位区间从窗口之后开始分配；具体原因仍未查明，今后不应再强制命令/着色器 BO 进低 4 GiB。
- `TU_BO_ALLOC_REPLAYABLE`（`USE_CPU_MAP`）与 `NO_32B_ROLLOVER` 同时使用时 `GPUOBJ_INFO` 返回的 gpuaddr 为 0，会被判为拒绝并分配失败；当前 CS BO 与 `pipeline_suballoc` 都不带 `REPLAYABLE`，不受影响。

## 9. 待完成

- 以 `debug.mesa.tu.debug=nocsreemit` 跑同一流程，预期在 loading 恢复挂死，作为规避本身起作用的负对照（`c5a0d6fd990` 默认配置复测与日志检查已完成，见第 7 节）。
- 合入 fork 主分支 `codex/shadps4-xr-turnip`（其上另有 LRZ 修复 `04e1d665b27`、`fa9873b2cab`）。分支已推送、父仓引用已更新到 `c5a0d6fd990`。
- KH3 帧率低（CPU、GPU 负载都不高），下一项单独分析。
- 类型 B（保护写 `addr=0`、IB2 跨 4 KiB 页的 CRB 恢复）是另一类错误，本修复不涉及。

## 待补充（bug_reports）

用户说明：bug_reports 中有大量上下文，高通驱动约 855.0.9 前后固件一同更新，相关问题大幅减少；本机没有 bug_reports 副本，以下由用户后续补齐：

- 对应的高通驱动/固件版本号与 SQE 版本字，Odin3 当前 `V@0800.72` 与之的差距。
- bug_reports 中同类问题（上下文切换后寄存器丢失、计算或图形状态失效）的记录、设备与复现条件。
- 新固件下该规避是否仍需要，以及是否可按固件版本收窄。

## 证据

主要现场（fault 51 的 snapshot、dmesg、解码结果、寄存器、截图）已复制到仓库 [`bugs/odin3-kh3-compute-state-20261011/`](../../../bugs/odin3-kh3-compute-state-20261011/README.md)，附 SHA256SUMS。完整原始目录在 `build/validation/odin3-kh3-20261011b/`（不入库）：

- `kh3-save-config-before.tar`：本轮开始前的存档与配置备份。
- `nocb-01..12.png`、`nocb-load-*.png`、`nocb-fault51/`：`dmesg.txt`、`dmesg-plain.txt`、`snapshot.bin`、`snapshot.sqlite`、`analyze.json`、`android-host.log`、`logcat-crash.txt`、`ib1-dw0-5400-decoded.txt`、`ib2-0x4032891b80-decoded.txt`、`mvc-br-sp-cs-registers.txt`、`cs-state-ib-0x40055a0a80-raw.txt`、`live-command-cursors.txt`、读取身份文件。
- `bindskipoff/`：对照截图（loading、过场、可操作场景）。
- `fix-2d96/`：修复版本截图（片头、标题、确认、loading、过场）。
