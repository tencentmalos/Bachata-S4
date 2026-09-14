# 图形调试修复、GPU Reshape 接入与 Citron 并发分析

2026-09-14，基于 `6062df92`，主分支 `codex/android-fex-round2`。按用户要求直接修复，不新增或修改执行 spec。FEX、Foundation、Citron 的源码和 pin 均未改动；保留原有 Bachata-S4 / dear_imgui 工作区。

## 本轮结论

控制面、RenderDoc coordinator 和真实推进计数的上一轮反例已修复；GPU Reshape SDK 已接入普通 APK / 私有 Turnip；迁移了 Citron 的有界 handoff 采集与物理线程区间分析，并修正四类并发/同步问题和 HLE 热路径计数开销。**这不是整个图形工具 spec 收口，也不是游戏可玩验收。**

关闭 SDK 的 120 秒真机运行与单 shader 插桩的 120 秒运行均通过主动 Stop。明确指定 AYN 主物理屏的截图确认启动 Logo、游戏对话框和背景能够显示，文字仍然破损。这个问题在不插桩时也存在。全量插桩仍有 descriptor 映射告警，不能用它直接证明游戏越界。单 shader 模式只证明一个指定 SPIR-V 能完成替换、提交和收集，不证明其他 shader 或所有 GPU 访问安全。

原始结果、阶段产物和 SQL 在 [证据目录](graphics-toolkit-2026-09-14/)。`final-artifacts.json` 是早期修复阶段；Status/capture阶段见 `status-capture-artifacts.json`；最新R8文字修复见 `font-r8/artifacts.json`。构建前的 source HEAD 是 `6062df92+dirty`，源码 SHA 清单保留当时身份，不把后来的提交号伪装成构建时 SCM。

## 已修复的控制与抓帧问题

- 真实 `GuestSubmission`、PM4 packet 消费、Vulkan queue submit、draw/dispatch、guest flip、host present 分别发布；不再把 guest 入队叫作驱动提交，不从另一计数推导 present。GPU retire 的总量仍标记 unavailable。
- session generation / process run UUID 随 publisher 固定；设备路径使用 PID、generation、单次运行标识，避免不同进程的 generation 1 追加到同一证据文件。终态保留计数、阶段、Stop 原因；已结束会话不再报告 active。
- CaptureCoordinator 使用原子只读快照；Query 不等待驱动 End。请求从一个 present 边界开始，到下一个边界结束，避免同边界空捕获；取消包含 request ID 和 generation，旧请求不能撤销新捕获。
- watchdog 处理超时和 backend discard；清理失败保持占用和明确失败状态，不能开始第二个捕获。Presenter 持有捕获绑定，析构先关闭绑定再销毁 Vulkan/窗口对象；VideoOut 锁不覆盖捕获结束的慢操作。
- Android 在实例创建前及 renderer 绑定时寻找 RenderDoc；receipt 包含实际文件路径、大小、SHA256、请求/实例身份，只有 backend Finalize 成功才 Ready。JNI debug command 编译门控绑定 debug APK。

**后续已取得真实 RDC 并在 Android 上以同一 Turnip 成功 replay。** 早期 `capture_launch` 的 ABI unsupported 仍保留；最终通过正常 Android GPU debug layer设置和普通APK内的 coordinator完成捕获，不以mock代替。`profiler_ring`、Foundation ImGui Layer、完整 decoded/raw PM4 导出还未完成，命令继续明确报告未实现。Auto tag 仍延期。

## 真帧捕获与同驱动回放

普通 APK 第105秒请求capture，155秒观察/Stop JUnit通过。request1/generation1，run UUID `697a9745cf360ce38a6ffad319609d18`，first_present876、last_present877，receipt确认为Ready。RDC223818013bytes，SHA256 `9ee847367cc38db45fa839293d0ed24d74b2b720ffa7c43edd6570ea6533d83b`；文件保留于本地 `build/graphics-toolkit-review/renderdoc/tmnt-dialog.rdc`，不提交游戏捕获二进制。四个GPU debug settings测试后恢复原值（原为null）。

