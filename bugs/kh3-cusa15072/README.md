# 王国之心 III（CUSA15072）问题汇总（截至 2026-10-11）

把 10-07 以来在 AYANEO Pocket DS（Adreno 740）和 AYN Odin3（Adreno 830，gen8.0）上查到的 KH3 问题集中到一处，标明状态、证据位置和下一步，便于后续继续定位。细节以各条链接的原始记录为准；本文只做索引和归纳，不新增结论。

游戏版本：Ver 1.10（PKG `VERSION=01.10`、`APP_VER=01.00`，直接挂载主 PKG，未装更新或 DLC）。

## 一览

| # | 问题 | 设备 | 状态 | 记录 |
|---:|---|---|---|---|
| 1 | 存档迁移检查卡住（试玩版 `CUSA16838/kh3demo` 返回参数错误） | Pocket DS | 已修复 | [kh3-save-transfer-fix](../../docs/validation/android-native-host/kh3-save-transfer-fix-20261007.md) |
| 2 | 版权影片 `sceAvPlayerAddSource` 相对路径被拒 | Pocket DS | 已修复 | 同上“后续黑屏” |
| 3 | `sceAjmBatchErrorDump` 未登记导致 Faulted | Pocket DS | 已接诊断桩；ATRAC9 原始错误未定位（见 #14） | 同上 |
| 4 | 管线缓存加载把 fetch shader 覆盖为空，GS 管线无顶点输入，调色 LUT 全零 | Pocket DS | 已修复 | 同上“第二次录制与管线缓存根因” |
| 5 | 开场 USM 影片黑屏（`libSceVideodec2` 未接入，游戏停在视频准备，最终一层不透明遮罩） | Pocket DS | 已修复（`GuestVideodec2`） | 同上“开场流程定位” |
| 6 | 缺 `libSceJson2` | Odin3 | 已修复，可到标题与 loading | [odin3-session-status-games](../../docs/validation/android-native-host/odin3-session-status-games-20261010.md) |
| 7 | 文字缺字（命令 BO 被强制进低 4 GiB 后字体复制着色器读错） | Odin3 | 已修复（Turnip `92c96071dc5`） | [odin3-kh3-fix-experiments](../../docs/validation/android-native-host/odin3-kh3-fix-experiments-20261011.md) |
| 8 | loading 必现 GPU hang（`00EC2405`，`CP_WAIT_FOR_ME`） | Odin3 | 已规避（Turnip `c5a0d6fd990`）；10-12 对照确认原因是 bin 边界的 L1 中途抢占，重发移到紧挨 `CP_EXEC_CS` 的 `1efc1d3f5c9` 已合入 Turnip 主干 `codex/shadps4-xr-turnip`（`65b63f66bf5`，含 LRZ 修复），合并版过 loading | [bugs/odin3-kh3-compute-state-20261011](../odin3-kh3-compute-state-20261011/README.md) |
| 9 | 帧率低（池提交碎片化，每帧数百次宿主映射且持写锁） | Odin3、Pocket DS | 已修复主要部分（8.6 → 28–31 FPS） | [odin3-kh3-performance](../../docs/validation/android-native-host/odin3-kh3-performance-20261011.md) |
| 10 | 运行约 19 分钟后 `CP DDE BR opcode=0` → DeviceLost 闪退 | Odin3 | **未定位** | [bugs/odin3-kh3-cp-dde-opcode-20261011](../odin3-kh3-cp-dde-opcode-20261011/README.md) |
| 11 | 启动期 `CP Protected mode error WRITE addr=0`（IB2 页内偏移 `0xe88`，CRB 跨 4 KiB 页） | Odin3 | **未定位**，间歇出现 | 见下文 |
| 12 | 每帧约 10 张 1600×900 目标整图重传（`Texture.Stage` 约 10 ms/帧） | Odin3 | **未核实原因** | 性能文档“纹理重传诊断” |
| 13 | 平台限制：大核被 core_ctl 挂起、GPU 停在 660–734 MHz；唤醒代理占用中核 | Odin3 | 未处理（设备策略） | 性能文档第 2、3 节 |
| 14 | ATRAC9 解码失败（`ERR_UNPACK_SUPERFRAME_FLAG_INVALID`）与一次 `ReadHuffmanValue` 空指针 | Odin3、Pocket DS | **未定位**；池提交修改后的一轮未出现 | 性能文档第 5 节、fix-experiments“独立字体复制” |

