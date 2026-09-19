# TMNT 实测帧调用、语义名称与主函数 C/C++ 重编译

日期：2026-09-16。主仓 `codex/android-fex-round2` / `4da582b7` + 保留的既有 dirty 工作；本轮未 commit/push。AYN Thor `9c2841a4`，API33、ARM64、4KiB、Turnip；未操作 Swan。

当前交付围绕“实机一帧的主要 guest 函数 → 可维护的语义源码 → 真实 guest C/C++ 替换”。之前只处理四个提交 helper 并不能代表完成整帧重编译。本轮新增一个实测主要函数的完整实现，其他已导出的函数继续区分原始实现、计时包装与待重编译源码。

## 已完成与明确边界

- 主帧内部341个调用点，加 Begin/Submit/Job 函数，共384个探针，未按64个截断。静态未执行路径不冒充动态调用。
- 第一次屋顶捕获197个保留帧，42个直接目标、20个未解析间接调用点。新 C++ 包在同 PID 的 generation2 屋顶捕获200个保留帧，49个直接目标、23个未解析间接点。两次均额外排除首尾2个边界帧，不把199/202当作最终完整帧数。
- [统一符号索引](../../../guest/games/CUSA50828/01.08/symbols/index.json) revision5，57个名称、9个已有类型化符号、2个聚合类型。包含帧根与49个实测直接目标。名称都有证据与 `candidate/correlated/proved`，不使用地址或 `sub_...` 作为主显示名。地址仍是模块相对身份，不能因改名改变探针归属。
- [帧列表分发与回收 C++](../../../guest/games/CUSA50828/01.08/frame_dispatch.cpp) 对应 `TMNT_DispatchFrameListsAndRecycleBlocks`。它真的执行列表遍历、虚调用、队列锁和双 bank 回收；不调用原函数代替实现。与之前四个 helper 组成 `tmnt_frame_recompiled_v2`。
- **整帧主函数 `TMNT_FrameCoordinator` 仍执行原始函数，未完成整个主循环的 C/C++ 化。** 导出的50个函数源码都是分析材料，除明确实现的函数外不能宣称可编译等价替换。23个间接点的动态目标、worker任务因果关系、浮点/异常语义仍需逐步恢复。

## 可持续使用的入口

- [源码与状态清单](../../../guest/games/CUSA50828/01.08/README.md)、[逐函数状态](../../../guest/games/CUSA50828/01.08/frame-workset.json)。
- 本机完整命名源码：`build/frame-recompile-20260916/frame-source/`，例如 `TMNT_FrameCoordinator.decompiled.c`、`TMNT_EnqueueJobAfterSequenceMarker.decompiled.c`。
- 本机交互帧图：`build/frame-recompile-20260916/frame-flow-named/frames.html`，p50/p95/最长帧、时间门槛可切换；完整每帧数据位于同目录 JSON。首列和时间线使用语义名，地址与证据保留在详情中。
- [保留帧汇总](frame-recompile-20260916/frame-summary.json)、[产物 SHA](frame-recompile-20260916/artifacts.json)。原始 PROF、大型分析数据库与失败尝试保留在本机 build 目录；不伪造可远程下载地址。

共同命名覆盖 `DispatchFrameListsAndRecycleBlocks`、`ProcessPhaseRecordBatches`、`UpdateEnabledObjectSubtree`、任务 marker/队列 barrier、限时请求/回调、输入事件、排序与音频控制。`ProcessPhaseRecordBatches` 尚不能直接命名为 RenderScene；输入边沿候选名仍需动态电平校对。FMOD 的10个新增公共方法名称有原库错误字符串与精确 C++ mangled-name → PS4 NID 双证据，包括音量、暂停、DSP时钟、事件播放/采样加载状态与释放；`fmodf` 是浮点取余，不是 FMOD 音频。

## bulk 导出与工具修复

`reverse_study.recompile_facts` 一次 native 查询返回完整原字节、全部指令/CFG/代码与数据引用、ctree、局部变量寄存器/栈位置、早期和最终 microcode。`guest_recompile_bundle` 一次请求导出整个选定工作单元；省略 targetOffsets 就消费实测帧请求。伪代码64KiB、函数128KiB、CFG1000项以及旧 IR/ctree 输出上限已移除；非连续函数 chunk 仍明确拒绝，不静默漏块。回复返回路径与 SHA，不重复把大段源码跨工具传输。

最终 FrameCoordinator：5840条机器指令、1278个CFG块、1296个局部变量位置、10688条早期IR/3501条最终IR、约192KB完整伪代码。保存数据库后使用 native original-input SHA 验证源身份，单独记录数据库 SHA；重新打开命名数据库不再被误当作另一个可执行文件。

新 `guest_frame_workset(symbolIndexPath=...)` 校验符号 shard SHA、程序/模块/runtime SHA、CPU、字节序和地址模型，将同一索引固定到下一步请求。`guest_recompile_bundle(applySemanticNames=true)` 在自有 workspace 显式批量导入名称；拒绝已有语义名称冲突，不猜测原型。`guest_auto_tag_plan` 同样使用这些名称。真实保存数据库生成384个新命名探针，与旧计划逐项比较 ID、kind、begin/end字节、caller/callee位置完全一致。

修正了一个实际分析陷阱：错误的 mutex lock“不返回”类型截断了任务入队函数伪代码。26个已知公共导入/已审核函数声明经原型门控写入独立数据库后，两种入队函数完整恢复。类型只在分析数据库更正，未改变游戏代码。GOT/TLS等未恢复常量仍不得直接作为 C++ 常量。

