# Odin3 KH3：GPU hang 与 Reshape 接入后版本缺字排查（2026-10-11）

延续 [固件交叉分析](odin3-kh3-firmware-analysis-20261011.md)。本轮用户授权独占 Odin3 复测，并补充另一台 8 Gen 3 可以进入游戏。后者是用户观察，未取得该设备的驱动、ROM 与配置，暂不能单独证明硬件代际是唯一变量。

## 概要

| 项 | 案发值与来源 |
|---|---|
| Spider | not present |
| Meego | not present |
| Slardar OS | not present |
| 设备 | Odin3 `6edb0593`，Adreno 830，chip `0x44050000` |
| ROM | `qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys` |
| SPR | unknown (source missing)，本轮 2D 会话未用于归因 |
| XRRuntime | unknown (source missing)，本轮 2D 会话未用于归因 |
| 出错进程 | `com.shadps4.android`，APK 0.1.8 / 26081400；三轮 PID 为 10206、10707、11659 |
| KMD | Build-ID `8ad43a33afb6affe54edeb6804aa14089dcfe7ce`，精确源码映射 unknown (source missing) |
| UMD | Turnip `2b8ed3fc20`，原库与诊断库完整 SHA 见下文 |

**尚未验证 KH3 loading hang 修复。** 后续独占复测已否定 MRT PKT4、关闭绑定跳过、`cmd_no_preempt` 和 `kgsl_preempt_rb` 作为修复。root 全局关闭抢占能延后错误，但仍发生 GPU timeout。三份 PKT4 preemption snapshot 的 SQE 错误恢复位置恰好落在 IB2 的 4 KiB 边界，提供了命令读取路径的排查线索。

**缺字回归已定位到本轮 IB32 分配改动。** 原接口的 32 位 flags 会截断 `FORCE_32BIT`；改用 64 位 flags 后低地址分配真正生效，却使独立字体复制测试持续读回错误。保持相同捕获 SPIR-V、相同输入与提交方式，原同源驱动两种描述符模式各 32/32 正确，IB32 驱动各 32/32 失败；保留 GPUOBJ_ALLOC、仅去掉 FORCE_32BIT 又各 32/32 正确。因此不是先前怀疑的 Reshape SDK、async skip、stream buffer 或绑定跳过所能解释。低地址为何影响 GPU 执行的更底层机制尚未证实。

当前修正为检查实际分配范围不跨 4 GiB 边界；遇到跨界分配，保留其占位再有限重试，最后释放拒绝项，不再强制所有命令存储位于 4 GiB 以下。驱动 `c22932ba…` 的两种字体复制测试各 32/32 正确。APK `d70cbac9…` 已安装，游戏内标题、难度、确认、亮度和 INFORMATION 全部完整显示；同轮 loading 仍触发 fault46，不能把缺字修复等同于 GPU hang 修复。下文按发生顺序保留已被后续对照否定的假设。

## 身份与证据

- 设备：Odin3 `6edb0593`，Adreno 830，chip `0x44050000`，boot ID `c322cd23-e6c8-46f8-8eb7-ff032d7162db`。
- ROM：`qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys`。
- KMD Build-ID：`8ad43a33afb6affe54edeb6804aa14089dcfe7ce`；精确源码映射 unknown (source missing)。SQE/AQE/GMU 身份沿用前一份记录，未更换固件。
- 游戏：`CUSA15072`，标题画面显示 1.10。APK versionName 0.1.8 / versionCode 26081400。
- Spider / Meego / Slardar：not present；SPR / XRRuntime：unknown (source missing)，本轮 2D 会话未用于归因。
- 原 APK SHA：`62618071029bc5b7d2e5fb0b152133a3bf5d1ed720d2502bfb5603b83ea64597`。
- 原 Turnip：Mesa commit `2b8ed3fc20552e5fe0ed3eca8fe9e2772e494a42`，库 SHA `ba53ae858c03dc7a0760d4cf6a81d6f32cca519514b8fb0900a5151d734c19b2`。
- 证据根目录：`build/validation/odin3-kh3-fix-20261011/`。原 APK、配置、存档分别备份为 `baseline/original.apk`、`baseline/config-save-before.tar`。

## KGSL 错误证据

检索覆盖全部四份 dmesg，pattern 包括 GPU hang、Fault id、Protected mode、SMMU/IOMMU、GMU/HFI timeout/reset、waittimestamp、context 与 BR/BV ring；逐条命中、源文件行号和时间范围保存在 `kgsl-coverage.json`。原驱动三份日志的 uptime 范围分别为 `129677.268251–132497.694718`、`129768.225595–132709.853584`、`129824.132822–132906.796878`；各自覆盖故障前超过 60 s，故障后只覆盖约 21 s、21 s、69 s，不能声称完整观察了后续恢复。诊断版为 `129954.713428–133365.869484`。本轮未取得 pstore/last_kmsg。

### 三个原驱动对照

| 条件 | PID / run UUID | KMD uptime / context / timestamp | 结果 |
|---|---|---|---|
| 默认 | 10206 / `5d8fd98f1a164bbdf341c1e4259c7740` | 132476.507790 / 14 / 5518 | 启动片段中保护写错误 |
| `vk_recorder off` | 10707 / `41dd3e0d68ae35e62e557c9d610ff77d` | 132689.159856 / 6 / 36623 | 延后至片段转 loading 时，仍保护写错误 |
| `pipeline_cache bind_skip off` | 11659 / `804b334756584f6dd27d66049374b78c` | 132837.672279 / 6 / 17637 | 仍保护写错误 |

三轮最早相关 KMD detector 均为保护写错误。默认轮原始来源 `baseline/dmesg.txt:27825–27828`：

```text
[132476.507790] [ T1988] adreno-gen8-gmu 3d37000.qcom,gmu: CP | Protected mode error | WRITE | addr=0x00000 | status=0x00600000
[132476.507814] [ T1116] kgsl kgsl-3d0: Fault id:2 and GX is ON
[132476.507818] [ T1116] kgsl kgsl-3d0: shadps4.android[10206]: ctx 14 ctx_type ANY ts 5518 policy 2
[132476.507821] [ T1116] kgsl kgsl-3d0: shadps4.android[10206]: cmdline: com.shadps4.android
```

其后 `Fault id:2`、进程/context 归属、应用 `ErrorDeviceLost`／断言是下游传播，不能用应用退出点替代 GPU 首次错误位置。地址 0 是保护寄存器访问报告，不是已经证明的 GPU 内存 VA。

`baseline/`、`recorder-off/`、`bind-skip-off/` 分别保存完整 dmesg、logcat、host 日志和原始 snapshot。三个 snapshot 的 OS 时间与各自新 occurrence 相符，读取后释放冻结 snapshot，未复用旧 snapshot。数据库仍使用前一轮隔离的重复关系去重 harness；不修改 raw 文件，也不把 Gen8.2 的 selector 解释状态升级为 exact。