另有用户观察：另一台 8 Gen 3 设备可以进入游戏（未取得其驱动与 ROM，不能据此认定只与硬件代际有关）。

## 已修复项要点

1. **存档迁移**：Android 迁移适配器把 `TransferringMount` 交给普通 `Mount`，跨标题 ID 一律拒绝，游戏无法把“没有试玩存档”当作正常结果。新增只读的 `GuestStorage::MountTransferring`，不存在返回 NOT_FOUND。
2. **AvPlayer**：游戏用 `../../../tresgame/Content/Movies/ja/copyright.mp4` 加文件回调，旧适配器要求绝对路径。有完整回调时原样交给回调解释。
3. **AJM**：`sceAjmBatchErrorDump` 接入桌面诊断桩（记日志返回 0），不改批次结果。ATRAC9 首个错误仍未定位。
4. **管线缓存**：`PipelineCache::LoadPipelineStage` 按 Fragment → Vertex → Geometry 恢复时，Geometry 的空 fetch shader 覆盖了 Vertex 恢复的值；改为与运行时编译一致，只在非空时更新。同时修了录制工具的两处遗漏（录制前排队的提交副本、写回期间的 flip 通知）。
5. **Videodec2**：开场影片 `mv_bt051.usm` 走 `libSceVideodec2`，原先模块加载返回 `0x805a10ff`、9 个导入全被拒，游戏卡在视频准备、最后一层是全屏不透明遮罩。接入 `GuestVideodec2` 后棋局开场影片与字幕正常。
6. **Json2**：补齐系统模块后可到标题与 loading。
7. **缺字**：上游 `118ec7f45b1` 想让命令 BO 不跨 4 GiB，KGSL 后端用的 `KGSL_MEMFLAGS_FORCE_32BIT` 在 32 位 `flags` 字段中被截断、从未生效；本 fork 曾改为真正生效（全部落到低 4 GiB），字体复制着色器随即读错。`92c96071dc5` 改为只检查实际分配不跨 4 GiB、跨界时有限重试。**低 4 GiB 为什么让着色器读错仍未解释，今后不要强制命令/着色器 BO 进低 4 GiB。**
8. **loading GPU hang**：gen8.0 上计算管线绑定后经过 render pass、blit、LRZ 清除，再 dispatch 时 SP 的 CS 配置寄存器为 0（`SP_CS_BASE` 保留），dispatch 永不完成。shadPS4 默认跳过重复 pipeline 绑定（B2）形成该模式；`bind_skip off` 可通过。Turnip 在 A8XX 上若计算状态发出后录入过其它命令，dispatch 前重发状态；`TU_DEBUG=nocsreemit` 关闭。10-12 在 Odin3 上用 `nocsreemit` 对照：默认复现挂死；`skipsaverestore=0` 仍挂死；`preempt_level=0`（不做中途抢占）通过。挂死的两份 snapshot 都在 GMEM pass 结束处有一次 bin 边界切出。所以是 L1 中途抢占切回后 SP 计算程序寄存器丢失，CP 的保存/恢复不覆盖它们，只能由 UMD 重发。
9. **池提交**：`MemoryManager::PoolCommit` 在碎片化池上把一次提交拆成约 22 块逐块映射（Map + Protect，Android 每次约 0.11 ms），全程持写锁，GpuComm 也在等这把锁。改为优先用能容纳整次提交的最小空闲池区，读写权限不再二次 `Protect`。10-07 Pocket DS 上同一热点已被采到（Guest-57 的 `PoolCommit` 占 75.75%，`UpdateDataMapping` 自身 43.56%）；`UpdateDataMapping` 每次复制整张映射表的问题仍在，只是调用次数降了。