共享源码和技能在 `/Users/bytedance/workspace/spatial_mcp_publish/dev_tools`，公开 MCP 为 `reverse_study` / 默认 `study0`。本机 release `0.3.0-local.20260916.frame2` 通过包管理器安装；Codex指向新版本，六项Spatial条目的args/env保留，Doubao11个wrapper均initialize/tools/list通过。现有已运行的旧 MCP 进程未强制终止，新连接使用新版。技能同步到本机目录，含多CPU约束和语义名称要求。

## 真正的 C++ 函数与验证

新分发器精确保留：对象列表和全局列表分别遍历；每次虚调用前重读vtable；回调后重读列表头/next；对象+24的单16-byte清零及+40的4-byte清零；原全局mode读取；`LOCK CMPXCHG 0→100`，mode2跳过当前bank，两个pending链表回收；原MOV unlock；返回32-bit零扩展bank offset。共享全局绑定原 guest 存储，函数仍在FEX运行。

- 原机器码与新 guest C++ 隔离差分16个场景/17checks，全通过：空列表、两bank、mode2、回调改next/vtable/global-head、整块内存、返回值与回调顺序。不是对真实游戏状态同时跑两遍，未据此宣称覆盖所有竞争/异常输入。
- 既有自动探针定向48/48，新跨RX mapping/page预期字节与映射代次检查8/8。
- Reader/语义索引29项；contract8项。只做受影响的定向检查，未完整回归。
- 普通 APK PID31258/gen2/context33 实际屋顶 MOVE→ATTACK，`GAMEPLAY_REVIEWED`；counter15记录新C++分发器2304次执行（每256次上报）。包 SHA `882c3181b0b20eaa165fb337884e9335d88b699e75c37adcf502f3fb1a88eb02`。
- 本轮修复自动探针跨4KiB/拆分RX mapping时的错误“代码已修改”拒绝。逐片验证可执行字节与映射代次，未放松普通VM权限/Pin；默认未安装探针行为不变。FEX/Foundation子仓未在本轮修改。

新版捕获的平均 inclusive elapsed（含子函数、host等待、调度，不能相加或当作纯CPU）：

| 语义函数 | ms/帧 |
|---|---:|
| SubmitFlipRotateBuffer | 42.621 |
| DispatchFrameListsAndRecycleBlocks | 15.507 |
| JobQueueDrainBarrier | 6.690 |
| ProcessPhaseRecordBatches | 3.232 |
| BeginFrameAcquireBuffer | 1.263 |
| EnqueueJobAfterSequenceMarker | 0.708 |
| UpdateEnabledObjectSubtree | 0.698 |

未做新384探针的未安装/启用/未安装性能A/B/A，不从这些数值声称加速。PROF边界和完整性诊断保留；无“全局无丢失”结论。

## 重编译方法选择与后续工作方向

当前确实存在能够帮助恢复寄存器/栈位置与重编译ABI的工具研究：[SCRIBE](https://github.com/purseclab/scribe) 使用特定LLVM/Clang15插件及若干编译修正，[PRD](https://arxiv.org/abs/2202.12336) 研究部分反编译/重编译。它们可以指导证据导出和函数边界设计，尚不能直接接到本项目NDK/SDK9.4并生成已验证代码。[Remill](https://github.com/lifting-bits/remill) 提供ISA→LLVM语义，适合难处理机器语义的对照；[N64Recomp](https://github.com/N64Recomp/N64Recomp) 与 [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) 可参考runtime/state边界，不能据此假定已有x86-64方案。[反编译器本身的限制](https://docs.hex-rays.com/user-guide/decompiler/limit) 决定了伪代码导出完成也不代表源码恢复完成。

后续直接沿当前50函数工作单元继续：先恢复 `ProcessPhaseRecordBatches` 和 `UpdateEnabledObjectSubtree` 的对象布局/虚调用契约，组合进现有SDK、原函数绑定和差分fixture；对重要间接点按需深入一层，保持同一语义索引。最后再处理大型FrameCoordinator中的内联主逻辑，而非再次退回三个外层等待或只加计时wrapper。性能等待链仍需把上述函数的host/GPU区间联合判读，不能通过改名或去锁宣称已定位GPU瓶颈。


## 最终清理状态

已停止捕获和自动输入，owned study0 workspaces 均关闭，没有guest/native debugger挂接（TracerPid0）。
旧自动探针已disable、next-session profile已clear；恢复到同PID/gen3时确认 `disabled_at_startup`，不是只停记录仍保留IR探针。
gen3实际达到屋顶、MOVE→ATTACK及角色/镜头移动；随后设备前台切换到Android设置，Surface退出，15:02:10日志为正常Cancelled，进程仍存活。没有崩溃证据；未把这次未完成的warmup提升成通过。
`restored-unarmed` 因错误physical-display ID拒绝，`restored-unarmed-primary` 因Session退出导致缺失guest_flip而ERROR_UNVERIFIED，后续只读复核等待也未完成；均保留失败记录。
没有重新抢回设备前台。设备上的C++包仍是v2，下一次启动无auto probes。gen2独立 `GAMEPLAY_REVIEWED` 与失败恢复记录分开。

最终安装产物：[完整SHA](frame-recompile-20260916/installed-artifacts.json)，APK `9e28169c`、host DSO `ae8043a2`、JNI DSO `1a152e8e`；[源身份](frame-recompile-20260916/source-identities.json)。