配对 MCP 的第一次 replay 使用系统Qualcomm ICD，缺少捕获所需 workgroup_memory_explicit_layout扩展而失败。主仓新增 `cmake/renderdoc` 的显式Android replay loader factory及配对源码补丁，在隔离worktree的实际ABI基准 `6ae929af` 上构建，未更改有其他改动的RenderDoc主工作区。factory加载与游戏完全相同SHA的bionic Turnip及相邻四个hook，显式失败不回退系统驱动。以原签名更新replay APK；SDK APK、补丁、driver/hook哈希见 `renderdoc/paired-renderdoc-build.json` 与 `turnip-replay-files.json`。

实际replay报告Turnip Mesa26.0.0-devel/5ac41be677，offline warmup通过：60actions、138resources、16textures、28outputs，6个默认materialization Ready。deep pipeline/pixel history仍pending，不把pending当成无数据。随后在同一配对工具的远端会话完成字体draw/descriptor/shader/texture检查；guest frame候选本身已含破损，早于StatusLayer合成。两种候选图不等于自动证明完整最终输出等价。普通replay APK未打包GPU Reshape replay provider；主游戏SDK证据保持独立。最后关闭remote capture/controller，并将 `debug.rdoc.vulkan_loader` 恢复为空。

## 字体调查与R8修复

详见 [根因、修改与复验](font-r8-tiling-fix-2026-09-14.md)。TMNT没有Font/FontFt导入，自带图集在guest内存中完整。当前Turnip支持shaderInt8但不支持8位SSBO，原host R8 shader非法依赖StorageBuffer8BitAccess。改为32位打包后真机6组正反向平铺均精确匹配CPU参考；普通APK文字已恢复，StatusLayer和全屏保留。以下早期“文字仍破损”的画面属于修复前证据，不能再当成最新状态。其他画面/游戏/API兼容和完整工具框架仍未验收。

## StatusLayer 与 Android 沉浸式显示

按用户最新优先级补齐可见 StatusLayer：复用主仓现有 ImGui context/backend，由每个 Presenter 独立拥有。参考 Citron 的分组，显示实际成功 present 的 FPS、Guest flip/s、最近240次新帧间隔曲线、当前/平均/Min/Max 毫秒值与16.7/33.3ms参考线，以及 Surface、generation、draw/dispatch/s。复用旧帧不计入新帧 FPS，停顿保留真实峰值，跨 Session 不累加；没有用 ImGui 自身刷新频率代替游戏速率，也未宣称 GPU 时间。

帧历史11项、overlay command58项等合计242/0；StatusLayer真机150秒Stop通过。截图中顶部 Android 状态栏来自调试 Surface Activity 漏设沉浸式，正式 Compose 入口也只隐藏一次。新增共用 SessionSystemBars：首次进入、恢复焦点和恢复 Insets 控制时隐藏系统栏，允许边缘滑动临时显示，离开会话时恢复原可见状态/behavior；不改变输入焦点。修复后普通 APK 真机120秒Stop通过；通知面板关闭后仍隐藏。WindowManager明确报告 STATUS_BAR/NAVIGATION_BAR invisible，实际 Surface1920×1080（原1920×970）。`immersive-restored.png` 可见清晰 StatusLayer/FPS曲线和仍损坏的游戏文字。