## 未解决项

### #10 `CP DDE BR opcode=0` 闪退

fault 53：会话约 19 分钟、进入新区域时，KGSL 报 DDE 非法包头，随后稀疏绑定提交 EPROTO → DeviceLost → 宿主断言。snapshot 只定位到 BR 停在一条管线 draw-state 的 `CP_SET_DRAW_STATE` 附近；draw-state 载荷未被冻结、DDE 的固件 PC 未映射。签名与 Swan 未解决的 B 类相同（那里三次都紧跟 RB2 的 level=1 抢占恢复）。完整现场、解析边界和 5 项待确认在 [bugs/odin3-kh3-cp-dde-opcode-20261011](../odin3-kh3-cp-dde-opcode-20261011/README.md)。Odin3 10-08 还记录过一次同签名故障（仓库根 `AGENTS.md` 10-09 条目）。

### #11 启动期保护写 `addr=0`

| 时间 / 条件 | KMD uptime、ctx/ts | IB2 | 证据 |
|---|---|---|---|
| 10-10 23:25:20、23:29:24（原驱动 `2b8ed3fc20`） | 126723.3 ctx6/5534；126967.2 ctx14/5528 | 23:29:24 为 `0x418e4d6e88`（304 dword），23:25:20 无匹配 snapshot | `build/validation/odin3-kh3-gpu-hang-20261010/current-driver/repro3/` |
| 10-11 默认 / `vk_recorder off` / `bind_skip off` | 132476.5、132689.2、132837.7 | `0x418f496e88`、`0x418fb06e88`、`0x418ee16e88` | `build/validation/odin3-kh3-fix-20261011/{baseline,recorder-off,bind-skip-off}/` |
| fault 48（pre-bind flush 诊断） | 157051.7 ctx4/24691 | `0x418ff16e88` | `…/compute-prebind-flush/fault48/` |
| fault 49（`TU_DEBUG=syncdraw`，`addr=0x08837`） | 164314.96 ctx6/5658 | `0x418e856eac`（307 dword） | `…/syncdraw/` |
| fault 50（`crb_page_align` 诊断） | 164611.2 ctx4/5581 | `0x418e9f6e88` | `…/crb-align/` |
| fault 52（含计算状态修复的版本） | 170168.5 ctx6/5593 | `0x418f9f6e88` | [bugs/…/evidence/fault52](../odin3-kh3-cp-dde-opcode-20261011/README.md) |
| 10-12 fault 60、61（`c5a0d6fd990` + `nocsreemit`，默认抢占） | 197536.2 ctx21/29931；197649.6 ctx4/5657 | `0x418ff16e88`、`0x418ea66e88` | `build/validation/odin3-kh3-preempt-20261012/A-nocsreemit/` |
| 10-12 fault 64、65、66（同上，**`preempt_level=0`**） | 198382.1 ctx14/5710；198523.6 ctx14/13383；198613.1 ctx4/5586 | `0x418da96e88`、`0x418e956e88`、`0x418fb16e88` | `…/C-nocsreemit-level0/`，fault 64 读取记录与 dmesg 见 [计算状态 bug 的 evidence](../odin3-kh3-compute-state-20261011/evidence/fault64-level0-opening/) |
| 10-12 fault 68（新 Turnip `1efc1d3f5c9`，默认抢占、计算状态重发开启） | 201235.6 ctx15/5872 | `0x418e6c6e88` | `…/D-reemit2/fault68/`；IB1 取指位置同样是 960，宿主随后在 `GetRenderFrame` 断言退出 |

