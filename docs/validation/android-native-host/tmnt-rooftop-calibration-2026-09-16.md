# TMNT 屋顶主流程标定与 Android 面键映射

本轮完成主帧、绘制准备、提交翻页、任务等待和 FMOD update 的 guest 入口标定，
通过真实 FEX guest C++ 补丁与 host/litep/GPU 时间对齐。修改面键策略与设置并安装 APK。
**没有修改调度器、取消 GNM 门控、替换 mutex 或宣称性能问题已修复。**

## 版本与证据

- 主仓 `4da582b7` + 保留的 dirty 工作；host/JNI 沿用上一轮，均为 RelWithDebInfo；FEX Release。
- APK `9c28a1b90e383835a38ef852bd09bc87c2729f5107e06e66ea091bc05f83d2b0`。
- Host Build ID `8bf15985aef4038795e3555e0061f4fcca31e600`；JNI `57cfd575ea3cd5886634999202124155a825aa67`。
- AYN Thor `9c2841a4` / API33 / 4KiB / 同一 Turnip；未操作 Swan。
- 新配方 `tmnt_rooftop_v1`，包 SHA `0e2c68f224a7b2581d6b13758f1b8996064e715c8589eb3a04b586a8b8ea3e35`。
- 实测 PID8463 / generation1 / context1 / UUID `1c9aee872e67e28297d55a653dacd1b6`。
- [证据目录](tmnt-rooftop-20260916/)与[完整身份清单](tmnt-rooftop-20260916/artifacts.json)。
  原始 PROF/perf/ELF/APK 留在本机 `build/rooftop-profile-20260916` 及清单指定路径；不提交游戏或二进制。

先前无新主流程补丁基线 PID32104/gen2：45s simpleperf 6474 samples / lost0，
47.795s 窗口 12.574 FPS。新采样 PID8463/gen1：40s simpleperf 5177 / lost0，
42.830s 窗口 12.678 FPS。这两个窗口不是同一帧、角色位置也不同，不能据此宣称加速。

## 标定入口

[配方](../../../guest/games/CUSA50828/01.08/rooftop.recipe.json)、
[C++ 实现](../../../guest/games/CUSA50828/01.08/rooftop.cpp)、
[SHA 固定的符号索引](../../../guest/games/CUSA50828/01.08/symbols/index.json)。
全部限定 CUSA50828 / 01.08 / 原始 eboot SELF SHA
`6122da7190de6b08d921b2c42c3ca9ed11dc4d11524f1139ff67aeceea5b204d`。

| eboot 相对地址 | 重建名称 | 输入/返回契约 | 证据 |
|---|---|---|---|
| `0x9e560` | `TMNT_FrameCoordinator` | RDI opaque self；保留 RAX | 包含 update、绘制、任务等待；末尾恢复栈并 tail-jump Submit；屋顶每帧一次 |
| `0x1545ce0` | `TMNT_BeginFrameAcquireBuffer` | 无入参；保留 RAX | 检查三缓冲 label，必要时 WaitEqueue，重置/初始化命令缓冲 |
| `0x1545f20` | `TMNT_SubmitFlipRotateBuffer` | 无入参；保留 RAX | 经过 `0x19630` 提交翻页，资源退休，索引模3递增 |
| `0x1591570` | `TMNT_JobQueueDrainBarrier` | RDI opaque queue；保留 RAX | 队列尾加入完成任务，用线程局部 semaphore 等待；空队列可直接返回 |
| `0x15c0b20` | `TMNT_FmodStudioUpdatePlt` | RDI this；EAX FMOD_RESULT | RIP-relative PLT → `H+G1XQRDyWk`，复用已核对的 FMOD 公共 ABI |

生产 loader 自动解析模块基址。本次 `eboot.bin` 基址 `0x400000`，入口安装结果有逐指令
trampoline map；不把这个运行时基址硬编码进配方/符号索引。
主帧入口在静态索引里没有直接调用者，但真实计数器命中；不能以静态无 xref 断定死代码。
没有索引到跳入 stolen prologue 中部的入口。内部函数返回的真实 C++ 类型仍未知，
`uint64_t` 表示此补丁保存 RAX 的约定，不能拿去推断对象布局/返回值意义。

原始解密 ELF SHA `cf14264107ef499c5fcaffc97386dcee25652c16d3975d1be27edc4e7e40ede5`。
IDA 原生 SCE ELF 导入失败；仅在分析副本将 ELF e_type `0xfe10` 改为 ET_DYN `3`。
text 未变，偏移逐项核对；未修改运行游戏内容。IDA 对未应用的 SCE relocation、栈 canary
会产生错误常量，因此没有照抄这些伪代码类型/常量。分析副本 SHA 单独留档。
符号源绑定原始解密 ELF，并显式列出允许的原 SELF runtime SHA。
符号/原型使用 `correlated`，不是厂商原符号；MCP `symbols_prepare` 验证5项 typed symbols通过。
未声称已在当前 RSP 会话完成符号绑定；本轮性能测量始终未挂 debugger，IDA workspace 已关闭。

每个 wrapper 使用栈局部 start/end，调用 typed original，返回原 RAX/EAX；没有共享 start槽。
SDK上报发生在所测调用外，计数器值保存开始时间和耗时，可按物理 TID/单调时钟和 host 对齐。
父帧仍包含子 wrapper 的计时/上报成本，且所有墙钟时间包含调度等待，不是 guest CPU cycles。
这是明确选取的5个函数，不是自动标记或全调用跟踪。

## 屋顶与主帧结果