`snapshot-comparison.json` 保存原始身份、BR SQE 相关值、IB2 内容与寄存器对：

| 条件 | IB2 VA | dwords | SQE PC / rem / BAD4 |
|---|---|---:|---|
| 默认 | `0x418f496e88` | 304 | `0x362 / 0xa9 / 0xbad4` |
| recorder off | `0x418fb06e88` | 304 | `0x362 / 0xa9 / 0xbad4` |
| bind skip off | `0x418ee16e88` | 304 | `0x362 / 0xa9 / 0xbad4` |

均对应 IB2 dword 38 的 `0x70dc8082`：130 个 payload dword，65 对 register/value；恢复时重读并跳过 169 dword，下一包头 `0x40808e02`。这重复了前日 23:29:24 的恢复边界，已不需要假设 mesh CTS 并行才能复现。但 SQE PC 仍是恢复代码，不是首次错误的执行 PC；capture 中合法的寄存器对不能证明首次 GPU 获取的数据必然相同。

录制线程关闭后计数为 chunks=0、commands=0、worker_submits=0；绑定跳过关闭后统计显示 `repeated pipeline binds left out: 0 (bind_skip off)`。两种单变量实验都没有解决，不能保留为修复。

## MRT PKT4 诊断版本

临时改动位于 Mesa 子仓 `src/freedreno/vulkan/tu_cmd_buffer.cc` 的 `tu6_emit_mrt`：相同寄存器和值逐项使用 `tu_cs_emit_regs`，绕开这一处 CRB 的 SQE handler。其他 CRB、cache flush、WAIT_FOR_ME、shader 和队列提交语义未改。

- 本地分支：`codex/kh3-mrt-register-writes`，未提交。
- 当前诊断改动暂适用于所有芯片；若验证有效，正式实现必须限定受影响的平台，保留 8 Gen 3 路径。
- 驱动 SHA：`8a0ea357a26da608ef84cfbca08295338c20d2fb311fa6276127da06f06b64ae`。
- APK SHA：`b5bf61c85449b133d996293438f624044003f3875dcfcc2416b5c27f68427994`。
- `pkt4-mrt/driver-identity.json`、`source.patch`、`diagnostic.apk` 保存完整身份和二进制。
- Android host 与 APK 编译成功。Gradle 复用已有 cinema 纹理资产，跳过 `compressXrCinemaTextures`；没有改动它们。
- 安装成功，启动状态确认实际加载上述 SHA；PID 13690，run UUID `52cb2dc06f57ca97b129d67b41fb806d`。新进程恢复默认 recorder on / bind skip on。
- `scene-4.png` 已进入标题菜单；该时刻 faultcount=13，未出现新 GPU fault。

### 外部强停，测试中断

`pkt4-mrt/logcat.txt:21401`：

```text
10-11 10:26:52.221 ... Force stopping com.shadps4.android appid=10113 user=0: from pid 14307
```

`exit-info.txt` 确认为 `reason=10 (USER REQUESTED) subreason=21 (FORCE STOP)`。故障计数仍为 13，snapshot/dump 返回 0 字节；`kgsl-snapshot.bin` 因此是空文件，不作为 snapshot 证据。

此时本机进程列表发现 `build/turnip-mesh/ws8-odin3/n53-smoke-20261011/run-probes.py prim256` 和其 `run_matrix.py --serial 6edb0593` 子进程；进程行保存在 `external-test-processes.txt`。该事实证明设备再次被同时使用；没有把外部脚本名称当成 PID 14307 的完整调用链证明。未结束或改写另一个任务，已请求恢复独占窗口。

## 后续独占复测（13:05 起）

用户再次确认其他设备测试暂停。本节各次 GPU 错误均有相应进程/context 的 KMD 记录；未沿用 10:26 外部强停作为本节 GPU 证据。所有材料位于同一证据根目录，各子目录保存 raw snapshot、SQLite、日志与截图。`later-snapshot-comparison.json` 摘录 BR SQE 与 IB，`kgsl-coverage.json` 已扩展到嵌套目录及重试日志，保留文件 SHA、行号、实际 uptime 起止和完整匹配。日志重复包含同 boot 旧错误，不能累加为新 occurrence。部分日志没有覆盖故障后 120 s；pstore/last_kmsg、连续 GTop/忙碌时间线仍缺失。

| 条件 / 子目录 | PID / KMD uptime / ctx / ts | 首个 KMD detector | 结果 |
|---|---|---|---|
| MRT PKT4 默认 `pkt4-mrt-r2` | 18055 / 142975.104680 / 6 / 84508 | GPU hang detected | 完成新游戏确认后 loading，SQE 停在 WAIT_FOR_IDLE 路径 |
| PKT4 + bind skip off `pkt4-bind-off` | 30560 / 143246.076148 / 15 / 5584 | Preemption Fault | 仍失败 |
| PKT4 + cmd_no_preempt `pkt4-no-preempt` | 32761 / 143738.753967 / 15 / 8251 | Preemption Fault | 应用命令级关闭没有解决 |
| PKT4 + kgsl_preempt_rb `pkt4-preempt-rb` | 971 / 143830.263358 / 21 / 6637 | Preemption Fault | 申请 style=1 不等于已证明实际 L1/L2 行为改变 |
| PKT4 + root 全局抢占关 `pkt4-global-no-preempt` | 4981 / 144342.879581 / 15 / 37265 | gpu timeout | 仍失败；不是正式修复 |
| 原驱动 + 全量 Reshape `reshape` | 7027 / 144659.158085 / 6 / 5375 | Protected mode WRITE addr=0 | 重现原 CRB 恢复边界 |
| 原驱动 + 全量 Reshape `reshape/repro2` | 7637 / 144935.798094 / 6 / 29059 | GPU hang detected | 采集结束/Stop 邻近窗口，不能算成功退出 |
| 32 位 IB 修正、Reshape 关闭 `ib32` | 8666 / 145155.951967 / 14 / 30789 | GPU hang detected | IB 已在低地址，仍 WAIT_FOR_IDLE |

以上 detector 是已捕获的首次相关硬错误，不等于精确的 GPU 停止进展时刻；其后的 `Fault id:2`、错误 context 和应用 device lost 是检测/传播链。

### 原始错误与命令边界

`ib32/dmesg.txt:15026` 起（kernel uptime，墙钟精确映射未另行校准）：

```text
[145155.951967] [ T1988] adreno-gen8-gmu 3d37000.qcom,gmu: MISC: GPU hang detected
[145155.951994] [ T1116] kgsl kgsl-3d0: Fault id:2 and GX is ON
[145155.952003] [ T1116] kgsl kgsl-3d0: shadps4.android[8666]: ctx 14 ctx_type ANY ts 30789 policy 2
[145155.952012] [ T1116] kgsl kgsl-3d0: shadps4.android[8666]: cmdline: com.shadps4.android
[145155.952055] [ T1116] kgsl kgsl-3d0: status 00EC2405 gfx_status 00E80004 gfx_br_status 00E80004 gfx_bv_status 00880004
[145155.952062] [ T1116] kgsl kgsl-3d0: BR: rb 1722/1759 ib1 00000000050FB000/5707 ib2 0000000004CB2D80/0000 ib3 0000000000000000/0000
[145155.952074] [ T1116] kgsl kgsl-3d0: BV: rb 1722/1759 ib1 00000000050FB000/0207 ib2 0000000000000000/0000 ib3 0000000000000000/0000
```