10-12 这 5 次的 BR IB1 取指位置都是 960（IB1 长 14223，fault 60 为 17494），SQE PC `0x362`、`$19=0xbad4`；IB2 是 304 dword 的 draw IB2，结尾为 `CP_DRAW_INDX_OFFSET`，已全部取完。fault 64 的 CP 上下文记录里没有任何中途切出（level 0 下预期如此），计算状态完好（`SP_CS_CONFIG=0x105`）。**在 level 0 下仍出现 3 次，说明 #11 不依赖 L1 中途抢占**；当天 8 次启动（7 个出故障的进程加 1 次通过）中 5 次在片头出这个故障，比之前频繁，但各次之间除抢占参数外条件相同，原因未知。换成新 Turnip `1efc1d3f5c9` 后的两次启动又出了一次（fault 68，IB1 长 14542，取指位置仍是 960），所以与计算状态重发的位置无关。gpu-snapshot MCP 解析结果：`build/gpu-snapshot/odin3-kh3-20261012/fault64.sqlite`。

共同点：都在片头到 loading 之前；IB2 尾部页内偏移 `0xe88`（fault 49 为 `0xeac`），IB2 的第 38 dword 起是 130 个 payload 的 `CP_CONTEXT_REG_BUNCH`，跨到下一 4 KiB 页；SQE 停在 `$19=0xbad4` 的错误恢复入口（PC `0x362`），重读并跳过 169 dword。snapshot 里该 CRB 的寄存器对全部合法，与 SQE 留下的零包头不一致。

已否定或未证实：`vk_recorder off`、`bind_skip off`、MRT 改逐寄存器写（PKT4 版本改为在 IB2 dword 121 处跨页，同样恢复）都不能消除；`crb_page_align` 诊断只覆盖 GROW 命令流的 CRB builder，未证明覆盖出错的发射路径。PKT4 诊断驱动上 root 全局关闭抢占后出现的是 GPU timeout（不是保护写），不能据此合并为同一机制；10-12 在正式驱动上 `preempt_level=0` 时保护写照样出现，抢占不是必要条件。**这是命令读取跨页时的可见性线索，不是已证明的页表或硬件根因。**

下一步：确认出错 CRB 由哪条 Turnip 发射路径产生（`tu6_emit_mrt` 等 draw state），让页对齐诊断覆盖它；对照 bug_reports 中 gen8.0 固件 CRB/抢占恢复的记录。

### #12 每帧纹理整图重传

`upload_diag` 39 帧：35 张图像 461 次上传，395 次内容与上次相同。两张 32×32×32 层 A2B10G10R10 体纹理约每帧各一次；约 25 张 1600×900 R16G16B16A16/B10G11R11 目标每张约 3 帧一次，`last_writer` 为空。推测：游戏每帧约 11 次 pool commit/decommit，`PoolCommit`/`PoolDecommit` 都会让纹理缓存释放区内图像，下次绑定时重建并从 guest 内存整图上传（既浪费，也可能用旧 guest 内容覆盖 GPU 结果）。**未核实**：先用 DebugBus `memory_describe <地址>` 确认这些地址是否在池提交区，再看图像被释放的调用点。

### #13 平台与调度

Thermal Status 2；大核簇 core_ctl `min_cpus=0`、几乎不参与；GPU 封顶 967 MHz 但 governor 停在 660–734 MHz。修改后 GPU 忙碌 71–77%，开始受 GPU 与频率限制。唤醒代理按设计放在最低容量簇，Odin3 没有小核，于是占用承载游戏的中核（过场中约 40%）；曾做的开/关对照跨了场景，作废。`scePthreadGetspecific` 每帧约 2,550 次 HLE（单次开销小，但每次都退出 JIT）。设备策略可用 ClusterTune 调整后再对照，本仓未改设备设置。

### #14 ATRAC9

Odin3 修改前一轮：从进入新游戏开始 `Atrac9Decode failed ret = -0x7e000000`（`0x82000000`，superframe 首帧标志与解码器内序号不一致），loading 期间约 5,300 次/秒、可操作场景约 2,000 次/秒，音频线程每次提交立即失败再提交，日志写满轮转。池提交修改后的一轮 0 次（单样本）。可能与旧的稀疏映射或帧率过低导致的流读取跳跃有关，均未验证。另有一次 attract 视频在 Atrac9 `ReadHuffmanValue` 空指针崩溃（fix-experiments“独立字体复制”一节）。Pocket DS 的 AJM 批次错误同样未定位。