正常 APK 启动，实际触屏 stick 移动改变角色/镜头，MOVE→ATTACK；
[场景](tmnt-rooftop-20260916/frame-0011.png)、[响应](tmnt-rooftop-20260916/frame-0017.png)，
warmup `GAMEPLAY_REVIEWED`。随后自动输入关闭，在静止屋顶进行性能采集。

35s PROF，主线程物理 TID9112：frame/begin/submit/fmod 各444次完整返回，jobs3625次。
去掉首尾捕获边界，442帧都具有有序的 begin/submit 和单个 FMOD update。

| 主帧连续区段 | 平均 ms | P95 ms |
|---|---:|---:|
| 主帧入口 → BeginFrame | 7.459 | 10.814 |
| BeginFrame | 1.295 | 1.998 |
| BeginFrame 返回 → SubmitFrame | 22.255 | 28.089 |
| SubmitFrame | 47.645 | 124.917 |
| 返回收尾 | 0.023 | 0.070 |
| **主帧总计** | **78.677** | **153.347** |

FMOD update 平均0.095ms/P95 0.263ms。任务 barrier合计平均7.553ms/帧，
其中1.196ms在绘制准备前、6.357ms在命令构建段；它们已包含在表内，不能重复加到总计。
主帧是约12.7FPS的真实 guest 主循环；本轮没发现固定66.6ms sleep限速证据。

Host重叠按同一TID、同一单调时钟计算（444区间统计，边界单列保留）：

- Submit wrapper平均47.767ms，其中 `sceGnmSubmitAndFlipCommandBuffers` 47.634ms，
  `GNM.SubmissionGate` 47.357ms。该scope是等待 `submission_lock==0`，不是 GPU busy计时。
- Frame的HLE区间去重后62.754ms/帧；减去它的剩余时间不能直接当作纯 guest计算，
  还含调度、SDK和未标记路径。`sem_wait`重叠6.138ms/帧；四种 mutex lock/unlock
  主线程合计约2.743ms/帧，跨界/外围检查可能落在scope外，不代表完整mutex成本。
- GpuComm TID9134：`Vulkan.SubmitExecution`总26.481s，其中等待共同submit mutex12.867s、
  `Vulkan.Submit`（实际queue.submit及小量标记）13.549s。
- Present TID9170：对应27.892s / 13.002s / 14.740s。这些是不同线程的重叠墙钟，不能相加
  宣称超过35s的CPU占用。相同VkQueue通过mutex外部同步；不能直接删除锁。
- `VideoOut.Prepare`本轮平均19.778ms，和早期66ms不同；真正较稳定的是完整guest GPU区间。
  不应把以前的一个scope名称永久当根因。

GPU timestamp：完整 `GPU.GuestFrame`442项，平均67.329ms/P95 71.481ms；
HostPrepare0.395ms，PostProcess0.393ms，Present0.585ms。GPU区间包含semaphore/内存等待，
不是67ms全为shader执行。当前Turnip无calibrated timestamp，CPU对齐估计误差最大±5.811ms；
适合看粗粒度重叠，不能据它判断亚毫秒先后顺序。

740 chunks有效、0 skipped，无中间gap marker；decoder仍标记truncated，有首尾未配对/未闭合
CPU spans，已保留全部诊断，不宣称全局无损。主帧连续分解剔除首尾，GPU区间与counter独立核对。
分析脚本和逐帧数字见证据目录，原始文件hash见清单。

## 开销对照与后续优先级

同一PID/gen/场景，ring/GPU继续开启、无file capture/simpleperf、各15s：

| 主流程补丁 | FPS | SDK调用变化 |
|---|---:|---:|
| 开 | 12.782 | +9361 |
| 关 | 12.804 | 0 |
| 再开 | 12.735 | +9288 |

这个短对照没有发现明显FPS变化，不是零开销证明；停用后SDK计数不增长，guest正常继续。
正常Stop达到 `Stopped/user_stop`。本轮没有性能优化实现或全回归。

下一步应沿这条连续链路定位：guest SubmitFrame → GNM准入门控 → Liverpool处理/Flush →
GpuComm与Presenter的共同queue.submit。先判定长submit来自驱动排队、GPU同步/事件，
还是频繁overlay/blank提交加剧串行；必须保存PM4/标签/IRQ顺序，不能把门控或queue锁当冗余删除。
另一块是22ms命令构建段，可继续对其内部render job做少量同样的guest标定。
FMOD update不是这个稳定屋顶窗口的优先优化对象；这不否定启动加载的FMOD等待。

## 面键修复

最终按用户明确确认的映射，设置 → Controllers 中新增 **Xbox / Nintendo 面键翻转**，默认不勾：

| Android面键 | 默认 | 勾选翻转 |
|---|---|---|
| A | 圈 | 叉 |
| B | 叉 | 圈 |
| X | 三角 | 方框 |
| Y | 方框 | 三角 |

原生游戏链路之前绕过ControllerProfile，直接把Foundation标准South/East/West/North送入
Orbis。现在在shadPS4 app边界应用button映射与flip，Foundation中立位置ABI和PS触屏不变。
设置按slot持久化，Session启动使用game override或global；旧JSON缺字段时false。
捕获/冲突替换/示意图使用翻转后的有效绑定，避免画面显示和存储操作相反。
热重配置先退休旧设备epoch与held buttons，触屏独立状态不被清空。
此轮只补按钮/HAT→按钮策略，未宣称完整analog任意重映射/传感器支持。

验证：runtime单测95/0、settings24/0；实际普通测试APK `NativePadInstrumentedTest`7/0，
枚举真实设备并注入带该device ID的KeyEvent，经过Foundation source → JNI → production scePad。
覆盖四面键双布局、按下/松开、held状态切换、overlay独立性与已有epoch/生命周期。
这不是人工物理按键验收。APK正常游戏触屏移动另有上述真实画面证据。