原始完整块保留在该 dmesg，后续行含 BR/BV 与 IB。不能把保护寄存器 addr=0 报告成内存 page fault；本轮没有由这些行证明 shader 越界。

三份 PKT4 preemption snapshot 的 IB2 分别是 `0x418e646e1c`、`0x418f596e1c`、`0x418ea96e1c`，长度均 294 dword；BR SQE 当前包头为 0，错误标记为 BAD4，恢复跳过 121 dword。`0x6e1c + 121 * 4 = 0x7000`，正好跨到下一 4 KiB 页。捕获内存中的 dword 121 则为合法的 PKT4 MRT6 寄存器 `0x8852 = 0`，与 SQE 留下的零包头不一致。

原 CRB 版本 IB2 尾地址 `0x6e88`，dword 38 开始的 CRB（130 payload dword）横跨页界，dword 94 正好位于下一页；SQE 恢复跳到整个 CRB 结束的 dword 169。**这是命令读取/可见性路径的线索，不是已证明的页表或硬件根因。** snapshot 是后续 CPU 可见内容，SQE 是错误恢复状态，仍需解释两者之间的差异。MRT PKT4 试验已撤回，未保留为修复。

root 全局关闭抢占的有界实验为 13:24:30–13:30:09；记录 `preempt_count=0x25BFFF` 前后相同，faultcount 35→36。看门狗在新 fault 后恢复原值 1（400 s 上限未到）。这一事实比单纯设置 `cmd_no_preempt` 更强，但只说明禁用全局抢占仍可触发另一种 timeout，不能据此合并所有错误机制。前面 `original-global-no-preempt` 使用不同旧宿主且没有 host present，已中止，不作为有效画面对照。

### 32 位 flags 截断与 IB 地址对照

`tu_bo_ib_flags` 已有避免 4 GiB rollover 的策略；但 `kgsl_gpumem_alloc_id.flags` 是 32 位字段，`KGSL_MEMFLAGS_FORCE_32BIT = 1ULL << 32` 写入后被截掉。试验改为 `GPUOBJ_ALLOC` 的 64 位 flags，并通过 `GPUOBJ_INFO` 检查返回标记/范围；其余分配保持原路径。补丁保存在 `ib32/source.patch`。

试验驱动 SHA `e9b29b05d4ad42b02eb252401873f3235841e072e792e3cd31bbf75a723e6c00`。`ib32/snapshot.sqlite` 的 IB1 为 `0x050fb000`、IB2 为 `0x04cb2d80`，证明低地址要求实际生效。此时 BR SQE PC=`0x74a`、包头=`0x70138000`、BAD4=0，仍与原始 WAIT_FOR_IDLE hang 同类；仅降低地址不能解决 loading。`ib32/no-retire-gates.json` 保留 SQL 门槛结果，部分 OS context ID 仍不可用，不能从表下标捏造 context 归属。

### Reshape 的有效范围

本轮复用已接入的 GPU Reshape SDK ABI 7，实际 DSO SHA `1242e1b8ea282f072aa983cc7b56ce5bff40a1b7665620d6edc85cec1195b24f`。本机 SDK checkout 包含既有未提交改动，不把 HEAD 当成完整构建身份。独立 host 使用 .NET 8 与 SQL schema 13，workspace 为 `reshape/workspace/workspace.sqlite3`；capabilities、schema、原始请求/查询结果均保留。查询结果携带 coverage、provenance、watermark 与 truncated。

第二轮 session `session:51649178aeb44f649acc5bd58105744e`：最终查询记录 336256 个 resource-bounds message，全部资源 token=`0x3ffffd`，SDK 定义为 `InvalidTableNotBound`，不是有效纹理/缓冲区的 PUID。另有 338368 个 table_not_bound、64 个 type mismatch；collector 完成 18817 次、payload limit 失败 717 次，transport dropped=3712，PRMT misses 最大 22923。大量溢出与 SGUID 归属缺口意味着不完整覆盖。

因此当前全量插桩既不能证明“游戏无越界”，也不能把这些无效 token 告警当作游戏越界。SDK 的隐式 descriptor safeguard 会改变画面（旧记录已说明），下一轮应先缩到一个有明确资源映射的 shader。host 已正常退出并释放自己建立的 adb forward；属性已恢复采集前空值。

## 文字回归对照（用户指出 Reshape 后版本）

`pkt4-global-no-preempt/scene-3.png` 中 loading 中文、标题与 Now Loading 完整；`ib32/scene-2.png` 中相同背景还在，文字大量缺失。`ib32/android-host.log` 确认 requested=false、resource_bounds_active=false、shader_replacement_active=false，不能把这一轮直接解释成仍在运行插桩。

原 KH3 recipe 与 driver cache 已备份到 `text-cold/cache-before.tar`，设备上只做重命名保留。使用相同 IB32 APK、新缓存复测（PID 11597）标题菜单仍缺字，主动 Stop 成功，faultcount 保持 39。说明不是这两份旧缓存残留造成。保持 SDK、撤回 IB32 的原驱动对照连续两轮在片头保护写错误，未走到可对比标题，因此不据此断言 IB32 是否导致文字丢失。采用 IB32 与同样缓存，只去掉 SDK 编译接入后（`text-no-sdk`，PID 14321 / run UUID `e40ca6aa485b32dc3802cc85bb456ef2`），NEW GAME / LOAD GAME / CONFIG / SPECIAL / PlayStation Store 文字全部恢复，中文难度设置及确认框也完整。状态确认 `adapter_compiled=false`，驱动仍是同一个 `e9b29b05…`。随后新出现的 INFORMATION 弹窗仍缺少大量字形（`information-stable.png`），因此这个结果只能证明主菜单改善，不能把 SDK 构建开关当成已闭合根因；正常/缺字画面都必须保留。该去 SDK 对照 APK、诊断版及旧缓存均保留；随后重新安装同一份 IB32 / SDK APK 做 accurate 对照。


进一步的实际资源证据保存在 `text-no-sdk/pipeline-skip-before.txt` 与 `gpu-images-status.txt`。该轮共跳过 27 draw / 14 pipeline，15 次被当成 history 清除缺失标记，仅一次触发 60 帧暂停。被标记的目标包括 `0x200b4c0000` / `0x200b6c0000` / `0x200b780000` / `0x200b840000` / `0x200b500000`（480×32 RGBA8），以及 `0x2009760000` / `0x2009f60000`（96×64 RGBA8）。读回相邻目标 image UID 31648 是清晰的“自豪模式”；被跳过的 `0x2009f60000` / UID 31673 读回为 RGB 全零、2816 个非零 alpha 像素（24576 字节），黑底预览看似空白，但不能称为全零或仅凭此证明字体丢失。精确地址/UID 仅适用于 PID 14321 这一会话，不跨进程复用。