这实现了 StatusLayer 内容，没有替换为 Foundation 的完整 Layer/litep框架。旧 APK结果的身份保留在 `final-artifacts.json`；Status/沉浸式实测 host Build ID `f1f1de4047a3a1b468d741ce5b10dff45fbbdc9d`、JNI `c393a559f40fd18412ce07667fdfe9df52c6fb5e`，APK及SDK哈希见 `immersive-artifacts.json`。窗口行为遵循 [Android沉浸式文档](https://developer.android.com/develop/ui/views/layout/immersive)。

## 与 Citron 的对照和直接修复

参考本机 Citron `6a86baf4b6521ca3ee5b2f5e2a0ca6a8e6007cc0` 的 `src/common/pipeline_handoff.*`、`skills/citron-stage-concurrency-analysis/scripts/analyze_pipeline_handoff.py` 和 `src/video_core/renderer_vulkan/vk_scheduler.cpp`。该本机提交此前未发布；本轮没有推送 Citron。主仓迁移代码保留来源及 GPL 声明，不依赖另机取得这个本机 pin 才能使用。

| 问题 | 原实现的具体风险 | 本轮处理 |
| --- | --- | --- |
| Liverpool 队列发布 | task 入队后先解锁，再增加 `num_submits`；已有活跃 consumer 可先消费并递减，导致无符号计数下溢 | 在同一 queue 锁内完成入队和计数发布；`num_mapped_queues` 改为原子访问 |
| Vulkan 提交锁范围 | 全局 queue 锁覆盖命令缓冲结束、池分配/等待、pending callbacks，额外串行化不同 scheduler | 对照 Citron，把 queue 锁收窄到真实 driver submit；回调先出队再在 pending 锁外调用，避免重入再次执行同一回调 |
| Present semaphore 等待阶段 | frame-ready 被放在第二个 wait，但旧静态 mask 的第二项只有 COLOR_ATTACHMENT_OUTPUT；前面的 blit/transfer/fragment 读取不受保护 | SubmitInfo 每个 wait 自带 mask，默认 AllCommands；Present 两个 wait 覆盖实际读取阶段，不按数组位置猜用途 |
| Recreate 锁和退出 | QueuePresent 锁跨整个 Recreate；GPU priority worker 在 timeline 超时中不能响应停止，非 Timeout 错误可能无限重试/线程异常退出 | Present 解锁后 Recreate；仅 `vkDeviceWaitIdle` 显式持 queue 锁；worker 每 50 ms 检查 stop token，非 Timeout 和 counter 查询失败抛给 owner，析构显式 request_stop / notify / join |

Vulkan 要求 queue 外部同步，wait 的 stage mask 决定等待的同步范围；上述修改依据 [vkQueueSubmit](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html) 和 [vkDeviceWaitIdle](https://docs.vulkan.org/refpages/latest/refpages/source/vkDeviceWaitIdle.html)。不能把“缩小锁”解释成允许多个线程无锁调用同一 queue。

额外检查发现每个 HLE 调用都会对同一个启动日志计数器执行 seq_cst fetch_add，即使前64次日志早已用完。现在先 relaxed load 检查预算，预算耗尽后不再执行共享 RMW，也避免计数回绕重新刷日志。这是源码上消除无用开销，未做同场景前后性能收益测量。

Android WSI 的 create/acquire/present/capability 失败改为由 session 处理的异常，避免已销毁 Surface 导致断言终止 APK；SurfaceLost 不原地复用死亡 ANativeWindow。定向负例覆盖**创建前销毁 Surface → named VideoOutOpen fault → 下一 generation 正常四次 present**。这不等于已经验证运行中任意 GPU hang 都能取消；普通非 worker timeline wait 和驱动内部阻塞仍有边界。

验证 SurfaceView 改用 debug variant 专用普通 Activity，避免 launcher 权限/设置导致 Activity 中途重建。`surfaceDestroyed` latch 之后还需等待 framework 释放 Java Surface；复测发现的时序失败已保留，不以单次通过代替稳定关闭。

Citron 的 worker recording、shader 编译线程和 Switch guest 调度不能机械复制到 FEX/PS4 runtime。本次迁移的是可复用的事件身份、区间算法与锁边界原则，没有宣称迁移了完整 PROF/litep 生产者，也没有修改 guest 代码插桩。

## 并发数据能说明什么

新增 `pipeline_handoff start [100..10000]` / `status` / `stop` / `dump`。默认关闭，最多 100000 条事件；固定 steady_ns 时钟、PID/run UUID/generation/capture ID、OS TID、queue/task/timeline token。producer 不做文件 I/O；dump 在锁外序列化。跨录制 Scope 不能把 end 混入新 capture；容量、手动停止、会话结束均标为不完整。分析脚本拒绝不完整记录。

Hook 覆盖 PM4 enqueue/resume/complete、VideoOut prepare/flip、present enqueue/dequeue/completion、queue lock/driver call、帧资源等待和实际 timeline wait。显式相同 token 才连接异步事件；相邻时间戳不建立因果关系。嵌套区间按物理线程求并集，已标记 wait 从 observed work 中扣除。

SDK OFF 原始采集 `tmnt-off.json`：5 秒、3651 条事件、TID 16675 / 16688；2 个 scope 和2个 present completion 在边界未配对，明确保留为 unknown。

| 已配对阶段 | 次数 | 总时长 ms | p95 ms |
| --- | ---: | ---: | ---: |
| Vulkan.SubmitLock | 249 | 0.630 | 0.00547 |
| Vulkan.Submit | 249 | 14.686 | 0.10797 |
| Present.QueueLock | 83 | 0.220 | 0.00573 |
| Present.DriverCall | 83 | 29.345 | 0.73917 |
| VideoOut.Prepare | 83 | 624.987 | 9.84609 |
| PM4.Resume | 166 | 787.239 | 13.17010 |

本样本中两个被观测线程的非已知等待区间没有重叠，累计约1006 ms；剩余约3994 ms 是**这些 hook 未观测到工作**，不是 CPU 空闲或 FEX 无事可做。不能将其称为 CPU 利用率、完整 guest 帧、GPU 时长或关键路径。当前样本不支持“queue mutex 是主要耗时”的假设；继续诊断应关注文字使用的纹理内容/坐标/格式与 shader，而不是据此盲目增加提交线程。

SDK 全开样本的 driver submit 明显更长，但工作阶段、采集起点及诊断开销不同，不能报告优化倍数。`tmnt-third` 保留历史比较，不作为最终性能基准。

可重复命令（从主仓执行，输出到自选目录）：

```sh
python3 scripts/analysis/analyze_pipeline_handoff.py trace.json --output analysis.json
python3 scripts/analysis/export_pipeline_timeline.py trace.json \
  --output timeline.json --first-present 0 --count 6 --note '记录实际 SDK 开关和场景'
node "$SPATIAL_ARCHIFY_CLI" deliver timeline timeline.json timeline.html --quality showcase --json
node "$SPATIAL_ARCHIFY_CLI" visual-check timeline.html --json
```

这里的6个区间是连续7个 **host_present** 边界，不是6个 guest frame。保留全部与选择窗口相交的 paired scopes及原始端点，投影 sidecar 列出保留 token 和 unknown。Archify HTML 完成确定性校验、1440/1600/1920/2048 桌面尺寸的浏览器检查，以及大屏截图视觉检查。原始事件和投影源已归档；HTML 可按上述命令重建。

## GPU Reshape：实际集成与已知限制

使用 Cemu 的 Android source SDK adapter 方式，主仓 `vk_gpu_reshape.*` 按 MPL-2.0 保留来源及许可证；接在选中的 Turnip `vkGetDeviceProcAddr` 上，先于 VMA 创建资源。SDK 的 indirect dispatch、draw indirect count / indexed core+KHR 共5条 hook 补齐；默认关闭，保留普通 descriptor/push descriptor 路径。

当 SDK 开启，guest 和 host 图形辅助 pass 都采用实际 descriptor set，避免 SDK 无法观察 push descriptor；descriptor 更新必须是增量。原迁移按每个 write 调用 SDK 的整组替换函数，导致此前 binding 消失，已改成逐资源增量更新。无效 shader hash 过滤串现在明确拒绝插桩，不能静默转换成“全 shader”。状态报告 descriptor registration failure / fallback / overflow，不能把错误当成零发现。

独立 SDK 本机根目录为 `workspace/gpu_reshape/GPU-Reshape`，HEAD `0441175aa88b2f243495b56ab130b08b47280352`，ABI7 / host SQL schema13。**这个 checkout 含已有未提交 Android SDK 工作，HEAD 单独不代表本次 SDK。** 主仓记录全 Source 文件 SHA、diff SHA 和实际 DSO SHA。本轮只修改其中 indirect hooks 与 async collector 两个文件；增量补丁保存在 `cmake/gpu-reshape/`，没有提交/推送 SDK 内其他人的未提交工作。干净的该 HEAD 未必能应用这些补丁；异机需取得匹配 SDK 来源后核验清单，或关闭可选 SDK 构建。不能声称此依赖已经有可检出的新发布 pin。

Collector 两个确定缺陷也已修复：

1. 65536-word bounds 流不是5-word record 的整数倍，且整流编码超过64KiB wire上限，原来会 `instrumentation_encoding_failed`。现在每类最多读取64条完整记录，保留 produced count，截断明确 overflow。overflow 的前缀不是完整发现集合。
2. portable compiler 的 SGUID 是每 shader 本地编号，多 shader 同一 SGUID 会让 host join 错误倍增归属。collector 排除碰撞的 source mapping，原始 finding 仍保留、shader归属为 unknown；这没有实现全局唯一 SGUID。

全量插桩修复后仍有 table-not-bound/type mismatch；隐式 descriptor safe guard 可能改变画面，即使用户没有开启资源 safe guard。尚未证明这些告警源于游戏，不能据此改 guest shader 或禁用检查来“修好”游戏。buffer lifetime 尚无 source annotations；texel-buffer/WHOLE_SIZE 等未验证面没有完整性保证。

单 shader allowlist `26af93b82752ceb9`：实际替换1个 module、无编译失败/driver reject/fallback，最终 SQL 中1147个 collector结果 complete，零 bounds / descriptor findings，status 无 PRMT miss/drop、无 collector overflow。此实验只定位工具组合边界；未做已知 GPU OOB 正例，不能作为 bounds 检测器整体验收。

Android 属性为 `debug.shadps4.gpu_reshape` 以及 `_resource_bounds`、`_shader_replacement`、`_shader_analysis`、`_shader_hashes`；`gpu_reshape_status` 为采样状态，不在 UI 线程等待 SDK。属性在建立 renderer 时读取，需下一次会话/进程才能应用。先保存原值，使用后恢复；本轮已恢复测试前配置。未加入系统 Vulkan fallback。

## 验证和画面边界

- Portable 定向 C++：242 checks / 0 failures，包括 recorder21、config11、coordinator46；Python interval/projection12个测试。SDK indirect forwarding ABI38/0 是调用转发验证，不是 GPU instrumentation 正例。
- SDK OFF adapter 的独立 NDK 编译通过，避免可选 SDK 关闭配置无法编译。
- host canonical 构建 `HOST_LINK_PASS`，NDK29/API33/arm64/c++_shared/RelWithDebInfo，FEXCore Release；APK `playstoreDebug`。源码扫描过滤 Python `__pycache__`，避免将测试产生的字节码误判成编译中改源码。
- SDK OFF/ON 合成普通 APK、Surface failure/recovery 结果分阶段归档；StatusLayer APK 三项定向测试见 `status-apk-tests.txt`，沉浸式修复后真实 TMNT120秒见 `immersive-tmnt.txt`。没有运行全量回归。
- 全量 SDK：稳定 Activity 后120秒/180秒/120秒运行都主动 Stop；旧第一次 abandoned Surface 断言崩溃证据保留。SDK OFF和单 shader各120秒也通过；不同阶段 DSO身份分别保留，最终 APK 不冒认先前长跑证据。
- 明确指定物理显示屏 `4630946441858561667`（Android display0）的截图：关闭SDK可见 Logo 和破损文字对话框，单shader可见完整背景但相同文字破损。之前 scrcpy one-shot 返回3264×1836全黑图，未能证明实际屏幕是黑色；这批截图不再支持“整个游戏一直黑屏”的推断。

**本阶段末已取得真实RDC/replay，并修复本次文字图集破损；没有交互场景、十分钟、三个真实游戏长启动、Swan或完整PROF/Layer/PM4验收。** 后续其他画面问题仍需按具体draw/资源/同步证据定位，不能仅凭SDK type告警推断根因。本文件记录实施和排障结果，不替换原整体目标。
