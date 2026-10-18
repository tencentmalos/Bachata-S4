# A8XX `CP DDE BR opcode=0` 与 stream submit 挂死：上游调研、本地核对与 A/B 方案（2026-10-17）

**状态：手机上的规避方案未确定，A/B 尚未运行。** 用户确认：Swan（Adreno 840）上的同类问题已由我们推动、通过更新 GPU 固件修复；手机（Odin3 的 Adreno 830，以及 8 Elite Gen 5 系列 Adreno 840 手机）的固件预计仍带着这个问题。高通改动的是跨 pipe 的抢占保存/恢复与 DDE 状态交接；实际修复对应的固件版本有待确认，见 6.1。本文记录 2026-10-17 对 Odin3 上两类 GPU 故障做的工作：

- 本地快照核对；
- 上游 Mesa 与外部渠道的调研（每条出处都已重新核对原文）；
- 由此做的 Turnip 改动；
- 待执行的对照方案。

相关记录：

- [GoW3 文档的 stream submit 一节](../online-content/gow3-launch-20261015.md#streaming-submit-33--51-fps-then-gpu-hangs-on-odin3-2026-10-17)
- [KH3 的同类故障 fault 53](../../../bugs/odin3-kh3-cp-dde-opcode-20261011/README.md)
- [Swan A/B 类固件分析](swan-cp-opcode-firmware-20260930.md)
- [A8XX 计算状态丢失（抢占恢复先例）](../../../bugs/odin3-kh3-compute-state-20261011/README.md)

## 1. 现场

| 项 | 值 |
|---|---|
| 设备 | AYN Odin3 `6edb0593`，Adreno 830（gen8.0） |
| 游戏 | God of War III Remastered `CUSA01740`（EmuPSP 引用 `emupsp-CUSA01740`） |
| 当时的 Turnip | `65b63f66bf5`（`codex/shadps4-xr-turnip`，含 A8XX 计算状态重发与 LRZ 修复） |

本文涉及两种故障签名。

### 1.1 fault36：`CP DDE BR opcode=0`（17:29，PID 27532，ctx 23）

快照 `kgsl-snapshot-fault36.bin`（内核时间 1792229338），用 my_mcp_tools `3cc8770` 的 gpu-snapshot 解析。expert 数据库 `kgsl-fault36-expert.sqlite` 在 `build/validation/console-ui-20261017/`。

- DDE_BR 固件停在 PC `0x717`，寄存器 19 = `0xbad4`，工具将其映射为"全零包头"陷阱。签名与 Swan B 类、Odin3 KH3 fault 53 相同。
- BR 处在 IB1 `0x423d8b2000`（6242 dword，剩 5730）→ IB2 `0x422a37e000`（993 dword，已全部取完，ROQ 中剩 388）。推算执行点在 IB2 第 606 个 dword 附近：一条 `CP_SET_DRAW_STATE`，绑定整条管线（组 0、1、2、3、4、5、7、8、18、26）。
- 该包及相邻包的全部载荷都被快照冻结，解码均为合法 PM4。也就是说，DDE 读到的"全零"在快照时刻已经不存在。
- 抢占记录里只有历史切出，且都不在这个 IB1 内，因此这份快照既不能证明、也不能排除故障前发生过抢占恢复。

### 1.2 stream submit 挂死（21:13、21:55，第二次 PID 21347，ctx 28）

`upload_diag stream_submit 200` 作为默认值时（APK `e679b8e7`、`c5fa83df`），两次运行都在读档后 1–2 分钟挂死：

- KGSL 先打印 `possible deadlock. Context N might be blocked for itself`，随后报 GMU 抢占故障，状态 `00880005`。
- 时间戳 N 已开始执行但一直没有退役，Present 线程以 `PrepareLastFrame: Device lost` 中止。

这两次没有留下快照：KGSL 只保留第一份未读快照，之后读到的正是上面那份旧的 fault36。默认值已改回 0（APK `fce194d7`），DebugBus 开关保留，供实验使用。

`blocked for itself` 是 KGSL 同步对象超时后打印上下文时的诊断行（CodeLinaro `adreno_drawctxt.c` 中的上下文转储），说明同一上下文前面的工作没有退役，是挂死的结果而不是原因。

两种签名是否同源尚未确定。共同点：都在 A8XX 上，都会随负载升高而更频繁。stream submit 让每帧的提交和命令缓冲的回收次数都大幅增加。

## 2. 本地核对（10-17 新增）

| 检查 | 结果 | 含义 |
|---|---|---|
| fault36 所有 draw-state 载荷（4896 条）和两个 IB 是否跨 4 GiB | 0 条跨越；高 32 位只有 `0x40`、`0x42` | 上游 `118ec7f45b1` 描述的"抢占恢复时地址进位丢失"不适用于这次故障 |
| 故障点那条 SDS（IB2 第 606 个 dword，10 组）是否跨 4 KiB 页 | 0 组跨页；全部载荷中有 53/4896 跨页 | Swan A 类"跨页载荷读成 0"不适用；这次属于 B 类（DDE 侧读到 0） |
| 这 10 组载荷所在的分配 | 都在 128 KiB 的 `OBJECTANY` 分配内（如 `0x4031d4e000`、`0x40310c9000`、`0x4003b54000`、`0x4031b89000`） | pipeline 子分配块（128 KiB）与命令流 BO 都可能是这个大小，快照无法区分；要靠 `TU_DEBUG=bos` 或带名字的 BO 日志 |
| shadPS4 游戏过程中是否销毁 `VkPipeline` | 否。`graphics/compute_pipelines.erase` 只出现在创建抛异常时，以及着色器热重载（调试功能）时 | 宿主侧 pipeline 的 use-after-free 可能性低；若存在 UAF，更可能在命令缓冲的复用上 |

## 3. 上游 Mesa 核对

我们的基线是 `31a6abbf998`（10-08 合入）。10-17 拉取的 `upstream/main` 为 `335d5698dca`，其后 freedreno 相关提交共 13 个，与本问题有关的只有一个：

- **`c88b6ebd383` "tu: Fix bindless descriptor invalidate sequence"**（Connor Abbott，Cc mesa-stable，[!45129](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/45129)）。
  - A8XX 的 `SP_UPDATE_CNTL` 没有 bindless 字段，旧代码在 A8XX 上实际**从未让 bindless 描述符缓存失效**；修复后改写 `SP_CS/GFX_BINDLESS_INVALIDATE`。
  - 这是正确性问题（可能读到旧描述符），与挂死无直接关系。
  - 已 cherry-pick 为 `bbb3e82bc8d`，无冲突，见第 5 节。

我们树里已有、且与抢占或固件相关的上游提交：

| 提交 | 内容 |
|---|---|
| `118ec7f45b1`（[!43970](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43970)，2026-08-25） | 固件在抢占后恢复 IB 预取时，地址加偏移不向高 32 位进位；IB 跨 4 GiB 时会从错误地址取指。上游的规避方法：给 `tu_cs` 和 `pipeline_suballoc` 的 BO 加 `TU_BO_ALLOC_NO_32B_ROLLOVER`。 |
| `68163a732e6` | gen8 上用 `CP_SCOPE_CNTL` 给性能计数器查询关抢占 |
| `1eb002f2624` | qrisc 增加抢占入口 |
| `9fb6203bc13`、`5cea3f8fb30`、`74865282cf8` | a650/a660 固件挂死、`QCTDD04536579` 改为 quirk、LRZ 方向写入 |

**上游的 4 GiB 规避在 KGSL 上无效：**

- `upstream/main` 的 `kgsl_bo_init` 把 `KGSL_MEMFLAGS_FORCE_32BIT`（定义为 `1ULL << 32`）或进 `struct kgsl_gpumem_alloc_id` 的 `flags`，而这个字段是 `unsigned int`，标志位被截掉。
- mesa#16460 的报告者实测 5337 个 BO 中没有一个落在 4 GiB 以下。
- 我们分支的 `92c96071dc5`（10-11）改用 `GPUOBJ_ALLOC` 加 `GPUOBJ_INFO`，检查分配是否跨 4 GiB，最多重试 4 次。因此我们的命令流和 pipeline 子分配块确实不跨越；上一节的快照核对也印证了这一点。

## 4. 外部参考（逐条核对过原文）

原始数据存在本地 `build/validation/preempt-ab-20261017/external/`（未入库），SHA-256 列在同目录的 `SHA256SUMS`。

### 4.1 mesa#16460：A840 上 `CP opcode error opcode=0`

链接：<https://gitlab.freedesktop.org/mesa/mesa/-/work_items/16460>。2026-09-30 开启，仍未关闭。

**报告者的环境与现象：**

- nubia（RedMagic 系）Adreno 840，`gen8-2-1`，KGSL。
- 在 Winlator 中用 vkd3d-proton 跑 D3D12 游戏（刺客信条 幻景），每隔几秒到几分钟出现 `opcode=0x00000000`，约 16 次 / 90 秒。
- 高通原厂 blob 驱动在同一内核上也出现同样的故障。

**报告者 10-02 的更正：** 他所有的抓取都是**主 CP 的 `CP opcode error interrupt`**，不是 `CP DDE BR`。正文里 DDE 的说法是标错了。也就是说，这个 issue 与我们的签名**并不完全相同**：我们是 DDE_BR 读到全零。

**报告者给出的其他事实：**

- 8 份快照里，故障所在的 IB1/IB2 都是合法 PM4。
- 他做过的负对照，故障都照旧：
  - 检查生命周期（约 75 万次提交的校验，约 100 万次 reset 均无违规）；
  - `TU_DEBUG=flushall`；
  - 末尾补 NOP；
  - 每个 IB1 都 `DISABLE_ALL_GROUPS`；
  - **开、关抢占**；
  - 更换缓存一致性模式。
- 换 GMU/SQE 固件（`0x50030017`、`0x50030019`、`0x50030027`、`0x50030030`）只改变抢占签名，或多出一种 `MISC: GPU hang detected` 风暴；opcode=0 故障一直存在。
- KGSL 在上下文失效时会把 sop/eop 时间戳都写成当前值，所以 hang 之后用户态读到的 retired 值会越过实际没完成的提交。

**维护者的意见：**

- Rob Clark（09-30）倾向于 KGSL 或 SQE 固件问题，建议换另一台 A840 设备复现。
- Connor Abbott 的意见：
  - （09-30）pipeline 里装的就是 DDE 处理的 draw state，所以这很像 **pipeline 的 use-after-free**。复用 BO 只是绕开问题，建议用 Vulkan Validation Layers（VVL）排查。
  - （10-01）DDE 不读 IB1/IB2，只读 draw state，所以"IB 合法"与问题无关，仍可能是生命周期问题。
  - 报告者表示在 Winlator 下挂不上 VVL。

### 4.2 mesa!44888（Draft）：回收退役的命令流 BO

链接：<https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44888>，2026-10-06 开启。

**做法：** 在 A840 + KGSL 上，用 device quirk 控制：退役的命令流 BO 不再 `GPUMEM_FREE_ID` 给内核，而是保留 GPU 映射、只解除 CPU 映射，再交给之后参数完全相同的分配复用。上限 256 MiB，pipeline 子分配块不在范围内。

**A/B 结论**（同一场景；报告者的基线在 15 秒到 2 分钟内出故障）：

| 条件 | 结果 |
|---|---|
| 回收命令流 BO（含/不含子分配块） | 5 分钟以上无故障；峰值约 45–52 MiB |
| 只回收 pipeline 子分配块 | 约 1 分钟出现 **DDE BR** 故障 |
| 关闭 quirk（基线） | 几乎立即出现 **DDE BR** 故障 |
| 只延迟、之后批量释放（不复用），32 MB 上限 | 约 2 分钟出故障 |
| 清零后保留映射、不复用 | 5 分钟无故障 |

报告者据此认为：只延迟不够，"释放给内核"这个动作本身才是触发条件；保留但清零也干净，说明并不是 GPU 在读退役内容。注意这张表里的基线故障是 DDE BR，与 issue 正文更正后的"主 CP"不同，可见两种签名在他的设备上都出现过。

Connor Abbott 10-06 的评论：这种形式绝不会被合入，它显然在绕开别的问题，很可能是 use-after-free。被释放的页如果又被别的东西复用，读到的就是那页的新内容，不一定会缺页。在 blob 上也出现同样的故障，恰恰说明栈里某处有 UAF。

### 4.3 WinNative-Emu/Drivers 0005 / 0006

- 链接：<https://github.com/WinNative-Emu/Drivers>，提交 [`c71f600`](https://github.com/WinNative-Emu/Drivers/commit/c71f600a2977a141f21a922c9ed2e14fb09b13d3)，2026-09-29。
- 0005：A8XX 通过 KGSL 虚拟 BO 别名取命令流，释放前先解除别名。其 README 的说明是，直接释放命令流内存会导致 GPU 挂死。
- 0006：按大小、内核分配标志和对齐，复用退役的 IB 存储和别名；上限 256 MiB，`TU_KGSL_IB_CACHE=false` 可关闭对照。
- 实测设备是 RedMagic Adreno 840。作者记录：关闭复用后同一二进制会复现 GPU 丢失，开启后可正常移动、进菜单、正常退出；20 分钟以上的长时间游玩尚未验证。
- 0006 依赖 0005 的别名机制，不能直接套在我们的分支上；!44888 是不依赖别名的版本。

### 4.4 Qualcomm KGSL（CodeLinaro graphics-kernel）

链接：<https://git.codelinaro.org/clo/la/platform/vendor/qcom/opensource/graphics-kernel>。以下各条都按提交原文核对过；与我们设备内核的对应关系未确认。

| 提交 | 日期 | 内容与相关性 |
|---|---|---|
| `44b9ce7d` "gen8: Fix the DDE ROQ size in snapshot" | 2026-04-29 | 只改了 `adreno_gen8_2_0_snapshot.h`：DDE_BR/BV 的 ROQ 转储量从 0x100 改为 0x200。gen8.0（Odin3）的表是否有同样的截断**未核实**；我们快照里 DDE ROQ（section 385）的数据可能不完整。 |
| `d5188a1e` | 2024-08-21 | gen8_0_x 默认启用 L1A 抢占 |
| `f48b371a` | 2026-03-06 | 新增 tracepoint，记录 GMU 动态修改抢占级别。可能解释 Swan 上请求 style 1 仍发生 L1 抢占（推测）。 |
| `9193f728` | 2025-05-02 | 从 ucode 读取抢占上下文记录大小，因为 ucode、GMU、KGSL 之间"可能不一致"。固件与内核版本不匹配时，恢复路径可能出错（推测）。 |
| `5816f6e6` "Give hint to SMMU for skipping TLB ops during slumber" | 2024-02-02 | slumber 期间通过 `qcom_skip_tlb_management()` 让 SMMU 驱动跳过 TLB 操作，唤醒时（hint 为假）再整体 flush。 |
| `93ddb84a` "Optimize TLB flush during slumber exit" | 2026-05-26 | 把 hint 改为两个位，CX 判定已掉电时**唤醒也不 flush**；提交说明自己写明"CX 不掉电时残留 TLB 项会导致错误翻译"。 |
| `0a40ed44` | 2026-08-31 | 在一个 a6xx/gen7 分支上回退的是 **`5816f6e6`**，不是 `93ddb84a`（10-18 之前本文误写为后者的回退）。 |

**Odin3 KMD 的实际情况（10-18 核对 `msm_kgsl.ko`，SHA `d00b3355…`）**：模块导入 `qcom_skip_tlb_management`，`kgsl_iommu_send_tlb_hint`（244 字节）对 user、LPAC、GMU 三个域发 hint 后只检查 bit 0，bit 0 为假时对三个域 `flush_iotlb_all`；没有 bit 1 的判断。所以 Odin3 带的是 `5816f6e6` 这一版（唤醒时会 flush），**不含 `93ddb84a`**。`_iopgtbl_unmap` 在 `qcom_arm_lpae_unmap_pages` 之后对三个域无条件调用 `flush_iotlb_all`（是否真正执行由 SMMU 驱动按 hint 决定）。"唤醒跳过 flush 导致残留翻译"这条路在 Odin3 上不成立。

### 4.5 调研中提到但未采用的线索

调研代理还提到 Banners-Turnip、whitebelyash 等分支对 0006 或 A830 GMEM 的说法。我在其 README 里没有找到对应原文，故不作为依据。

## 5. 本轮的 Turnip 改动

都在 `references/mesa-turnip`，只提交在本地，未 push；父仓引用也没有推进。

| 分支 / 提交 | 内容 | 驱动 SHA-256 |
|---|---|---|
| `codex/shadps4-xr-turnip` `bbb3e82bc8d` | cherry-pick 上游 `c88b6ebd383`（A8XX bindless 失效） | `7cf188b722ac04df474f23b151861b6d48f1af88a5a3c3eea7f178a50de65871` |
| `codex/a8xx-cs-recycle` `ca038cc10f1`（基于 bbb3） | 新增 `TU_DEBUG=cs_recycle`，**默认关闭** | `c71158723b758eacef5c9ace3a7091a8aa8cd193a562da50074cdd9728d64046` |

`cs_recycle` 是 !44888 的移植，改为 `TU_DEBUG` 开关，而不是按设备启用的 quirk：

- 范围：带 `TU_BO_ALLOC_NO_32B_ROLLOVER` 的分配，排除 `pipeline_suballoc`（新增 `TU_BO_ALLOC_IB_SUBALLOC` 标记）、`REPLAYABLE` 分配和稀疏分配。
- 匹配条件：请求的大小、请求给内核的 flags、iova 对齐。
- 退役时保留 GPU 映射、解除 CPU 映射；空闲上限 256 MiB，超出时先释放最旧的；设备销毁时统一释放。
- 适配我们分支的分配路径：命中缓存时直接复用已通过 4 GiB 检查的 iova，未命中时走原来的检查加重试。
- 启用时打印 `Turnip: cs_recycle keeps retired command-stream BOs ...`；每 4096 次入缓存打印一次统计，设备销毁时打印最终统计（hits/misses/parked/evicted/peak）。

两版驱动都在 WSL 中用仓库配方从源码构建，源码干净（`dirty=False`）。

**宿主与 APK：** host `libshadps4_host.so` SHA 前缀 `4bdd298b`，APK `97d67aa9b038d66a7835d833adae9d9da5a8fb51c0d16acf9d9cb76074ada32b`，内含 `native-turnip-mainline` = cs_recycle 驱动（Odin3 未设 `debug.shadps4.vulkan_driver`，默认就加载 mainline）。APK 存档于本地 `build/validation/preempt-ab-20261017/shadps4-csr.apk`。只带 bbb3 的 APK `93e7f36b…` 已被它取代，两者都**尚未安装**。

## 6. 固件状态与假设

### 6.1 Swan 的固件更新与手机现状

用户确认（2026-10-17）：

- Swan 上的同类问题由我们推动高通，以**更新 GPU 固件**的方式修复。
- 手机预计仍带着这个问题，包括 Odin3 的 Adreno 830，以及 8 Elite Gen 5 系列 Adreno 840 手机。

**内部资料：** 用户提供的飞书文档《Spider 新 GPU firmware 与当前设备旧 firmware 对比》（2026-08-07 修订版，Markdown 导出）。副本在本地 `build/validation/preempt-ab-20261017/external/spider-gpu-firmware-compare-20260807.md`，属内部资料，不入库；飞书原文链接见该副本。要点如下：

- **对比对象：**
  - 新固件：高通改动包 `v855.0.5-add-FW-change.zip`（SM8850，2026-07-30 经飞书发送，62,962,506 字节，SHA `17bb5f6d1b565a99c2f6e6f8b00bfc339677320625e2be00a2bc3df011342bb2`）。随包说明列出的改动文件是 `gen80200_sqe.fw` 与 `gen80200_zap.mbn`，ZAP 未纳入分析。
  - 旧固件：当时的 Swan 设备（B3110，ROM `6.1.0-202607290259-NIGHTLY`，`qcom,adreno-gpu-gen8-2-1` / Adreno840v2，固件族 `gen80200_*`）。
- **有变化的只有 `gen80200_sqe.fw`（ZAP 未分析）：**
  - SQE 从 `0x01510095`（119,348 字节，SHA `bd5bfed8c4456c5387dd6257b1e6791f4bbd0126c0af701c6f01bdbfcf9489dd`）变为 `0x01510103`（121,172 字节，SHA `1f1c7bae8050a9910902ec42b3d741ff3a68bc21114dea966a502ade2a9b606d`），多 456 个 32 位字：SQE image +312、BV +88、LPAC 0（逻辑有变）、DDE +56。
  - AQE `0x01510026` 与 GMU（core 5.3.25 / HFI 2.7.1）逐字节相同。
- **SQE 四个 image 的变化**（静态反汇编推断，不是高通 changelog）：
  - 新增一段跨 SQE、BV、LPAC、DDE 的抢占保存/恢复状态机（SQE `0x3556..0x3615`）：读取 `AQE0_PREEMPT_BASE` 并通过 `DDE_PM4_WRITE` 进入保存路径，等待 WFI 与内存写完成后，在恢复路径中使用 `MEM_READ_ADDR_HI_PREEMPTION`。BV（`0x28ce`、`0x28d5`）和 DDE（`0x085a`、`0x0867`）各新增对应入口。
  - WFI 完成等待的辅助函数从 `0x075f..0x0762` 迁到 `0x077b..0x077e`，旧版 PC 不能直接套到新版上。
  - 启动时按 pipe 分别配置 `CP_APRIV_CNTL_PIPE`。
  - 一组与 FDM 相邻的寄存器范围被重定向。
- **文档自己的结论：** 新固件压测中仍出现 GPU 先挂死、2D page fault 和 `CP Protected mode error`，这次 SQE 更新没有消除所有 GPU 问题。

**与仓库记录的矛盾（待用户确认）：** [Swan 固件分析](swan-cp-opcode-firmware-20260930.md)里 09-28～29 的 7 份故障快照（A 类 4 份，DDE 的 B 类 devcd13/14/15 共 3 份），`SQE_VERSION` 全部是 `0x01510103`，正是上面的"新固件"。所以 0103 这次更新并没有消除 Swan 上的 DDE opcode=0。可能的解释是：

- 真正修复 DDE 故障的是比 `0x01510103` 更晚的固件；
- 或者 0103 只降低了频率。

需要用户提供实际修复所对应的固件版本号与 SHA。

**已知固件版本：**

| 设备 | SQE | 其他 | 来源 |
|---|---|---|---|
| Swan（A840，gen8_2），修复前 | `0x01510095` | AQE `0x01510026`，GMU core 5.3.25 / HFI 2.7.1 | 上述内部资料 |
| Swan，09-28～29 故障快照 | `0x01510103` | 同上 | [Swan 固件分析](swan-cp-opcode-firmware-20260930.md) |
| Odin3（A830，gen8_0），10-11 | 版本字 `01500081 / 01006d44 / 018bc081 / 01003238 / 0152eb01`，`gen80000_sqe.fw` SHA `9c8f009b…` | AQE `01500022`，GMU core `50010019` | [Odin3 固件分析](odin3-kh3-firmware-analysis-20261011.md) |
| mesa#16460 报告者（A840 手机） | — | GMU 试过 `0x50030017`、`0x50030019`、`0x50030027`、`0x50030030` | 4.1 |

**对本问题的影响：**

- 高通改动的位置正是**跨 pipe 的抢占保存/恢复，包括 DDE 的状态交接**，与 fault36 中"DDE 瞬时读到全零包头"的形态一致。这支持 H1（抢占恢复不完整）。
- mesa#16460 / !44888 中"开关抢占无效"不足以排除 H1。CodeLinaro `f48b371a` 表明 GMU 会动态修改抢占级别，Swan 上也观察到请求 style 1 后仍被 L1 抢占（[Swan 固件分析](swan-cp-opcode-firmware-20260930.md)）。所以 KGSL/GMU 级别的开关未必真的阻止了 SQE 在 IB 中途抢占。
- 相比之下，B 组的 `cmd_no_preempt` 是在命令流里用 `CP_SCOPE_CNTL(disable_preemption)` 由 SQE 直接执行，更能检验 H1。gen80200 `0x01510103` 已确认实现了这个包；Odin3 的 gen80000 未单独核对，但上游自 `68163a732e6` 起在 gen8 上为性能查询发出同一个包。
- 手机无法通过更换固件解决：A830 与 A840 的固件文件不同（`gen80000_*` 与 `gen80200_*`），而且替换 vendor 固件要改动系统分区，不能作为用户方案。因此手机上只能在驱动侧规避，下面 A/B 的目的随之改为**找出在 Odin3 上有效、代价最小的规避**。
- 如果拿到 gen80200 新旧 SQE 原文件与 QRISC 反汇编工具（资料在另一台机器的 `qualcomm_gpu_driver` 和 bug_reports 中，本机没有），可以比较 Odin3 的 gen80000 SQE 是否缺少同样的 DDE 交接入口，从而确认手机固件确实处在修复之前。
- **线索，未建立因果：** 新固件按 pipe 配置 APRIV。Odin3 KH3 上未解决的片头 `CP Protected mode error | WRITE | addr=0x00000`（[KH3 #11](../../../bugs/kh3-cusa15072/README.md)）与这项改动属于同一功能面，内部资料也只建议做新旧固件 A/B。

### 6.2 假设

| 假设 | 支持 | 反对或不确定 |
|---|---|---|
| H1：L1 中途抢占恢复后，DDE 或 draw-state 状态不完整 | 高通给 Swan 的 SQE 更新正是改动跨 pipe 抢占保存/恢复与 DDE 交接（6.1）；我们已证实 A8XX 计算状态在 L1 中途抢占后不恢复（fault 62/63）；重负载时 GPU 工作更长，被抢占的次数更多 | mesa#16460 / !44888 中开、关抢占都不改变故障，但那是 KGSL/GMU 级的开关，未必阻止 SQE 中途抢占（6.1）；fault36 快照看不到抢占恢复 |
| H2：把命令流存储释放给内核（`GPUMEM_FREE_ID`，含 IOVA 解映射和 TLB 维护）会让 GPU 随后读到瞬时的零或错误内容（内核、固件或硬件缺陷） | !44888 与 WinNative 0006 的 A/B；blob 驱动在同一内核上同样出错；stream submit 和重负载都会增加命令缓冲的回收与释放 | 维护者认为这只是在绕开 UAF；根因不明 |
| H3：栈里某处有 use-after-free（对我们而言，主要是命令缓冲在 GPU 完成前就被复用或释放） | DDE 读的是 draw state，绑在命令缓冲或 pipeline 上；stream submit 是新的提交、回收路径，在途的命令缓冲更多；回收缓存恰好能掩盖 UAF | shadPS4 游戏中不销毁 pipeline；Android 上命令缓冲复用已要求 GPU 完成并且 host 提交返回（09-17 修复） |

用户指出了两点，作为本轮的判断依据：

- **"重负载下更容易发生"**：三个假设都与此相容，单靠这一点无法区分它们。
- **"改 nolrz 本质可能都是 GPU 跑得更慢了"**：nolrz、sysmem、关闭 stream submit 都会改变 GPU 时序，它们让故障变少不能作为根因证据。因此对照只改变一个机制，不改变 GPU 的速度。

## 7. 对照方案（待设备空闲后执行）

**条件：**

- 同一 APK（`97d67aa9…`），各组只切 `debug.mesa.tu.debug`。
- 每组前强停我们自己的 app：宿主在进程内只把属性合并进 `TU_DEBUG` 一次，驱动也只加载一次。
- 进入 GoW3 存档后，用 DebugBus 打开 `upload_diag stream_submit 200` 作为快速复现（此前两次都在 1–2 分钟内挂死），然后循环走动加攻击。
- 发生故障（faultcount 增加或进程退出）即停止，收集 dmesg（root）、logcat 命中和 KGSL 快照。
- 每组都从驱动日志核对实际生效的 `TU_DEBUG`、开关提示行，以及加载的驱动 SHA。

| 组 | `debug.mesa.tu.debug` | 检验 |
|---|---|---|
| A | 空（宿主仍会追加 `gmem_warmup`） | 新驱动下是否仍能快速复现（基线） |
| B | `cmd_no_preempt`（每个 primary 命令缓冲首尾用 `CP_SCOPE_CNTL` 关、开抢占） | H1 |
| C | `cs_recycle` | H2 / H3 |

**判定：**

- 先确认 A 组仍在 1–2 分钟内复现；否则先查新驱动（含 `c88b6ebd383`）是否改变了行为，再决定后续。
- B 组或 C 组要连续跑过 A 组复现时间的 5 倍以上不出故障，并按 A→B→C 轮换重复，才算有效。
- 单次没出故障不作结论。

**后续（视结果而定）：**

- 若 C 组有效，H2 与 H3 仍需区分。在 A 组条件下加 `TU_DEBUG=check_cmd_buffer_status`（Turnip 已有：命令缓冲在 GPU 仍为 ACTIVE 时被 reset 就会报告），审计 stream submit 路径下命令缓冲的回收时机。注意：故障发生后 KGSL 的时间戳不可信（见 4.1），只有故障前的报告才有意义。
- 必要时在 Android 上挂 VVL 跑一轮。
- 核对 Odin3 内核（gen8.0）的 DDE ROQ 转储大小（见 4.4）。

**脚本**（本地 `build/validation/preempt-ab-20261017/scripts/`，未入库）：

- `gow3_ab.py prep|run|restore`；
- `gow3_soak.py`（走动、攻击，监视故障）；
- `gow3.py`（DebugBus 手柄、截图、状态）；
- `rootsh-odin3.sh`（rootctl 执行脚本）；
- `wait_device_free.py`（等待共享设备连续 20 分钟无其他任务）；
- `cs_recycle_patch.py`（cs_recycle 的源码改动）；
- `turnip-csr.sh` 与 `wsl-turnip-prepare.sh`（WSL 构建）。

## 7.1 实测结果（2026-10-18）

**环境：** Odin3，GoW3 冥界存档（10-17 21:07，"哈帝斯的領域"），宿主相同。只换 APK：

- `97d67aa9` / `8778fd08`：新驱动 `ca038cc10f1`（含 `c88b6ebd383`，cs_recycle 关闭）；
- `b6ce482d`：旧驱动 `65b63f66bf5`，即 10-17 挂死时的版本。

`debug.mesa.tu.debug` 为空，宿主自动追加 `gmem_warmup`。脚本测试的做法是在存档点附近循环走动加攻击。

| 时间 | 驱动 | 倍率 | stream_submit | 操作 | 结果 |
|---|---|---|---|---|---|
| 10:12–10:22 | 新 | 0.75 | 200 | 脚本，10 分钟 | 无故障（约 50 FPS，GPU 90% / 1.10 GHz） |
| 10:31–10:37 | 旧 | 0.75，10:34 起改为 1.0 | 200 | 脚本，约 5.5 分钟后停止（中途改了倍率） | 无故障 |
| 10:40–10:50 | 旧 | 1.0 | 200 | 脚本，10 分钟 | 无故障 |
| 10:51–10:59 | 旧 | 1.0 | 0（默认） | **用户实际游玩**，约 8 分钟 | **fault 37：`CP DDE BR opcode=0`** |

结论：

- 原地走动的脚本负载复现不了，用户的实际游玩可以复现。
- 不开 stream submit 也会出现 DDE opcode=0。stream submit 是不是另一类挂死的诱因（10-17 那两次签名是 GMU 抢占故障），仍待复现。
- 后续对照改为由用户在高频复现场景游玩，脚本在后台监视 faultcount 并自动收集证据（`fault_watch.sh`）。

**fault 37 快照**（`build/validation/preempt-ab-20261017/fault37/`，SHA `78109759…`，PID 25234，ctx 21，ts 78671）：

- 签名与 fault36 相同：DDE_BR 固件停在 `0x717`，寄存器 19 = `0xbad4`（全零包头陷阱）；BR 在 `CP_SET_DRAW_STATE` 中等 DDE（PC `0x391`）。工具把这版 DDE 固件的抢占保存、恢复入口标在 `0x05f4` 和 `0x061c`。
- BR IB2 `0x412f85d70c` 的取指位置为 10013，ROQ 中还剩 486 个 dword 未执行，因此执行点约在 9527，即 dword 9526 那条 `CP_SET_DRAW_STATE`。
- 这条包绑定整条管线，组 0、1、2、3、4、5、7、8、18、26，分别是 PROGRAM_CONFIG、VS、VS_BINNING、HS、DS、GS、VPC、FS、PRIM_MODE_GMEM 和一个动态状态组。**组集合与 fault36 那条完全相同。**
- 这次只有组 3/4/5 的载荷被冻结；其余组标为 `not_frozen_but_memlist_mapped`，内容看不到。
- 组 0 位于 128 KiB 分配 `0x411bbea000` 的偏移 0 处。但这块被快照中 31 条 draw-state 包引用，含多条管线，所以**不能**据此说这是刚建的管线。
- 39,094 个载荷都没有跨 4 GiB。
- 6 条抢占切出记录都是历史记录，没有一条在当前 IB1 中，也都不是 IB 中途切出。与 fault36 一样，看不到故障前发生过抢占恢复。

**Odin3 固件结构核对**（原始文件与反汇编只存在本地 `build/validation/preempt-ab-20261017/firmware/`，不入库）：

- 从设备读出的 `gen80000_sqe.fw` SHA 为 `9c8f009b…`，与 10-11 的记录一致。用本仓 Mesa 树在 WSL 中编出的 `qrisc-disasm` 反汇编，版本 `01500081`。
- 四个 image 都有 `CP_SCOPE_CNTL` 处理入口。主 SQE 中禁止抢占的分支会保存并清除 `PREEMPT_ENABLE` 的第 0 位，重新允许时恢复。所以 B 组的 `cmd_no_preempt` 在 Odin3 上确实生效。
- 这版固件已有 `AQE0_PREEMPT_BASE`（20 处）和 `DDE_PM4_WRITE`（78 处）的引用，说明 AQE 参与的抢占逻辑本来就在。它缺不缺 Swan 那次补上的部分，需要拿 gen80200 新旧两版固件对照（本机没有）。

**`c88b6ebd383` 在 A8XX 上的实际含义：**

- 本仓寄存器表中，A8XX 变体的 `SP_UPDATE_CNTL` 只有第 0–5 位（各阶段 STATE），没有 bindless 字段。
- 所以旧代码在 A8XX 上写入的 bindless 位全被丢掉，实际写的是 0，**bindless 描述符缓存从未失效**；修复后改写 `SP_CS/GFX_BINDLESS_INVALIDATE`。
- 这会造成读到旧描述符的正确性问题，是否与挂死有关尚无证据。

## 7.2 用户场景复现与 B 组（2026-10-18）

用户在 GoW3 冥界区域找到了高频复现场景。Odin3 装新驱动 APK `8778fd08`，倍率 1.0，`stream_submit` 为默认值 0，由用户实际游玩，后台 `fault_watch.sh` 自动收集证据。

| 会话 | `debug.mesa.tu.debug` | 启动 → 结果 |
|---|---|---|
| PID 27686 | 空（A） | 11:02:52 启动，**11:04:00 崩溃（fault 38）**，去掉读档时间，实际游玩不到 1 分钟 |
| PID 29419 | `cmd_no_preempt`（B） | 11:10:17 启动，**11:45:49 出现 fault 39：`CP DDE BR opcode=0`**，约 35 分钟 |
| PID 7853 | `cs_recycle`（C） | 11:50:18 启动，到 13:08 约 **78 分钟实际游玩无故障**（画面 30 FPS，GPU 74%，推进到冥界新区域，红魂 344 → 4750）。13:08 之后角色停在原地不动（13:39 截图位置和红魂数都相同），游戏仍在渲染，到 13:39 仍无故障 |

C 组的开关由两处确认：

- 宿主日志 `Turnip TU_DEBUG=cs_recycle,gmem_warmup`；
- Turnip 启动行 `cs_recycle keeps retired command-stream BOs instead of GPUMEM_FREE_ID (cap 256 MiB)`（`armC-startup-logcat.txt`）。

运行期间没有出现缓存的运行统计（每入缓存 4096 次才打印一次）。15:54 通过会话菜单正常 Stop 后，Turnip 打印了最终统计（`armC-cs-cache-final.txt`）：

```
kgsl cs cache (final): 177 hits, 225 misses, 402 parked, 0 evicted, peak 132592 KiB
```

含义：

- 整个会话中 402 个命令流 BO 退役后没有 `GPUMEM_FREE_ID`。不开这个开关时，这 402 次都会把内存还给内核。
- 新分配中 177 次复用了缓存里的 BO，225 次需要新分配。
- 缓存峰值约 129 MiB，在 256 MiB 上限以内，从未淘汰。
- 退役频率平均每分钟不到 2 次（会话约 4 小时，含后段空闲）。即使只是其中少数几次释放引出故障，也足以解释不开开关时几分钟一次的故障频率。

这说明缓存确实在工作，与"把命令流内存还给内核会引出故障"（mesa!44888）一致。

**fault 40：C 组加 stream submit，进关即崩（推翻"C 组有效"）。** 同一进程 PID 7853 停止后在 16:58:33 开始第 2 次会话：

- Turnip 设备重建时再次打印 `cs_recycle` 启动行，开关仍生效；之后打开 `upload_diag stream_submit 200`。
- 16:59:56 读档，17:00:00–17:00:06 加载冥界关卡资源，**17:00:10 崩溃**：`CP DDE BR opcode=0`，ctx 16，ts 28227。快照在 `fault40/`，SHA `6bfb24c0…`。
- BR 的 SQE PC 为 `0x60e`，与 fault 38 相同，处在 `CP_DRAW_INDX` 中把 draw 交给 DDE 的循环里。DDE 停在 `0x717`、`$19=0xbad4`、`$01=0`。
- 执行点约在 IB2 `0x4116ade000` 第 551 个 dword。此前最近一次整条管线绑定在 dword 353，组集合**仍是 0、1、2、3、4、5、7、8、18、26**。这次所有载荷都被冻结，内容合法。组 0 在 `0x40629ac000` 块的偏移 `0xaf00` 处，不在块起始。
- 抢占记录仍全为历史记录，没有当前 IB1 的中途切出。

**修正后的结论：**

- C 组第一次 78 分钟无故障，不能说明 `cs_recycle` 有效。开着它照样在进关几秒内崩溃。
- 进关瞬间集中编译管线，命令量暴涨，`stream_submit` 又把一帧切成很多命令缓冲，这三者叠在一起时最容易触发。
- 至此 A、B、C 三种规避都没有消除 DDE 故障。GoW3 的四次 DDE 故障（36、37、39、40）出错位置都在"整条管线绑定之后的 SDS 或 draw"上，组集合相同。不过 GoW3 绑定管线时本来就用这组组号，这个共同点不一定有区分度。KH3 fault 53 的组集合未核对。

**为什么"原地走动不复现、实际游玩复现"：** Turnip 的命令流 BO 只在命令量增长时更替（`tu_cs.cc`）：

- 空间不够时开一个新 BO，大小翻倍；
- 命令缓冲 reset 时释放除最后一个以外的所有 BO；
- 命令缓冲销毁时全部释放。

画面稳定时命令缓冲一直沿用最大的 BO，几乎不释放。进入新区域、战斗、draw 增加时才开新 BO，并在随后 reset 时释放旧的。这与以下现象一致：

- 脚本原地走动 25 分钟以上不复现；
- 用户推进场景几分钟内复现；
- stream submit 把一帧切成更多命令缓冲后 1–2 分钟就挂死。但每组仍只有一次样本，还需要 A 组（新驱动、不加开关）在同一场景复核基线。

**fault 39**（`fault39/`，快照 SHA `cd612fad…`，PID 29419，ctx 24，ts 596915）：

- DDE_BR 停在 `0x717`，`$19=0xbad4`；BR 停在 `0x38f`，在 `CP_SET_DRAW_STATE` 中等 DDE。
- 执行点约在 IB2 `0x43b1699940`（475 个 dword）第 351 个 dword。此前最近一次整条管线绑定在 dword 252，组集合又是 0、1、2、3、4、5、7、8、18、26。组 0 再次位于 128 KiB 分配的偏移 0 处（`0x43d9737000`），管线载荷再次没被冻结。
- 这块分配被 68 处引用，含多条管线，比它地址更高的分配也很多，所以不是刚建的块。
- 抢占记录全为历史记录，没有一条在当前 IB1 中。

**B 组结论：** 在我们的命令缓冲内部关闭抢占（SQE 执行的 `CP_SCOPE_CNTL`），DDE 故障仍会出现，触发间隔变长，但样本太少，不能归因于开关。"IB 中途抢占恢复出错"的假设因此被削弱。命令缓冲之间的上下文切换不在 B 组控制范围内，那里 DDE 状态同样会被保存和恢复。

**三次 DDE 故障的共同点：** 出错的都是一条整条管线绑定，组集合相同（0、1、2、3、4、5、7、8、18、26）；其中两次组 0 的载荷位于 128 KiB 管线子分配块的起始位置。原因未明，先作为线索。（**10-18 下午更正**：按 DDE 寄存器反推，fault 40、41 实际出错的是命令流里的 VB 组，不是管线组；各次的共同点是读取进入 BO 第 0/1 页时得到 0，见 7.3。）

B 组的开关从宿主日志确认：`Turnip TU_DEBUG=cmd_no_preempt,gmem_warmup`。Turnip 初始化时的确认行已被 logcat 轮转掉（主缓冲只有 256 KiB），以后各组都在启动后立即保存 logcat。

**fault 38**（`fault38/`，快照 SHA `b13088a9…`，PID 27686，ctx 21，ts 10758）是另一种签名，与 10-17 两次 stream submit 挂死相同：

- dmesg 先报 `possible gpu syncpoint deadlock … Context 21 might be blocked for itself`（queue 10762，submit 10760，start 10758，retire 10757）。随后 GMU `Preemption Fault: cur=2 … next=1`，状态 `00FE1307`。
- 也就是时间戳 10758 已开始执行但一直没完成，GMU 从游戏所在的环切到更高优先级的环时超时。
- dmesg 中没有 IOMMU 缺页记录。
- BR 在 IB2 `0x414f2390fc`：取指位置 2497，ROQ 中剩 499，执行点约在 1998。这段 IB2 没被快照收进去，无法解码。
- 两个微引擎卡住的位置（按 Odin3 `gen80000_sqe.fw` 的反汇编核对 PC）：
  - BR 的 SQE PC 为 `0x60e`，在 `CP_DRAW_INDX` / `CP_DRAW_INDX_OFFSET` 处理函数中，正循环把这次 draw 的参数写进 `DDE_PM4_WRITE` 交给 DDE。BR 停在这里，说明 DDE 不再接收。
  - DDE_BR 的 PC 为 `0x36`，在 PKT4 处理函数的 `(rep) mov $data, $data` 上，即按包头计数把载荷写入寄存器时停住，等不到数据。
- 寄存器现场（`CP_SQE_STAT`）：DDE_BR 的 `$01`（当前包头）为 `0x40a9c785`，即从 `0xa9c7`（`SP_LB_PARAM_LIMIT`，之后是 `SP_REG_PROG_ID_*`）起写 5 个寄存器。近期包头还有 `0x48886601`（`RB_LB_PARAM_LIMIT`）和 `0x48a9a304`（`SP_PS_INITIAL_TEX_INDEX`），都是格式合法的片段着色器管线状态。
- 作为对比，fault 36/37 的 DDE_BR 是 PC `0x717`、`$19=0xbad4`、`$01=0`，即读到全零包头后落入陷阱。

**推断（未证实）：** fault 36/37 与 fault 38 可能是同一问题的两种表现。DDE 读取管线 draw-state 载荷时出错，读到 0 时落入非法包陷阱，读不到数据时卡死；三次都没有 IOMMU 缺页记录。这与高通给 Swan 的修复内容（抢占时 DDE 状态的保存和交接）相符，也与 B 组目前的结果相符。

## 7.3 重复复现与 DDE 实际读到零的位置（2026-10-18 下午）

**复现方式。** 用脚本 `dde_trial.sh` 自动跑：

1. 冷启动 GoW3，启动后立即开 `upload_diag stream_submit 200`（或不开）。
2. 通过菜单进入"載入"，用物理手柄 ○ 读最新存档，进入冥界存档点。
3. 角色原地不动，观察 180 秒。

全程开一个独立的 tracefs 实例（每 CPU 4 MiB，共 32 MiB），记录提交/退役、RB 级抢占、上下文切换、电源状态、BO 分配/释放和 GPU 故障；`adreno_gpu_fault` 上挂了 `traceoff` 触发器，故障瞬间冻结，事件无丢失（overrun 0）。证据在 `build/validation/preempt-ab-20261017/trials/`。

| 轮次 | `debug.mesa.tu.debug` | stream_submit | 结果（读档后） |
|---|---|---|---|
| fault 40（17:00） | `cs_recycle` | 200 | 约 14 秒，DDE opcode=0 |
| t1 | `cs_recycle` | 200 | 86 秒，**fault 41**，DDE opcode=0 |
| t2 | `cs_recycle` | 200 | 59 秒，**fault 42**，`CP BV opcode error \| opcode=0x70970001` |
| t3 | `cs_recycle` | 200 | 180 秒无故障 |
| t4 | `cs_recycle` | 0 | 180 秒无故障 |
| t5 | 空 | 200 | 180 秒无故障 |

`cs_recycle` 加 stream submit 4 轮中 3 轮在 90 秒内出故障，属于高频但不是必现（后续轮次复现率下降，合计约 1/3，见 7.4）。两组对照各只有 1 轮，还不能下结论。三轮无故障的截图都确认已进入关卡（约 48 FPS）。

**DDE 寄存器的含义（按 Odin3 固件反汇编核对）：**

- `$1a` 是 QRISC 的 `$sp`（`qrisc.h` 中 `REG_SP=0x1a`）。DDE 的 `CP_SET_DRAW_STATE` 处理函数（DDE PC `0xbd`）把 BR 转来的第一个数据字，即本批要执行的 draw-state 组数，放进 `$sp`；每个组的 IB 执行完（`IN_IB_END`，DDE `0xd0`）减 1。所以 `$sp` 是**本批还剩几个组（含当前组）**。
- `$13` 是 BR 的 draw 处理函数里每个 draw 加 1、取低 8 位的计数（SQE `0x615`），在 `0x607` 随 draw 包交给 DDE。它是 draw 序号，本身没有诊断意义。
- `CP_SQE_STAT` 的 word `0x24`/`0x25`/`0x26` 是最近三个包头（新到旧），`0x24` 就是读到的 0。
- `$19=0xbad4` 来自 DDE `0x712` 的分发入口（`jump` 的延迟槽 `0x713` 写入），最终停在 `0x717` 的死循环。
- `0x708a8000` 是 `IN_IB_END`（opcode `0x0a`），即一个组执行完的内部标记。
- fault 41 的 DDE ROQ（section 385）里能看到 BR 交给 DDE 的流：`70430002 0000000f 00000002`（`CP_SET_DRAW_STATE`，本批 15 个组）和 `7038800c …`（draw）。

**硬件 draw-state 表。** 快照的 `GEN8_CP_DRAW_STATE`（BR，section 377）是硬件当时记录的各组地址和大小。每个组 8 个 word，含两个槽（地址、大小），两个槽按批交替使用。拿 DDE 最近的包头去两个槽的载荷里匹配，就能确定 DDE 执行的是哪个组、哪个槽，以及读到 0 的那个 dword 的地址：

| 故障 | DDE 最近包头（旧→新） | 执行的组/槽 | 载荷 | 读到 0 的位置 |
|---|---|---|---|---|
| 36 | `VB[4]` → `IN_IB_END` → 0 | VB 组结束后的下一个组（推断为组 11 的 B 槽 `0x40f8bf700c`） | VB 的 B 槽在 BO `0x40f8bf6000` 第 0 页 | 下一个组的第 0 个 dword，位于该 BO **第 1 页**（偏移 `0x100c`） |
| 37 | draw → `CP_SET_DRAW_STATE` → 0 | 本批第一个组（推断为组 0 的 B 槽） | `0x41fb246000`，即 128 KiB BO 的**起点** | 第 0 个 dword，BO 第 0 页偏移 0 |
| 39 | `0x70b90001` → `CP_SET_DRAW_STATE` → 0 | 同上（组 0 的 B 槽） | `0x43d9737000`，128 KiB BO 的**起点** | 第 0 个 dword，BO 第 0 页偏移 0 |
| 40 | `CP_SET_DRAW_STATE` → `VB[0]` → 0 | VB 组（组 9）A 槽 | `0x414b036ff8`，20 dword，BO `0x414b036000`（2 MiB） | 第 4 个 dword；dword 2 起已在 BO **第 1 页** |
| 41 | `VB[0]` → `VB[1]` → 0 | VB 组（组 9）B 槽 | `0x41601c6fec`，20 dword，BO `0x41601c6000`（4 MiB） | 第 8 个 dword；dword 5 起已在 BO **第 1 页** |

几点核对：

- fault 40、41 中，另一个槽的同一位置都是合法包头（例如 fault 41 A 槽第 8 个 dword 是 `48a01883`），所以 DDE 执行的一定是跨页的那个槽。
- 快照里（KGSL 用 CPU 读）这些位置的内容都是合法 PM4。例如 fault 41 B 槽第 8 个 dword 是 `48a01883`（`VFD_VERTEX_BUFFER[2]`）。
- fault 41 的 DDE ROQ 中，该载荷 dword 1–4（第 0 页的最后 16 字节）是正确数据，从 dword 5（第 1 页开头）起全是 0。被吃进去的前几个 0 被当作上一个 PKT4 的数据，到下一个包头位置才触发 opcode=0。
- 跨页前后两页属于**同一个 KGSL 分配**（`memory_allocations` 核对），两页之间不存在 Turnip 自己拼接的问题。
- fault 38（卡死）的管线载荷没有被冻结，无法定位 DDE 卡住的那个 PKT4（`0x40a9c785`）在哪个 dword。

**结论（确认部分）：** 五次 DDE opcode=0 故障，零数据都从 DDE 的读取**进入一个新的 4 KiB 页**时开始，而且这一页都是所在 BO 的第 0 页或第 1 页。CPU 看到的同一位置内容正确，dmesg 也没有 IOMMU 缺页。之前"出错的都是整条管线绑定"的说法不准确：fault 40、41 出错的是命令流里的 VB 组，不是管线组。

**BO 属性（trace 中的 `kgsl_mem_alloc`）：**

- fault 41 的 4 MiB BO 由 `VkRecord` 线程在故障前 **51 秒**分配，flags `0x010c0000`，即 `GPUREADONLY | CACHEMODE_WRITECOMBINE | 4 KiB 对齐`。
- 128 KiB 的块同为 `0x10c0000`。
- shadPS4 自己申请的 64 KiB 内存是 `0x8c0c0000`（`IOCOHERENT | WRITEBACK`）。
- Turnip 的内部 BO（命令流、管线 suballoc）走 `tu_bo_init_new`，不带 `HOST_CACHED`，因此是 write-combine。

**trace 观察（t1 / fault 41）：**

- 约 130 秒内有 15112 次 RB 级抢占，游戏 rb2 与合成器 rb0 交替，以 level 0 为主，偶有 level 1。
- 出错的提交（ctx 18，ts 42361）在**提交后 175 µs** 就报了故障。前一次提交 ts 42360 是在切回 rb2 之后正常完成的，所以不存在"当前 IB 中途被抢占"。
- 故障前 4.7 秒内没有 `kgsl_mem_free`。这一轮没开 sparse bind（`kgsl_mem_add/remove_bind_range`）和 SMMU TLBI 事件，所以还不能排除稀疏绑定引起的 unmap。

**fault 42（BV，另一种签名）：**

- BV 的 `$01` 是 `0x70970001`，即 `CP_THREAD_CONTROL`（opcode `0x17`）；最近包头依次是 `CP_THREAD_CONTROL`、`CP_CONTEXT_REG_BUNCH`（18 dword）、`CP_THREAD_CONTROL`。
- BV 当时的 IB1 是 `0x40e0e47000` 上 5 个 dword 的时间戳写入（`CP_EVENT_WRITE7`），已经取完；IB2 寄存器里留的是一个 KGSL 全局缓冲区（`0x1ff8002a36190`，长度 0）的旧值。说明 BV 当时在执行 ringbuffer 这一级的命令。
- 签名与 KH3 fault 75 相同（`bugs/kh3-cusa15072`）。与 draw-state 跨页无直接关系，单独记录，未分析。

**新假设 H4（未证实）：写入方 CPU 的 write-combine 缓冲还没排空，GPU 就读了。**

- 机制：命令缓冲由 `VkRecord` 线程录制、管线由编译线程创建，两者都写 WC 内存；提交在另一个线程、可能是另一个 CPU 上执行 ioctl。线程交接只靠锁和条件变量（`DMB ISH`），只保证其他 CPU 看到的顺序；KGSL 提交前的 `wmb()` 也只作用于提交方自己的 CPU。如果 GPU 读得足够早，就会读到 DRAM 里的旧内容。
- 支持的证据：零数据都在 BO 开头，而命令流长大时 Turnip 会新开 BO 从头写，新管线也写在新块开头，这些正好是提交前最后写的数据；`stream_submit` 让提交紧跟录制；快照里内容正确且没有缺页。
- 不能解释的地方：fault 41 的 BO 已经用了 51 秒，第 1 页在 DRAM 中应是上一次录制的内容，不一定是 0；零数据恰好从页边界开始，单靠写缓冲排空也说不通。
- 验证：Turnip `10f23b2bdc0`（本地分支 `codex/a8xx-cs-recycle`，**未推送**）新增默认关闭的 `TU_DEBUG=cs_dsb`，在 `vkEndCommandBuffer` 末尾和图形/计算管线创建结束时，于写入方线程执行 `DSB SY`，开启时打印 `Turnip: cs_dsb issues DSB SY …`。驱动 SHA `3e3e29c0…`。结果见 7.4：**H4 不成立**。

## 7.4 cs_dsb A/B、故障 43–48 与外部调研对照（2026-10-18 傍晚）

### cs_dsb A/B

APK `d2423a4d…`（驱动 `3e3e29c0…`，含 `cs_recycle` 与 `cs_dsb`）经用户同意装到 Odin3，按 7.3 的流程交替跑 D（`cs_recycle,cs_dsb`）和 C（`cs_recycle`），每轮读档后观察 180 秒。两组的 host 日志都确认了实际生效的 `TU_DEBUG`，`cs_dsb` 组 logcat 有对应打印。

| 轮次 | 组 | 结果 |
|---|---|---|
| d1 | D | 无故障 |
| c1 | C | +44 s，**fault 43**，DDE opcode=0 |
| d2 | D | 无故障 |
| c2 | C | +120 s，**fault 44**，DDE opcode=0 |
| d3 | D | +97 s，**fault 45**，DDE opcode=0 |
| c3 | C | 窗口内无故障；窗口结束约 9 秒后 **fault 46**（BV，游戏仍在运行） |
| d4 | D | 窗口内无故障；窗口结束约 7 秒后 **fault 47**（Protected mode error，游戏仍在运行） |
| c4 | C | 无故障 |

`cs_dsb` 组照样出现 DDE opcode=0，且位置特征与其他故障相同（见下），所以"写入方 CPU 的 write-combine 缓冲没排空"这一假设不成立。

证据目录：D 组为 `trials/d1–d4`；C 组原为 `trials/c1–c4`，后续 syncdraw A/B 的脚本复用了 `c1–c4` 这几个标签，已把这一批改名为 `trials/dc-c1–dc-c4`。改名前 `c1` 的 trace、host 日志、logcat 和两张截图已被新一轮覆盖（`dc-c1/NOTE.txt`）；fault 43 的快照、dmesg 和数据库都还在，trace 的末段和事件统计在覆盖前已经提取，记在本节。

### 三次 DDE 故障读到零的位置

| 故障 | 组 | DDE 最近包头（旧→新） | 读到 0 的地址 | 所在 BO | BO 分配到故障 |
|---|---|---|---|---|---|
| 43 | C | `CP_SET_DRAW_STATE` → `VB[0]` → 0 | `0x415e507008` | `0x415e506000`，4 MiB，**第 1 页**偏移 `0x008` | 25.1 s |
| 44 | C | `VB` → `IN_IB_END` → 0 | `0x415e977218`（组 11 B 槽首 dword） | `0x415e976000`，4 MiB，**第 1 页**偏移 `0x218` | 99.6 s |
| 45 | D | `VB` → `IN_IB_END` → 0 | `0x415bd87218`（组 11 B 槽首 dword） | `0x415bd86000`，4 MiB，**第 1 页**偏移 `0x218` | 77.7 s |

- fault 43：VB 组 A 槽 `0x415e506ff8` 长 20 dword，跨页，第 4 个 dword（包头）读到 0。
- fault 44、45 的布局相同：VB 组 B 槽在 BO 第 0 页 `0xf6c`，执行完（`IN_IB_END`）后，下一个非空组是组 11 的 B 槽，在第 1 页 `0x218`，首 dword 读到 0。`$sp=3`。
- 三个 BO 都是 `VkRecord` 线程在读档后约 18–20 秒（关卡加载完、命令缓冲长到最大时）分配的，flags `0x010c0000`，故障时已用了 25–100 秒。快照抓下了整个 4 MiB，有效内容约 2.2–2.4 MiB（543–605 页），即当前这一帧的录制；第 0、1 页在快照里都是完整的 PM4。
- 加上 7.3 的五次和下文的 fault 49，九次 DDE opcode=0 中有七次在 2/4 MiB 命令流 BO 的**第 1 页**读到 0（偏移 `0x1000`–`0x1218`），两次在 128 KiB BO 的起点。

对"写缓冲没排空"的进一步反驳：开启 `cs_recycle` 后，同一个 BO 每帧都从头重录，第 1 页在一帧里是最早写入的部分之一，比它晚写的第 2–540 页都读对了；而且故障前这个 BO 已经被 GPU 正确读了几十秒、上千帧，第 1 页在 DRAM 中早已是有效内容，不会是全 0。零数据应来自 GPU 侧的读取路径（地址转换、GPU 内部缓存或 DDE 预取），而不是 CPU 写入没到内存。

### 不是 SMMU 缺页

- Odin3 的 `ft_pagefault_policy=0x0`：按 KGSL 源码，这表示不 stall、不停 GPU（`CFCFG=0`、`HUPCF=1`）。但 `kgsl_iommu_print_fault` 在任何策略下都会先发 `kgsl_mmu_pagefault` tracepoint，再打限流的 `GPU PAGE FAULT` 日志（CodeLinaro `5816f6e6` 版 `kgsl_iommu.c`，blob SHA `e4eac8fc…`，即 Odin3 模块已含的那个提交；本地副本 `external/kgsl-src/kgsl_iommu.c`）。
- 7.3 起每轮 trace 都开了 `kgsl_mmu_pagefault`，从未出现；dmesg 当前保留的范围（约 17:24 之后，覆盖故障 41–48）没有一条 `PAGE FAULT`。因此这些零数据**不是**地址转换失败后 SMMU 返回的零。
- 另一个发现：`/sys/class/kgsl/kgsl-3d0/pagefaults` 记录本次开机以来 shadps4.android 有 **491,333 次读、地址转换类**的 GPU 缺页，发生时间不明（18:31–18:40 间没有增加，dmesg 保留范围内也没有对应日志）。从 c2 起每轮在开始、停止和故障时都记录这个计数。
- trace 自 c2 起加开 `arm_smmu` 的 `map_pages`/`unmap_pages`/`iommu_pgtable_add`/`iommu_pgtable_remove`。

### 时序

三次故障中，出错的提交都是 GPU 空闲后的第一个提交（`inflight=1`，此前刚从 rb0 切回 rb2），提交后 184–404 µs 报故障。但所有提交中有 42–46% 是 `inflight=1`，这一点区分度不够，只作记录。

### BV 签名（另一类）

| 故障 | 组 | BV | BR |
|---|---|---|---|
| 42 | C | PC `0x1f4`，`$01=0x70970001`（`CP_THREAD_CONTROL`），最近包头 `CP_THREAD_CONTROL` / `CP_CONTEXT_REG_BUNCH(18)` / `CP_THREAD_CONTROL` | PC `0x362`，`$19=0xbad4`，最近包头 `CP_CONTEXT_REG_BUNCH(16)` → 0 → 0 |
| 46 | C | 同上 | PC `0x35c`，`$19=0xbad4`，最近包头 `0x40823101` → `0x48880001` → 0 |
| 48 | E（syncdraw） | 同上，最近包头 `0x70d58003` ×2 → `CP_THREAD_CONTROL` | PC `0x74a`，正常 |
| 47 | D | dmesg：`CP \| Protected mode error \| WRITE \| addr=0x00000`，BV IB2 寄存器为 KGSL 全局缓冲 `0x1ff8002a36190` 的旧值 | 无快照 |

- 新发现：fault 42、46 中 BR 的 SQE 也停在非法包陷阱（`$19=0xbad4`，最近包头为 0），即 BR 同样读到了零包头，只是 KGSL 报的是 BV 的错。读到零的具体地址还没定位。
- fault 46 的快照是 18:21 才拉的（当时 faultcount 已是 47），按 BV IB1（`0x40e4106000`，剩 `0x14d7` dword）和 ctx 16 ts 78895 核对，它属于 fault 46。KGSL 只保留第一份未被读取的快照，因此 fault 47 没有快照。证据在 `fault46-47/`。
- 脚本只在观察窗口内检测故障，窗口外的故障不会收集；后续需要在每轮开始时先读掉残留快照。

### 外部调研的结论与本地数据对照

另一台机器（有 Swan 完整固件与 UMD）的调研结果（`odin3_gow3_dde_firmware_umd_20261018.md`，未入库）要点：

- 855.0.9 包里没有更新的 Swan SQE；gen8.0 也有同一个陷阱路径。
- `$1a`/`$sp` 是剩余 SDS 组数，`$13` 是 8 位 draw 序号，与本文 7.3 一致；但不能用 `$sp` 推组长度或越界。
- 高通 UMD 的 SDS 发包函数按组复制控制字和地址，不在组之间插 WFI/WFM，不做对齐或填充；组控制字 bits 16–23 是渲染模式与 DIRTY 标志。没有找到针对 A8xx DDE 的统一规避措施。
- 推荐的诊断顺序：先用现有 `TU_DEBUG=syncdraw`（每个 draw 前 `WAIT_MEM_WRITES`、`WAIT_FOR_IDLE`、`WAIT_FOR_ME`）做 A/B；仍失败再加一个补丁，在 `tu_CmdBindPipeline` 立即发 pipeline SDS 的分支前也插这三条等待。
- 与本地数据的分歧：调研认为"不能用存档点站立代替实际路线"，但 7.3/7.4 的数据表明，原地站立加 stream submit 也能复现（合计约 1/3，前期更高），不必依赖实际路线；复现率不稳定才是 A/B 的主要障碍。

### syncdraw A/B：本批 C 组未复现，无法判断

同一个 APK，交替 E（`cs_recycle,syncdraw`）和 C（`cs_recycle`），各 4 轮（18:26–18:57）。host 日志确认 E 组实际 `TU_DEBUG=cs_recycle,syncdraw,gmem_warmup`。

| 组 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| E（syncdraw） | +86 s **fault 48**（BV `0x70970001`） | 无 | 无 | 无 |
| C（对照） | 无 | 无 | 无 | 无 |

对照组 4 轮全无故障，所以这一批不能说明 syncdraw 有没有作用；只能确定 syncdraw 不能消除 BV 签名。从 c2 起 trace 额外开了 `arm_smmu` 的页表事件，USB（adb）的 `map_pages` 很多，cpu0 有溢出（只影响最早的事件，故障时冻结的末段完整）。

### 复现率需要修正

把所有脚本化的 `cs_recycle` + stream submit 200、读档后看 180 秒的轮次合起来：

| 组 | 轮数 | 窗口内故障 | 窗口外几秒内 |
|---|---|---|---|
| C（只开 `cs_recycle`） | 15（t1–t3、dc-c1–4、c1–4、r1–r4） | 5（DDE 41/43/44/49，BV 42） | 1（BV 46） |
| D（加 `cs_dsb`） | 4 | 1（DDE 45） | 1（47，保护模式写） |
| E（加 `syncdraw`） | 4 | 1（BV 48） | 0 |

复现率随时间下降：17:00–18:10 的前 7 轮 C 组有 4 轮出故障，18:29 之后的 4 轮一次都没有。原因未查明（可能与设备温度/频率、调试事件开销有关，都没有记录）。7.3 中"4 轮中 3 轮"是前期数据，偏高；按全部数据是 C 组约 1/3。三组的差别都在随机波动范围内，**cs_dsb 和 syncdraw 都没有表现出效果，也不能排除效果**；要下结论需要先恢复稳定复现。

### GPU 页表事件

`arm_smmu` 对 GPU 上下文（`gfx3d_user`）只有 `iommu_pgtable_add/remove`（L3 页表页的分配和释放，每页管 2 MiB VA），没有 `map_pages`/`unmap_pages`，因为 KGSL 直接操作自己的 io-pgtable。一轮约 5000–6000 次，主要来自 GpuComm（约 2100 次）和 Guest-1（约 400 次），即游戏运行中 L3 页表在不断建立和回收。

一个待验证的猜测：如果某个 2 MiB 区域的 L3 页表被回收后又被复用，而 SMMU 的页表遍历缓存仍指向旧页表，GPU 会按旧页表翻译到别的物理页。这会读到错误内容而不产生缺页，与"内容在 CPU 侧正确、无缺页"相符；但这也需要旧缓存项活过几十秒，可能性不高。检验方法是在新故障的 trace 里看故障地址所在 2 MiB 区域（如 fault 43 的 `0x415e400000`）在故障前是否有页表回收/重建。还没抓到带这些事件的 DDE 故障。

### C 组复查（r1–r4）与 fault 49

同一 APK 只跑 C 组 4 轮（`trials/r1–r4`，19:00–19:13）：r3 读档后 45 秒出 **fault 49**（DDE opcode=0），其余 3 轮无故障。C 组合计 15 轮 5 次。脚本现在每轮开始先读掉残留快照（`stale_drain.sh`），并记录 GPU 频率与温度；但记录点在上一轮游戏还在运行时（例如 1100 MHz、83 °C），不是本轮关卡中的状态。

fault 49 与 44、45 的布局相同：VB 组（组 9，A 槽 `0x41602b6ae4`，24 dword）在 4 MiB BO `0x41602b6000` 的第 0 页，之后执行的组 11 在第 1 页 `0x41602b718c`，读到 0；`$sp=7`。

### 零数据出现在本次提交第一次读第 1 页的时候

在出错的 IB2 里按顺序列出所有 `CP_SET_DRAW_STATE` 引用的地址（只看未禁用的组），fault 43、44、45、49 都是同一情形：

| 故障 | 出错的 SDS（IB2 dword） | 此前引用过的页 | 出错时的读取 |
|---|---|---|---|
| 43 | 450 | 只有第 0 页（446 处的单组 SDS 把组 11 设到第 1 页，但组 11 在组 9 之后执行） | 组 9（VB，第 0 页 `0xff8`）跨入第 1 页 |
| 44 | 441（本命令缓冲第 10 个 SDS，第 5 个 draw） | 只有第 0 页 | 组 9 结束后执行组 11，第 1 页 `0x218` |
| 45 | 441（同上，与 44 逐项相同） | 只有第 0 页 | 同上 |
| 49 | 391（第 5 个 SDS，第 2 个 draw） | 只有第 0 页 | 组 9 结束后执行组 11，第 1 页 `0x18c` |

也就是说，每次都是 **DDE 在本次提交里第一次读这个 BO 的第 1 页**时读到全零。每次录制都从 BO 偏移 0 写起，所以第 0 页总是在提交一开始就被读到，第 1 页要到几个 draw 之后才第一次被读。上一节的统计（六次在第 1 页）也都符合这一点。组的执行顺序按组号（与硬件组表的槽一致），所以前一个单组 SDS 里设到第 1 页的组 11 要等组 9 执行完才轮到。

可能的解释（都未证实）：

- 第 1 页的翻译在本次提交中第一次需要做页表遍历，而 DDE 在遍历未完成时拿到了零数据。提交之间有大量 TLB 失效，例如 GpuComm 释放 64 KiB 缓冲时会做 `tlbi`，fault 49 前 2.7–2.9 秒就有一批。
- 第 0 页在提交开头被访问时 GPU 较空闲，第 1 页的第一次访问发生在 draw 流中间，同时有其他访问。

**页表历史（fault 49）**：这个 BO 所在 2 MiB 区域（`0x4160200000`）的 L3 页表页在 BO 分配前 1.9 秒（故障前 24.3 秒）由 VkRecord 建立，用的物理页 `c73880000` 2.75 秒前刚作为区域 `0x4103c00000` 的页表被 GpuComm 回收。故障前 3 秒内这个区域没有页表的建立或回收，附近只有其他区域的 64 KiB 分配/释放及对应的 `tlbi`。因此"故障时刻页表被改写"不成立；页表页复用只发生在 BO 创建时，之后这个 BO 被正确读了约 22 秒。

**可做的验证（需要新驱动）：**

1. 命令流 BO 跳过开头两页再写：若零数据跟着移到新的"第一次读的页"，问题与"提交中第一次访问某页"有关；若消失，则与 BO 开头的页本身有关。
2. 在一个 SDS 首次引用某页之前，先用 CP（BR）读一次该页（例如一条只读该页的 `CP_MEM_TO_MEM`），让翻译先进入 TLB。若故障消失，可作为规避方案继续评估开销。

## 7.5 `cs_skip_head` / `dde_touch` A/B 与 fault 50–52（2026-10-18 晚）

两个开关都编进 Turnip `84db376cfde`（本地，未推送；驱动 `34f92ded…`，APK `3a4e0cf4`，host `fe5dcb7e`），默认关闭：

- `TU_DEBUG=cs_skip_head`：64 KiB 及以上的命令流 BO 前 8 KiB 不用，录制从偏移 `0x2000` 写起。检验零数据是跟着"第一次读的页"移动，还是固定在 BO 的第 1 页。
- `TU_DEBUG=dde_touch`：命令缓冲里某个 4 KiB 页第一次被 draw state 引用时，在下一个 draw 之前插一条 `CP_WAIT_REG_MEM`（mask 0，总是满足），由 BR 读一次该页的起始地址。即 7.4 末尾的第 2 项。

同一 APK 按 C（`cs_recycle`）/ F（加 `cs_skip_head`）/ G（加 `dde_touch`）轮换 6 轮（`trials/k1c`–`k6g`，20:05–21:06），流程同前（stream submit 200，读档后看 180 秒）。每轮进关后记录的 GPU 状态都是 1.1 GHz、77–82 °C、`thermal_pwrlevel=0`、89–91% 忙，`pagefaults` 不变。

| 组 | 轮数 | 故障 |
|---|---|---|
| C | 6 | 1（k2c，fault 50，DDE，读档后 64 s） |
| F（`cs_skip_head`） | 6 | 0 |
| G（`dde_touch`） | 6 | 2（k3g fault 51，55 s；k5g fault 52，50 s；均为 DDE） |

F 的 0/6 与 C 的 1/6 在随机波动内，不能说明 `cs_skip_head` 有效；F 组没有故障，也就没有回答零数据是否会移到第 3 页。G 组照样出故障，`dde_touch` 不是规避方案。三次故障的零数据位置都已定位，仍然是 **DDE 在本次提交中第一次读 BO 第 1 页**：

| 故障 | IB2 / 命令流 BO | 出错的 draw state | 前一组（已读完） | 读到零 |
|---|---|---|---|---|
| 50（C） | `0x40e44a7000` / `0x415fbe6000` | SDS 443/447，第 6 个 draw | 组 9（VB，第 0 页 `0xe1c`，12 dword） | 组 11 包头，第 1 页 `0x044` |
| 51（G） | `0x41170da000` / `0x41611a6000` | SDS 475/479，第 5 个 draw | 组 9（第 0 页 `0xfac`，20 dword，到 `0xffc`） | 组 11 包头，第 1 页 `0x258` |
| 52（G） | `0x40e4a36000` / `0x4160846000` | SDS 512/516，第 5 个 draw | 组 9（第 0 页 `0xfb8`，最后一个 pkt4 包头在 `0xff8`，其 payload 的后两个 dword 在第 1 页 `0x000`/`0x004`） | 组 11 包头，第 1 页 `0x2bc` |

DDE 的最近三个包头都是 `00000000`、`708a8000`（IN_IB_END）、`40a02083`（组 9 最后一个 pkt4），即读完组 9 后在下一组的包头读到零。

**`dde_touch` 的读确实发生在 DDE 读之前。** fault 51 的 IB2 在 dword 495 有 `CP_WAIT_REG_MEM`，地址 `0x41611a7000`，紧接着就是 draw #5（dword 502）；fault 52 在 dword 565 读 `0x4160847000`，紧接着就是 draw #5（572）。DDE 正在处理的是这个 draw 的 state：BR 的 `$13` 比 DDE 大 1（fault 50/51/52 都是如此），硬件组表里已同时装着这个 draw 和下一个 draw 的两套地址（组 9/11 的 A、B 槽）。也就是说，BR 已经执行完这条 WAIT 和该 draw，又处理了下一组 SDS，DDE 才在第 1 页读到零。BR 的 `CP_SQE_STAT` 包头历史里不出现 `CP_WAIT_REG_MEM`，因此不能直接从历史判断 BR 的位置。fault 51 的提交（ts 29189）从提交到故障约 0.41 ms，其间 trace 中没有抢占事件。

结论：同一页刚被 BR 通过 `CP_WAIT_REG_MEM` 读过，DDE 读这一页仍然得到零。"第一次访问页面时 TLB 未就绪"如果成立，也只能是 DDE 自己的读取路径（例如独立的 TBU/预取），不能用 BR 读来预热。由于这条 WAIT 用的是 mask 0，它不检查读到的值，所以还不知道 BR 在那一刻读到的是否也是零。

另一个观察：fault 52 中组 9 的最后两个 payload dword 在第 1 页。如果 DDE 读第 1 页时这两个 dword 也是零，它们会被当作寄存器值（顶点缓冲地址高位和大小）写入，不会报错。也就是说，同一问题在零落在 payload 而不是包头时，可能表现为无报错的渲染错误，而不是 GPU 故障。

### `dde_check`（Turnip `f610504c89c`，本地）

为区分"内存在那一刻就是零"和"只有 DDE 读到零"，新增 `TU_DEBUG=dde_check`。对第一次被引用的页，`CP_WAIT_REG_MEM` 不再读页起始地址，而是用 `WRITE_NE`、mask `0xffffffff` 轮询首次引用该页的那个 draw state 的第一个包头（包头不可能是零），直到读到非零。只被上一页延续过来的 draw state 覆盖的页，仍用 mask 0 的读。可能的结果：

- GPU 挂在这条 WAIT 上：BR 在该地址也读到零，即那一刻内存（或两条路径共用的某层）确实是零。
- 通过这条 WAIT 后仍出 DDE opcode=0：只有 DDE 的读取路径看到零。
- 故障消失：零数据是暂时的，WAIT 等到了有效数据，可以作为规避方案继续评估。

驱动 `44f215fc…`，APK `94003880`，host `d8b708e2`，21:06 装机（设备上 SHA 已核对）。C/H（H = `cs_recycle,dde_check`）按 C、H、H 轮换 6 轮（`trials/m1c`–`m6j`）进行中。

### `dde_check` 的结果：BR 读同一包头也持续得到零（fault 54）

**fault 54（m2j，H 组，读档后 113 s）** 正是 `dde_check` 要区分的情形。dmesg 只有 `Preemption Fault: cur=2 R/W=0x1950/0x19fb`（rb2 无法被抢占到 rb0），没有 opcode 错误。快照中：

- BR：`pc=0xc79`，`$19=0`，最近的包头是 `70bc8006`（`CP_WAIT_REG_MEM`）、`7043800f`、`70438003`。IB2 `0x4117e06000` 取到 dword 992，ROQ 里还有 493 个未消费，所以 BR 位于 dword 499 之前，正停在 dword 492 的 `dde_check` WAIT 上。
- 这条 WAIT 是 `WRITE_NE`、mask `0xffffffff`，轮询 `0x4160167218`：命令流 BO `0x4160166000`（4 MiB）第 1 页 `+0x218`，即 draw #5 的组 11 包头（SDS 472 设组 11 到第 1 页，SDS 476 设组 9 到第 0 页 `0xf6c`）。这和 fault 43–52 的位置模式相同。
- 快照中这个地址的内容是 `40ab0a02 60166fc3 00000041 48ab1f01`，包头非零。
- DDE 空闲（`pc=0x1a7`，`$19=0`，`$13` 与 BR 相同），因为 BR 没有走到 draw #5。

时间线：这次提交（ts 44971）在 341777.1839 提交，341777.1960 发出的抢占请求一直没有完成，10 秒后（341787.196）报故障。也就是说，BR 从提交后约 12 ms 起轮询这个地址约 10 秒，每次读到的都是零。

**结论：** CP 读这一页时持续看到零，DDE 和 BR 都是如此，而 CPU 侧快照里的内容是正确的。这不是 DDE 独有的读取路径问题，也不是短暂的写入未完成或读取竞争。剩下的可能是 GPU 侧对这一页的视图错了：SMMU/TLB 的翻译指向了别的物理页，或者 GPU 侧缓存中有过期数据。

这个 BO 的其他事实：

- 它在故障前 83.4 s（341703.786）由 VkRecord 分配，flags `0x10c0000`（GPU 只读，write-combine，4 KiB 对齐）。此后它所在 2 MiB 区域（`0x4160000000`）的 L3 页表没有回收或重建。
- 这张 L3 页表在 341703.489 建立，物理页 `97996e000`。这个物理页在此前 73 秒里被 USB 控制器的 IOMMU 域（`a600000.dwc3`，即 ADB 的 USB）反复用作页表，最后一次释放在 341703.406，83 ms 后成为 GPU 的页表。这与故障是否有关未知：GPU 之后正确读了这个 BO 约 83 秒。
- 故障前 2 秒内 trace 中没有 GPU 上下文（`gfx3d_user`）的 `tlbi`；这次提交与上一次之间做过一次 rb0↔rb2 抢占切换（level 0）。
- `cs_recycle` 复用 BO 时只接管句柄与地址，不清零内容（`ca038cc10f1`），所以零不来自回收。

**fault 53（m1h，H 组，读档后 21 s）** 是另一种表现，与 fault 42 同类：

- dmesg 同样只有 Preemption Fault。BR `pc=0x362`、`$19=0xbad4`，最近三个包头全是零；fault 42 的 BR 也是 `pc=0x362`、`$19=0xbad4`，fault 46 的 BR 是 `pc=0x35c`。BV 与 DDE 正常。
- BR 的 IB1 位置在 dword 6117：刚执行完 6113 处调用 IB2 `0x40e4186000`（2663 dword）的 `CP_INDIRECT_BUFFER`。IB2 的 512 个预取 dword 都还没消费，所以 BR 是在这个 IB2 的第一个包上读到零。这是本次提交中第一次读这个 256 KiB BO 的第 0 页：IB1 里其后的 40 多次 IB2 调用都在同一 BO 的更高偏移。
- 快照中 BR 的 ROQ 里，这段 IB2 数据（ROQ[512..1023] 的环形区）与内存逐字一致；IB1 窗口（6112–6367）也一致。也就是说 ROQ 最终装进了正确的数据，但 BR 执行时读到的是零。故障前经过的所有 `dde_check` WAIT 都已通过，它们的地址在快照中都非零。

两次加起来：零数据不只出现在 DDE 取 draw state 时，BR 取 IB 和 BR 用 `CP_WAIT_REG_MEM` 读内存时都会出现；共同点仍是"本次提交中第一次读某个命令流 BO 的某一页"。

**H 组出故障后表现为抢占超时：** `dde_check` 打开时，DDE 读到零的情形会变成 BR 卡在 WAIT 上，最后表现为抢占超时（Preemption Fault），而不是 DDE opcode=0。

**本批统计（21:06–21:37，用户要求转去其它机器后中止）：**

| 组 | 轮次 | 故障 |
|---|---|---|
| C | m1c、m2c、m3c | 0 |
| H（`dde_check`） | m1h、m1j、m2h、m2j、m3h | 2（m1h fault 53：BR 在新 IB2 首包读到零；m2j fault 54：BR 卡在 `dde_check` WAIT） |

m3j 在读档前中止，不计。中止后设备上的 tracefs 实例和临时文件都已清理，故障计数仍为 54。

### 下一步建议（区分翻译与缓存）

1. **CP 读值转存**：在 `dde_check` 的 WAIT 之前，用 `CP_MEM_TO_MEM` 把 CP 读到的几处数值写进同一命令流 BO 中已能正确读取的页（快照会抓到），包括该页起始、目标包头、页内其它偏移和相邻页。整页都为零说明是翻译问题（TLB 或页表），只有目标行为零说明是缓存行问题。
2. **别名映射**：给同一物理页再建一个 GPU 虚拟地址（导入同一 dma-buf 或 user-mem），出问题时两处都读。只有原地址读到零说明是该虚拟地址的翻译问题；两处都为零说明 GPU 看到的物理内存本身过期。
3. **USB 页表页复用**：留意 fault 54 中 GPU L3 页表物理页刚从 `a600000.dwc3`（ADB 的 USB）域释放后被复用这一点。可以在不接 USB 调试（无线 adb 或断开）时统计复现率，或在新故障的 trace 中检查其它故障区域的页表页是否也来自 dwc3 域。

## 8. 当前状态

- 已完成（10-18 下午）：
  - 读档进关的复现流程已脚本化；`cs_recycle` 加 stream submit 合计 15 轮中 5 轮在 180 秒内出故障，前期高、后期低（见 7.4）；
  - 九次 DDE opcode=0 读到零的位置：七次在 2/4 MiB 命令流 BO 的第 1 页，两次在 128 KiB BO 起点（见 7.3、7.4）；10-18 晚 fault 50–52 三次也都在第 1 页（7.5）；
  - 排除：CPU 写缓冲未排空（`cs_dsb` 仍出故障）、SMMU 缺页（tracepoint 与 dmesg 均无）。
- 10-18 傍晚已完成的 A/B（APK `d2423a4d`，驱动 `3e3e29c0…`，已装机）：`cs_dsb` 4 轮 1 次 DDE；`syncdraw` 4 轮 1 次 BV；同批对照 C 组共 8 轮 2 次 DDE。差别在随机波动内，见 7.4。
- C 组复查 r1–r4：1 次 DDE（fault 49），C 组合计 15 轮 5 次。fault 43/44/45/49 的零数据都出现在 DDE **本次提交第一次读该 BO 第 1 页**时（7.4）。
- 10-18 晚 C/F/G 各 6 轮（APK `3a4e0cf4`，驱动 `34f92ded…`）：C 1 次、`cs_skip_head` 0 次、`dde_touch` 2 次，三次都是 DDE，零数据仍在本次提交第一次读第 1 页时出现；BR 刚读过该页也没用（7.5）。C 组合计 21 轮 6 次。
- `dde_check`（APK `94003880`，驱动 `44f215fc…`）与 C 组 C/H/H 轮换跑到第 3 轮中止：C 3 轮 0 次，H 5 轮 2 次。fault 54 证实 BR 读同一包头也持续得到零，问题不限于 DDE（7.5）。
- 设备终态（10-18 21:40）：APK `94003880` 不变，`debug.mesa.tu.debug=cs_recycle,cs_skip_head`（本轮各组中故障最少的组合；`debug.*` 属性重启后失效），app 停在桌面，tracefs 实例与临时文件已清理。
- Turnip 已推送到 fork 分支 `codex/a8xx-cs-recycle`（`ff5d4093fa5`），父仓引用推进到它。推送前为补 fork 要求的 `Generated-by: LLM` 尾注改写了本地提交，哈希对照（各对的树相同）：`ca038cc10f1`→`ce8f89d4e43`（cs_recycle）、`10f23b2bdc0`→`84a5c7ac917`（cs_dsb）、`84db376cfde`→`69bf236e9de`（cs_skip_head/dde_touch）、`f610504c89c`→`ff5d4093fa5`（dde_check）；`bbb3e82bc8d` 不变。文中与日志里的旧哈希指本地构建时的提交。
- 待做：
  - 区分翻译与缓存：CP 读值转存、别名映射、不接 USB 调试的对照（7.5 末尾三项）；
  - 先找到稳定复现的条件（记录 GPU 频率/温度），再重做 syncdraw 与调研建议的 B1（pipeline SDS 前插 WFI/WFM）对照；
  - `cs_skip_head` 需要更多轮次才能判断；
  - 试验脚本与快照分析脚本已整理进 [`tools/a8xx-dde`](../../../tools/a8xx-dde/README.md)。快照、trace 与各轮截图仍只在原机器的 `build/validation/preempt-ab-20261017/`，没有入库；
  - BV 签名（faults 42/46/48，`0x70970001`）：其中 42、46 的 BR 同样读到零包头，零的地址未定位；
  - A 组（不开 `cs_recycle`）和关闭 stream submit 的对照仍各只有 1 轮。
- 历史记录：用脚本做的 A 组（新、旧驱动）在存档点附近走动均未复现；旧驱动在用户实际游玩时复现了 fault 37（见 7.1）。B、C 两组的单次结果见 7.2。
- Odin3 的 GoW3 配置已由用户改为倍率 1.0、帧率上限关闭（10:34），保持不动。
- `upload_diag stream_submit` 的默认值保持 0。
- 待用户确认：Swan 上实际修复 DDE 故障的固件版本号与 SHA。内部资料中的 `0x01510095` → `0x01510103`，与仓库里 09-28～29 的 DDE 故障快照已在 `0x01510103` 上相矛盾（见 6.1）。

## 参考

- mesa#16460：<https://gitlab.freedesktop.org/mesa/mesa/-/work_items/16460>
- mesa!44888：<https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44888>
- mesa!43970 / `118ec7f45b1`：<https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43970>
- mesa!45129 / `c88b6ebd383`：<https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/45129>
- WinNative-Emu/Drivers `c71f600`：<https://github.com/WinNative-Emu/Drivers/commit/c71f600a2977a141f21a922c9ed2e14fb09b13d3>
- CodeLinaro graphics-kernel：<https://git.codelinaro.org/clo/la/platform/vendor/qcom/opensource/graphics-kernel>（`44b9ce7d`、`d5188a1e`、`f48b371a`、`9193f728`、`93ddb84a`、`0a40ed44`）