这里的代码缺口是：`vk_missing_content.cpp` 把在 4 帧内再次被读取的缺失内容一律当成时域历史而取消标记，读取本身并不意味着目标已被重新绘制；晚于 4 帧才发现陈旧内容时，只能停止以后继续跳过，不能补回已经丢失的生成过程。这个模式本来就是有损模式，字体/缓存 UI 资源是不能假定下一帧自动重画的反例。但本轮 accurate 对照依然缺字，证明这个通用缺口不足以解释当前现象，未按这一猜测改全局默认或渲染代码。


### async_accurate 对照与字体图集

`text-accurate-sdk` 使用原 `ib32/diagnostic.apk`（SHA `558bf8158feb44e8fa9fba2faab4b8fc8488672908812f601e820884ab4ba6a4`），SDK 编译接入但运行时关闭。仅把 KH3 profile 的编译模式改为 async_accurate，并重命名 KH3 recipe/driver cache 保留备份。PID 17217 / run UUID `e81b4eb45499f2349925a86c80215ce6`：generation 1 为冷缓存；generation 2 继承其启动/标题管线缓存，新难度、亮度、INFORMATION 和 loading 均仍大量缺字。`retry-pipeline-final.txt` 记录 0 次 skipped draws。这轮正常点 Stop，未增加 GPU fault（41）；未验证 loading 后确认。

在 generation 2，GPU 图像 UID 42938 / Guest VA `0x2002d00000` 的 1024×1024 R8 图集读回只见启动/标题字形；它的 GpuModified 标志已置位，Guest CPU 内存全零不能当作应上传内容。图集截图见 `font-atlas.png`，完整原始读回与身份保存在同目录。`36b7e09850ea6a564a58876a8b9f3e56` 命令 trace 完整捕获 flip 39087..39103，共 16 个区间；每帧有 4 个 draw 用 FS `0x0c21998a758dcf4b` / VS `0xf81eeafc1b68590c` 读取该地址，没有图集写入。因此这是消费者证据，不能冒充缺失字形的生成过程。后续需捕获首次生成新字形的窗口。


### 字体复制生产者（generation 3）

`text-accurate-sdk/menu-traces/2068142cc7cf03d183991c7a04a0a53a.gcmdtrace.ps4` 捕获 action 2483：CS `0xb903b5f213ef7d4d`，128×128×1 个工作组，把 R8Unorm 1024×1024 的 `0x2010b00000` 复制到字体图集 `0x2002d00000`。没有 HLE 替换。其 GCN/旧 IR 表明，16 字节常量给出复制矩形，线程坐标在矩形内才执行 imageRead/imageWrite；旧 IR 的常量值不代表本次 dispatch。

随后 `font-copy/0a7b02198af7d0a54b5d41a526fe308d.gcmdtrace.ps4` 捕获 action 27184，同一 CS 将 `0x200ff00000` 复制到相同图集，常量地址 `0x2006ff2c10`。事后读回源图 UID 267569 的 GPU 与 CPU 字节完全相同（SHA256 `2a0c669d6a0bfaabfafeaec66c010e924cd37de384f558a6524e89880601b735`，177296 非零字节）；目标 UID 194025 只有 167151 非零字节（SHA256 `de1854992ee1f1204c949a8a1e3ef837653f775562841fb2eb2e693f1757ffac`），缺最新增加的一部分字形。目标 guest CPU 内存为零是 GPU 写入图像的正常情况，不能作为上传失败的证据。

这是事后读回，尚不能证明 dispatch 当时源图与常量是什么：源内存可能继续被 CPU 更新。下一步需要采集实际暂存上传字节与实际绑定的 16 字节矩形，避免时点混淆。

Generation 4 在标题前运行时执行 `vk_recorder off`，随后新打开的难度面板仍大量缺字；commands/chunks 固定在 128264068/10219819，确认该段录制线程已关闭。重启会重新读取设置，因此不能把 generation 3 退出前的 off 当作 generation 4 从启动起生效。该轮随后恢复 on 并主动 Stop。


### 实际上传输入与完整会话对照

为消除“事后源内存已改变”的歧义，诊断版在 `ObtainBufferForImage` 之后保存真正交给上传路径的暂存字节，并在绑定时读取真正分配到 stream buffer 的 16 字节常量。只针对 1024×1024 / R8 的字体路径，文件和日志有数量上限；同步写诊断文件会影响时序，不能把此版的出现概率与普通版直接比较。补丁及 APK 分别保存在 `font-diagnostic/`、`font-diagnostic/r3/`。所有对照仍使用 IB32 驱动 `e9b29b05…`、SDK 运行时关闭、async_accurate。

| 条件 | 精确会话 | 结果 |
|---|---|---|
| 原计算路径 + 实际输入留档 r1 | PID 25711，UUID `ae84bc9cefebc95a0d321796e4b8d4bf`，APK `c8ea253207f5ff603f5c2a1077a689890f22c6320f6e93a6ad421438d3f9101e` | 标题/难度图集保持旧内容；后来一次更新成功，亮度页又缺字 |
| 从 Preparing 起关闭 texture_bind_fast，r3 | PID 27789，UUID `e5795cb65292c4e43175694feb1dc345`，APK `3da72634383c285e83e0b426b072f8e10b0b03de09af9154e51fd9e736346bd1` | 标题/难度正常，但亮度和 INFORMATION 仍缺字；fast_hits 为 0 |
| r3 同 APK，恢复快速路径，新进程 `TU_DEBUG=flushall,gmem_warmup` | PID 29444，UUID `216e67c126b4cace317aad21c473382f` | 标题缺字，难度更新补回标题与难度文字，确认框和亮度页再次缺字；不是有效修复 |

r1 目标 UID 12 / guest `0x2002d00000` 的标题和难度读回完全相同，均只有 64107 个非零字节，且与较早的实际上传 `3800-10881-14.bin` 逐字节相同；更晚上传 `7010-23810-31.bin` 已有 159890 个非零字节。一次后续更新成功时，目标又与 `13023-47868-32.bin` 的 163631 个非零字节全部吻合。说明目标并非始终不可写。

r3 完整会话从第一次字体更新前关闭快速路径，难度目标与 `5295-16953-30.bin` 完全一致；INFORMATION 目标只有 239747 个非零字节，最新暂存上传 `13070-48107-46.bin` 有 260543 个，相差 20796 个字节。目标最接近旧上传 `13064-48075-43.bin`（12 个字节不同）。可复核比较结果为 `font-diagnostic/byte-comparison.json`。