## 复现路径

1. 主 PKG 直接挂载（`/sdcard/game/ps4/roms/CUSA15072.pkg`，SHA-256 `5f54ed49…`）。
2. 启动时“检查保存资料” → 片头 → 标题 → 新游戏 → 难度 → 确认 → 亮度 → INFORMATION → loading → 棋局开场影片 → 彩色玻璃平台（“觉醒之所”）可操作场景。
3. #8 在 loading 开始约 52 s（修复前必现）；#11 在片头附近间歇出现；#10 在场景内游玩约 19 分钟后出现一次。

Odin3 上用 DebugBus 手柄推进；每次 GPU 故障后需要 root 立即读取一次 snapshot（读一次即清空，否则后续故障会被旧 snapshot 占住）。

## 常用开关与命令

| 用途 | 方式 |
|---|---|
| Turnip 调试开关 | `debug.mesa.tu.debug`（宿主会再并入 `gmem_warmup`）：`nocsreemit`（关闭计算状态重发）、`nocb`、`syncdraw`、`cmd_no_preempt`、`kgsl_preempt_rb` / `kgsl_preempt_fg`、`sds_page_align`、`rd`（记录提交与 BO，开销大） |
| 重复绑定 | DebugBus `pipeline_cache bind_skip on` / `off` |
| 纹理重传 | DebugBus `upload_diag start`、`status`、`stop` |
| 地址归属 | DebugBus `memory_describe <address> [bytes]` |
| 唤醒代理 | DebugBus `wake_proxy on`、`off`、`spin <us>`、`status` |
| GPU 计时 | DebugBus `gpu_timing start` / `stop` |
| LiteP 采集 | DebugBus `profiler_capture file <MiB> <sec>`，`index_trace` 时 SDK 修订去掉 `+slow-frame-v1` 后缀 |
| snapshot 解析 | gpu-snapshot MCP `gpu.snapshot_analyze`；旧发布版解析该机 snapshot 报唯一键错误，my_mcp_tools `master` `f309a13` 已修复（`FindMemoryIndex` 先按引用身份查找，附回归测试）；在其之前构建的发布包仍会报此错，需用新版本 |

## 证据位置

- 仓库内：[bugs/odin3-kh3-compute-state-20261011](../odin3-kh3-compute-state-20261011/README.md)（fault 51）、[bugs/odin3-kh3-cp-dde-opcode-20261011](../odin3-kh3-cp-dde-opcode-20261011/README.md)（fault 52、53）。
- 本机忽略目录（未入库）：`build/validation/kh3-save-fix-20261007/`（Pocket DS 录制、回放、LiteP）、`build/validation/odin3-kh3-gpu-hang-20261010/`（首次 hang、固件分析）、`build/validation/odin3-kh3-fix-20261011/`（faults 42–50 与各诊断驱动）、`build/validation/odin3-kh3-20261011b/`（fault 51–53、性能采集）。
- 固件原文件：bug_reports `qualcomm_gpu_driver/firmware/odin3-gen8-0/`（SQE `9c8f009b…`、AQE `f04cb97e…`、GMU `1ad8175e…`），见 [固件分析](../../docs/validation/android-native-host/odin3-kh3-firmware-analysis-20261011.md)。
- 讲解视频：[docs/media/turnip-architecture](../../docs/media/turnip-architecture/README.md)，约 8 分钟，用 #8、#10 和 ASTRO BOT VSC 溢出作例子讲 Turnip 各层。

## 建议的后续顺序

1. #8 负对照：`nocsreemit` 跑同一流程，确认规避本身起作用。
2. #10、#11：在有 bug_reports 和固件反汇编的环境里按 bugs 目录的清单确认；两者都落在 draw-state / CRB 读取路径上，可一起看抢占恢复的关联。
3. #12：核实图像释放来源后优化，是 GpuComm 当前最大的单项。
4. #14：多轮复现确认 ATRAC9 错误是否随池提交修改消失。