r3 绑定日志显示各次新源图 UID、VkImage、view 均更新；目标始终 UID 12，VkImage `0xb400006dc9257200` / view `0xb400006e3925a6f0`，没有 backing 替换。该窗口实际暂存的常量均为 `(0, 0, 1024, 1024)`，adjust=0。这排除了复制矩形本来就只覆盖旧字形的解释，也否定了“关闭 texture_bind_fast 就解决”的早期猜测。它仍未直接证明 GPU 执行那一刻读取的源图、SSBO、flatbuf 与 CPU 暂存内容完全一致；计算执行、输入可见性及后续覆盖仍需区分。`flushall` 失败也不能排除所有同步问题。

上述两个正常退出的对照均在亮度/提示页主动 Stop，faultcount 保持 42；`flushall` 属性恢复为空，并结束原进程以清掉继承的 TU_DEBUG 环境。

### r1 loading 的新 fault 42

r1 在后续 loading 发生新 GPU hang，早于最终 Stop；不能归因为 Stop 触发。原始来源 `font-diagnostic/fault42/dmesg.txt:15576–15582`，kernel uptime：

```text
[149356.073712] [ T1988] adreno-gen8-gmu 3d37000.qcom,gmu: MISC: GPU hang detected
[149356.073725] [ T1116] kgsl kgsl-3d0: Fault id:2 and GX is ON
[149356.073729] [ T1116] kgsl kgsl-3d0: shadps4.android[25711]: ctx 21 ctx_type ANY ts 93086 policy 2
[149356.073731] [ T1116] kgsl kgsl-3d0: shadps4.android[25711]: cmdline: com.shadps4.android
[149356.073769] [ T1116] kgsl kgsl-3d0: status 00EC2405 gfx_status 00E80004 gfx_br_status 00E80004 gfx_bv_status 00880004
[149356.073771] [ T1116] kgsl kgsl-3d0: BR: rb 1d42/1dea ib1 0000000006B59000/57a3 ib2 0000000004CB5D80/0000 ib3 0000000000000000/0000
[149356.073773] [ T1116] kgsl kgsl-3d0: BV: rb 1d42/1dea ib1 0000000006B59000/02a3 ib2 0000000000000000/0000 ib3 0000000000000000/0000
```

后续 149357.681785 记录冻结 snapshot，149357.684858 记录 GMU suspended，属于检测后的采集/恢复链。完整连续日志已保留。`snapshot.bin` 为 16611940 字节，magic `02004d50`；本轮 snapshot 工具不可用，未解码嵌入时间/OS context，不能把这份 raw 的精确故障时点或当前执行 shader 当作已证实。缺字在此前菜单持续出现而无新 hard fault，缺字与 loading hang 是否同根因尚未确定。


### 局部计算路径替换：首次得到完整字体图集

`font-transfer/diagnostic.apk`（SHA `1543f3f42c61a04a224e5b8ecb82eedc3427a95717722a80ae058a912f3c6b77`，host `ab7bf93be1ec4c85e9cfeecd71455f1189328480e9b67c875da2d21e1c287dc4`）在 r3 的输入留档之上，仅对 CS `0xb903b5f213ef7d4d`、128×128×1、1024² 单层/单 mip/单采样 R8、未缩放、矩形 `(0,0,1024,1024)` 的复制，完成原资源绑定后使用 Vulkan 图像复制代替 dispatch。普通 draw、其余 compute、上传和后续字体采样不改；这是一项隔离试验，不能冒充根因修复。

PID 31636 / UUID `3052923e70dd18ce3eb7b217ed8d8c8e`，日志确认 `TU_DEBUG=gmem_warmup`（已无 flushall）、SDK requested=false。标题、难度、确认框、亮度、INFORMATION 和 loading 页均完整显示文字。INFORMATION 图集 UID 12 读回有 260543 个非零字节，与最新实际暂存上传 `12280-44941-47.bin` 的全部 1048576 个字节完全一致，SHA256 同为 `9b026b24dd64316dbdcd1bb20a912067b9894af4125f4b2511922ec103e60f8b`；比较见 `font-transfer/byte-comparison.json`。

这比单看菜单偶然恢复更有区分力：前述失败的 r3 INFORMATION 目标停在旧上传，而同一场景使用图像复制可得到完整最新内容。排查入口因此聚焦到原 compute 的 shader/descriptor/SSBO 输入可见性、执行与屏障路径；还不能单凭这一替换区分驱动 bug、宿主状态错误或时序问题。源/目标存储及消费者能够正确表现，是这个局部对照支持的范围。

另从本轮设备的 recipe cache 提取了两个精确 SPIR-V（`font-transfer/shaders/copy-{0,1}.spv`），均为 5256 字节，解包前的 SPRB 文件同时保留。排列 0/1 的差异只有 imageWrite 的通道重排（R/0/0/1 与 R/G/B/1），不是旧桌面 shader dump 冒充当前 Android 程序。


### 图像复制对照仍发生 loading hang（fault 43）

通过字体局部替换后，loading 文案一直完整，随后仍出现 GPU hang。没有到达可人工按确定的完成状态；本轮不能算“loading 后确认”复测成功。最早新 KMD hard error 为 kernel uptime 150979.341306，后续 fault/context 归属和 snapshot/GMU suspended 是检测及恢复链，未取得连续 consumed/retired 与 GTop，因此不把 hard-error 时间写成精确停止进展时间。

原始 `font-transfer/fault43/dmesg.txt:15636–15642` 保留终端颜色字节，以下摘录来自同一日志仅去 ANSI 的 `dmesg-plain.txt`：

```text
[150979.341306] [ T1988] adreno-gen8-gmu 3d37000.qcom,gmu: MISC: GPU hang detected
[150979.341342] [ T1116] kgsl kgsl-3d0: Fault id:2 and GX is ON
[150979.341355] [ T1116] kgsl kgsl-3d0: shadps4.android[31636]: ctx 4 ctx_type ANY ts 91665 policy 2
[150979.341368] [ T1116] kgsl kgsl-3d0: shadps4.android[31636]: cmdline: com.shadps4.android
[150979.341429] [ T1116] kgsl kgsl-3d0: status 00EC2405 gfx_status 00E80004 gfx_br_status 00E80004 gfx_bv_status 00880004
[150979.341439] [ T1116] kgsl kgsl-3d0: BR: rb 12ff/1336 ib1 00000000055CD000/57a3 ib2 0000000004DA4D80/0000 ib3 0000000000000000/0000
[150979.341455] [ T1116] kgsl kgsl-3d0: BV: rb 12ff/1336 ib1 00000000055CD000/02a3 ib2 0000000000000000/0000 ib3 0000000000000000/0000
```

完整日志覆盖至 151020.765637，后续 `dmesg-late.txt` 补充至 151177.357432（fault 后 198 s），检索覆盖与原始命中已追加 `kgsl-coverage.json`；旧 boot 内的重复故障行不重复计数，纯文本派生副本不当作第二份独立证据。

15:22:05–15:22:07 +0800 使用单次采集 guard 取得 `font-transfer/fault43/snapshot.bin`，15430204 字节，SHA256 `133393b33449c61364205bbbde827afc02ddb2e3be4a9cc69750ec08cb2d8b39`，结构校验及 END section 通过。manifest 记录同 boot ID、faultcount 43、sysfs timestamp 271166、读取后 timestamp 0。sysfs 时间与 dmesg uptime 的时钟域没有单独校准，OS/EVENTLOG 也尚未由不可用的 snapshot MCP 解码；保留身份关联边界，不从 faultcount 或文件名推定执行现场。没有为了补证据再触发故障或再次读取一次性节点。

两份当前 SPIR-V 均通过 `spirv-val --target-env vulkan1.3`；这只检查中间码有效性，不证明驱动执行正确。

### 字体局部替换试验的阶段收尾

字体输入转储、绑定日志和硬编码局部复制均从工作代码撤下，完整 patch/APK/输入/输出仍在证据目录。没有将实验替换当成正式修复提交。已恢复 KH3 原 profile 的编译模式（只撤销测试添加的 `vulkan.pipeline_compile_mode`），`debug.mesa.tu.debug` 与 Reshape 测试属性均为空、全局抢占为 1。原服务在本轮 GPU fault 后已结束。

清理后 Android host 与 APK 均编译通过，已安装 `font-cleanup/app.apk`（SHA256 `165c564d14361fb15230ee941b00e400c4312bea5eb42ee33de63af4490e6638`，host `56b9396f2de79e57494bfde3e0b4b2a5d3a604b0faabc80847956079c5356d68`），设备上实际 APK hash 核对一致。该普通构建不包含临时字体绕过，因此不宣称缺字已修复。安装后未再次启动游戏；服务不存在，进程列表无 shadps4/FEX 残留。没有提交或推送本轮修改。


## 独立字体复制复现与分配回归（16:00 后）

新增 `android_font_copy_probe`，直接通过应用相同的 Turnip 加载器运行当前捕获的 `copy-0.spv`（SHA256 `4b68fce29443cd11296d07402fce718a926e2a5aca4bca902967834ea505200c`）。两个 1024² R8 图像、已知且每轮变化的像素、矩形 `(0,0,1024,1024)`、相同 flat buffer 与 push data、128² 工作组、subgroup 64；每轮等待 fence 后逐字节比较 1 MiB。`push` / `set` 分别使用 push descriptor / 普通 descriptor set，`copy` 为 Vulkan 图像复制控制。测试只需着色器与已知输入，不运行 Guest/FEX，不依赖游戏不断变化的图集。

| 条件 | push / set | 证据目录与结论 |
|---|---|---|
| IB32 `e9b29b05…` | 各 32 次均失败 | `font-probe/`；常见旧图、全零或末尾 8 行/列未更新 |
| 原同源驱动 `ba53ae85…` | 各 32/32 正确 | `font-probe/` original 对照；只有驱动/配套原宿主不同，后续同宿主高地址对照进一步隔离分配开关 |
| 相同新宿主，GPUOBJ_ALLOC 去 FORCE_32BIT | 各 32/32 正确 | `font-high-obj/`，仅改变分配限制 |
| 最终范围检查与有限重试 `c22932ba…` | 各 32/32 正确 | `font-address-range/probe-{push,set}.log` |
| IB32，Vulkan image copy 控制 | 32/32 正确 | 存储与读回可以正确工作，不证明所有 compute 同步都正确 |
| IB32，`nopreamble` | 各 32/32 正确 | 隔离线索；关闭 preamble 改变更多编译行为，不作为最终修复 |

同一 IB32 驱动的固定矩形独立存储、CPU 等待、普通描述符集合、`noearlypreamble` / `noubwc` / `nouboopt` / `nodescprefetch` / `fullsync` / `fullnop` / `noaliastex` 均未构成有效修复。简单无 SSBO 的图像复制着色器通过；带矩形 SSBO 的小型 GLSL 也可失败。这使输入加载/执行成为入口，但不能仅由相关性断言某条 ISA 有 bug。STC 改普通寄存器源、扩大 MOVA1/LDC.U 诊断也失败；两个 IR3 源码试验均已撤回。

游戏内的 `font-immutable-rect`、`font-fence`、`font-descriptor` 试验仍有难度/亮度缺字，全部已撤回。`font-bind-off` 轮未取得有效字体对照，停留标题触发的 attract 视频在 Atrac9 `ReadHuffmanValue` 空指针崩溃，不能当作 GPU 故障或字体结果。

### 关闭 preamble 的游戏对照与 fault 45

`font-nopreamble` 使用普通 IB32 APK `165c564d…`、单独设置 `IR3_SHADER_DEBUG=nopreamble`，重命名并保留原 driver pipeline cache。PID 14211、UUID `f1f024981546a90d54c13de213b6c32e`：标题到 loading 的字体均完整，但 loading 仍 GPU hang。属性随后恢复为空，未保留编译器绕过。

一次性采集 guard 保存 `font-nopreamble/fault45/snapshot.bin`：15300224 字节，SHA256 `ac308f04373309b682ad59a9531f05b3b4bc0bfc4ba0f8f27dbb6a8cf040a29e`，END 完整，共 1199 sections。SQL schema 12；Odin3 chip `0x44050000` 的自动 PM4 composition profile 仍不支持，KMD Build-ID 到源码未精确映射，不能套 Swan 结构作为 exact 结论。

`os_state` 原生 current_context=6，seconds=1791706436，PID=14291；这里保留与 app PID 14211 的区别。`v_context_backlog` 第 5 槽 q/c/r 为 55886/55881/55880，原生槽位 context_id=0、推断为空，不能把槽位 5 称为 KGSL context 5。RB section 1166 / VA `0x1ff8000139000` 唯一匹配 preemption record 1174，effective rptr/wptr=3760/3928；待完成 EOP marker 分别为 ctx 6 / ts 55881、55882。当前 BR IB1 候选 `0x5317000`，27360 dword，剩余 21696；BR IB2 `0x4cb1780`，44 dword，剩余 0。待消费的另一份 IB `0x528f000` / 178 dword 是排队后继，payload 未捕获，不能误称为触发者。

按 `v_live_command_cursors`、`rb_cursor_matches` 和捕获 payload 范围解码 IB2，得到两包计算状态 CRB，程序地址候选 `0x4cb0900`。IB1 有界窗口包含 4 个 compute dispatch、event/cache 操作、WAIT_FOR_IDLE 及后续 2D blit；未作 Gen8 ROQ 剩余量补偿，CP 游标也不是精确故障 PC。HFI 各队列 pending=0，不能把残留消息当作活跃 GMU 工作。请求、SQL 结果和 bounded PM4 均在 fault45 目录保留。

### Snapshot 工具的重复映射修复

原工具在不同 MEMLIST 项值完全相同的情况下，总返回第一个相等项，导致 `program_candidate_mappings` 唯一键冲突。开发树 `GpuSnapshotDatabase.FindMemoryIndex` 改为先独立查引用身份，再做值相等回退；保留两条原始记录，不丢弃映射、不改 raw snapshot。Debug 构建 0 警告/错误；fault45 与原来报唯一键错误的 fault43（SHA256 `133393b3…`）均成功落库。只使用本地 debug 二进制离线分析，没有替换用户已安装 MCP 发布包。

### 范围检查版本身份

`font-address-range/identity.json` / `source.patch` 保存精确构建输入。驱动 SHA256 `c22932ba06470e585373db12095153b8d4ab2e27b9d73319705f8dbc742fcff0`；host `f60d83c6e85c8930a1f8da9db6d32d66511ca0405d7d7f9d536b9400410d84a9`；APK `d70cbac9f246bc9e6ef3775af434e9c710694f16b06af4e1d414f746520907fe`。Android host / APK 构建成功。第一次启动时设备在 Dozing，前台门控未启动服务；唤醒后开始真实会话 PID 18692 / UUID `a81f9d48cedfffc65300cc4d21be7ec4`。全局抢占=1、IR3/TU 属性为空，实际 TU_DEBUG 仅有应用默认 `gmem_warmup`，Reshape requested=false；测试 profile 仍使用 async_accurate。标题、难度、确认、亮度、INFORMATION 截图完整保留。


## 高地址版本仍在复制与计算切换边界挂死（fault 46）

PID 18692 的本轮 fault 首个 KMD detector 为 `font-address-range/fault46/dmesg.txt:15539` 的 uptime 155733.824485 `MISC: GPU hang detected`。155733.824522 明确为 `shadps4.android[18692]: ctx 6 ... ts 55561`；BR IB1=`0x41d14fa000` / remaining `0x54c0`、IB2=`0x4033e93d80` / remaining 0。后续 GPU 恢复与应用 device-lost 为结果，未取得连续 GTop/retirement 时间线，不以 detector 时间冒充精确停止进展时刻。

单次 guard 保存 snapshot 15429412 字节，SHA256 `0ef2985863889ee40f5cd3c0805fb35046b5c3dd9258fe62ee70f99361634990`，END 完整。OS seconds=1791707994、current_context=6、ptbase=`0x8fe8e5000`、process_id=18885（同 host 日志 Guest-1 的线程号，与 app TGID 18692 区分）。OS 槽 5 q/c/r=55566/55561/55560，槽位 context_id=0、无推断，仍不能直接用槽号充当 context ID。RB section 1166 / `0x1ff8000139000` 唯一匹配 preemption record 1174，待完成 marker 为 ctx 6 / ts 55561；另一个 RB 的 ctx 3 marker 不是本游戏当前批次。HFI pending 全为 0。完整 SQL、raw ROQ 与指针来源保留，未套用 A6xx 补偿。

BR SQE indexed section 370：slot 0=`0x74b`、slot 1=`0x70138000`、slot 31=`0x74a`，仍在此前固件分析定位的 WAIT_FOR_IDLE handler。IB1 section 1181 共 27200 dword，剩余 21696，但由于预取，不能把二者之差直接当作当前包；本轮向前扩展的有界 PM4 结果 `ib1-before-cursor.json` 找到与 BR IB2 精确相同的调用：

| IB1 dword | 命令 | 原始关键操作数 |
|---:|---|---|
| 5228 | TPL1_A2D_SRC_TEXTURE_INFO | `00500083,00008b00,9756d400,00000041,00000000` |
| 5234 | RB_A2D_DEST_BUFFER_INFO | `00000083,72a7f000,00000040,00000000` |
| 5239 | GRAS_A2D_DEST_TL / BR | `00000000,00000aff` |
| 5247 | CP_BLIT | `00000003` |
| 5249 | CP_INDIRECT_BUFFER | `33e93d80,00000040,0000002c` |
| 5253–5267 | cache/CCU events、register write | 完整 raw operands 见有界 JSON |
| 5269–5270 | WAIT_FOR_IDLE / WAIT_FOR_ME | 无 payload |
| 5294 | CP_EXEC_CS | `0,4,2,8`，在等待之后 |

同源 Mesa 的 format `0x83` 为 `FMT6_32_32_32_32_UINT`，观测矩形为 2816×1，相当于 45056 字节线性复制。MEMLIST 查询显示源 `0x419756d400` 包含在 16 MiB allocation `0x41970da000` 中；目标 `0x4072a7f000` 包含在 2 GiB allocation `0x403fa62000` 中，完整范围不超界。该范围检查不能证明内容、页映射或同步正确，也不能把大分配自动叫作 Guest DMA。

fault45 在对应位置也有相同尺寸的 128 位格式复制、相同长度的计算状态 IB 和同样的事件/等待序列，只是地址不同。**当前计算管线是在等待前绑定，真正的 dispatch 位于等待后**；因此不把已绑定的 CS（fault46 program base `0x4033e92d00`）或 CP 预取窗口里更后的 dispatch 当作正在挂死的 shader。下一项单变量诊断在 A8xx 的计算管线绑定前先发 WAIT_FOR_IDLE，区分前一段复制与状态改写是否相互干扰；此时尚未作为正式修复保留。


### A8xx pre-bind WFI 对照仍失败（fault 47）

仅在 `tu_CmdBindPipeline(COMPUTE)` 前加 WFI；驱动 `31aca038f7a2a1ebfba6cf402209b73d0e58b4156a3cd6a02d4f4bc5176f39a9`、APK `9f731b353cefdae55286c2f87067f99ec050d24e5c4d654c6191041e9bd543ce`。PID 21880 / UUID `4f9d682e4b3ed482185adc91beb86b18`，字体完整，loading 再次失败。`compute-prebind-wfi/fault47/dmesg.txt:15497` 的首个 detector uptime 156577.635921；同块记录 app PID 21880、ctx 6、ts 60229。Snapshot 15300000 字节，SHA256 `37792d384fa69a0ae45a0a08ae66c6b01339d8dbdbedcf532687e2beadf3a617`，END 完整。OS current_context=6、seconds=1791708838、process_id=21959；同样保留与应用 TGID 的区别。backlog 槽 5 为 60235/60229/60228，HFI pending=0；SQL、RB 指针、ROQ 与 KMD 绑定证据完整保留。

BR IB1=`0x4191970000`，27675 dword、remaining 22171；IB2=`0x4032893d80` / 44 dword，remaining 0。SQE section 370 slot 0=`0x74b`、slot 1=`0x70138000`、slot 31=`0x7e3`。有界 PM4 在 dword 5248 是相同 44 KiB CP_BLIT，5250 是新 WFI，5251 是与 BR IB2 精确匹配的调用，5255–5269 为刷新/失效序列，5271 为原 WFI，5296 是 dispatch，5301 又是下一个 pre-bind WFI。因此新 WFI 没有解决；仍不能用预取游标直接在多个 WFI 里选一个当作精确执行 PC。讨论中的“提前等待已通过、卡在刷新后”是从匹配 IB2 顺序提出的候选解释，不是独立 wave/SQE 栈证明的结论。该增加等待的代码已撤回。

下一项诊断是仅在 A8xx、非 renderpass 内的 compute bind 前执行现有待处理 cache flush，改变 flush 与管线状态写入的顺序，不添加新的刷新位。其有效性另行记录，尚不作为修复结论。


### Pre-bind flush 对照被另一类错误提前打断（fault 48）

驱动 `2409cf6fccd9db1ac754a10fb34a9473299425f0b03af384c3f88ccaa21c9959`、host `3765f99cdc029127c4cfc75806f01577fe531a72f17adb5b9cca28c44a972beb`、APK `3b4662e1c143670d2ee35fa856601519ad10845af0a3a5b43fa59412200681a3`。PID 24237 / UUID `39820ff710bf1c4fede1ec0c45fd66d5`，新游戏难度文字完整；随即退出，未进入 loading。`compute-prebind-flush/fault48/dmesg.txt:15608` 的 uptime 157051.731960 首先记录 `CP | Protected mode error | WRITE | addr=0x00000 | status=0x00600000`，后续绑定 app PID 24237、ctx 4、ts 24691。`shadPS4:VkRecor` 的 SIGSEGV 是 device-lost 后的下游错误，不能当作这次 GPU 首因。此实验未到原 loading 观察点，不作为 flush 顺序有效或无效的判定。

单次 guard 的 raw snapshot 为 6028612 字节，SHA256 `84fa203746acd1064ff12eb03009836cd5e0756bc1314691639fd4fde3861a2a`；END 完整，1192 sections。OS seconds=1791709312、current_context=4、PID=24314（保留线程号/TGID 区别）。BR IB1=`0x41904ca000`，19078 dword、remaining 18118；IB2=`0x418ff16e88`，304 dword、remaining 0。SQE slot 0=`0x362`，与先前的错误恢复入口相符；它不是首次保护写的执行 PC。IB2 的页内偏移与此前保护写案例相同，继续保留 4 KiB 命令读取边界这个独立线索，不把它与 loading WAIT_FOR_IDLE 视为已证明同一根因。该顺序调整源码也已撤回。

## 本轮最终保留与设备状态

- 保留 KGSL 命令分配的实际 4 GiB 范围检查与有限重试，移除导致缺字回归的 FORCE_32BIT；未保留两项 pre-bind 试验、IR3 试验或字体专用绕过。
- 保留 `android_font_copy_probe` 回归诊断目标；最终 probe 修正参数校验、输入读取检查及图像复制控制的 barrier access mask 后重新编译；push / set / copy 三种模式各 32/32 正确（`final-probe-*.log`）。
- 重新安装已验证的 `font-address-range/app.apk`（`d70cbac9…`），设备 `base.apk` SHA256 与归档完全一致。驱动回到 `c22932ba…`；源码和最终 Android host / probe 构建通过。
- KH3 profile 只撤销测试添加的 `vulkan.pipeline_compile_mode=async_accurate`，恢复原空覆盖；字体游戏对照是在 accurate 模式下完成，不把恢复后的默认 async skip 当作已重新上机验证。全局抢占恢复/保持 1，IR3/TU/font_fence 测试属性为空；Reshape 未开启采集。
- APK 安装后未再次启动游戏。服务与 shadps4/FEX 进程均已结束；新的 fault46–48 snapshot、日志、SQL、补丁、APK 全部保留。没有提交或推送。

本轮确认并修复的是 **字体复制的分配回归**。KH3 loading 的 GPU hang 与高地址命令保护写仍未解决；已有更窄的传输/缓存/状态切换窗口，但尚缺精确执行点和能稳定通过游戏的修正，不能把候选解释当作最终硬件根因。


## 19:00 后的补充对照与提交检查点

按同机精确 SQE 固件再次查询 fault46/47 的 indexed section 387：`0x7008` 分别为 `0x1497` / `0x1499`，BR stat 的 packet=`0x70138000`、stack0=`0x8cc`；对应 IB1 dword5270 / 5272 的 WAIT_FOR_ME。WFI pending 分别为 5 / 6。该核对修正前文未使用控制偏移时的边界：前一次 pre-bind WFI 增加的是待完成 token，不代表已等到后端完成；等待后的 dispatch 不是此时已执行的命令。SQL 见两份现场的 `firmware-wait.json`。固件字节已按独立分析记录核对；KMD 精确源码映射仍未完成。

字体修复 APK `d70cbac9…` 下启用 `TU_DEBUG=syncdraw`，PID 6018 / UUID `65b3c4d1e83034f7c40986dda84d5e41`，启动阶段出现另一类错误，尚未到 loading。`syncdraw/dmesg.txt:15442` 首报 uptime 164314.955183 `Protected mode error | WRITE | addr=0x08837 | status=0x00608837`，同块绑定 ctx6 / ts5658。单次采集 guard 保存 fault49：6419544 字节，SHA256 `3eaca4c5f8cc956d260f6272151e46cabd605d767cad7113b98a152d0edb94f3`，END 完整；SQL 原生 seconds=1791716575、current_context=6、process_id=6099（线程号，与 TGID 区分）。IB2=`0x418e856eac`，307 dwords；dword38 的 130-payload CRB 跨 4 KiB，捕获副本中含正常形状的 0x8837 写入。SQE PC=`0x362`，仍是恢复现场，不能把该寄存器或后续 draw 直接当作首因。

随后仅在 GROW 命令流的 CRB builder 中加可选页对齐诊断。APK `f372662679c9dff60d73793eb9b5d10e0b27c428a315607c5a95aed056e4060b`，驱动 `69c8065608ae30d5b5a3ed18ce2b3356d98b964b716cbb64f41d87434a093f40`，运行 `crb_page_align,gmem_warmup`。PID 8289 在启动阶段再次故障，未完成 loading 对照；KMD uptime164611.225532 绑定 ctx4 / ts5581，IB2=`0x418e9f6e88`。fault50 由 guard 保存为 6419284 字节，SHA256 `ebca918dde900a1105b438fb2e14bce552cc817d7a04c5afaf1d532dc9aea7b6`；本检查点尚未 SQL 解码这份新现场，也未证明诊断覆盖了出错的 CRB 发射路径，因此不作页边界假说成立或排除的结论。

用户要求先 commit/push，设备实验在此停止。诊断源码仅留本地（`crb-align/source-diagnostic.patch` 和 Mesa 未暂存改动），不包含在主仓引用的驱动提交中；原始 snapshot、日志与 APK 留在忽略的证据目录。两份新 dmesg 的完整检索已追加 `kgsl-coverage.json`，同 boot 重复行不当作新 occurrence。测试属性恢复为空，KH3 profile 撤销 accurate 覆盖，设备重新安装字体修复 APK；服务结束后未再启动游戏。

提交内容为此前完成的会话/手柄/状态层改动、模块查询与固件保留、命令分配范围检查、独立字体复制诊断及排查记录。**不包含 KH3 GPU hang 已修复的声明。**
