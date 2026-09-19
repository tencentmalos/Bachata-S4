# Android Vulkan 异步提交落地（2026-09-16）

本轮将已结束录制的 Vulkan 批次交给设备所属的 FIFO 提交线程，保留独立 PresentThread。目标是让 GpuComm 不再直接承受 `vkQueuePresentKHR` 持锁造成的等待；不是删除队列锁、提前宣布 GPU 完成，或再增加一个 present 线程。

## 实现与边界

- `Instance` 持有 `SubmissionWorker`、设备队列互斥锁。Android 默认启用；`debug.shadps4.async_submit=0` 为下一次创建 Session/Instance 生效的同步诊断模式，桌面继续同步提交。
- `Scheduler::Flush` 结束 command buffer 后转移 buffer handle 和按值复制的 `SubmitInfo`（包括 wait/signal/timeline 数组与 fence）。worker 在自己的调用栈重建 Vulkan 指针结构。没有捕获 caller 的栈数组、guest PM4 指针或可继续录制的 current buffer 引用。
- FIFO 容量 16，包含正在执行的任务；每个 Scheduler 最多提前 8 个未完成 GPU tick。容量反压与真实 timeline 资源复用仍允许等待，不承诺 guest 无限超前。
- 严格区分 **host 已接收 → 驱动已提交 → GPU timeline 完成 → VideoOut flip 完成**。`submit_accepted` 是接收证据，`vk_submit`/QueueSubmit counter 是实际调用完成证据，内存/command pool/query/descriptor 回收继续看真实 GPU tick。
- PresentThread 的本次 present batch 入队后，先在队列锁外 `WaitSubmitted`，确保 binary semaphore 的 signal 及其依赖已经提交，再调用 `vkQueuePresentKHR`。这个等待不回到 GpuComm。
- GPU query lease、捕获 generation 和 profiler context 仍属于录制 owner；worker 只发布小型 receipt。owner 在 receipt release/acquire 与真实 GPU tick 就绪后采集，`Finish` 同时确认 host receipt 与 GPU 完成。
- swapchain 重建、VM publication / unregister 的 drain、ImGui texture/font 销毁先排空 host 队列，再取队列锁/等待 device idle。VM drain 保留原先“PM4 已排空且已实际提交”的语义，不把它加入每帧 GNM gate。
- 提交异常 poison 新入队，销毁未执行的 captures 并唤醒 host 等待者；timeline wait 检查 worker health。Session health 能发现后台错误，Presenter 析构处理重复错误并尽力排空 device，避免析构抛错直接终止 APK。
- Android 旧帧/空帧/暂停 UI 重绘增加 `RedrawPacer`：慢 present 后的 vblank 追赶不再连续重绘旧帧，且用 steady clock 限制重绘频率。真实 guest flip、vblank 事件、prev buffer label 释放语义未提前。
- 增加 `PM4.WaitVideoOutLabel` 标记，区分下一轮缓冲区复用等待和提交锁等待。现有 `Vulkan.EnqueueSubmission`、`InflightBudget`、`WaitSubmitted` 与 `WorkerSubmit` 支持分阶段分析。

这次迁移的是 **sealed Vulkan submission batch**。Citron 还把 Record/CommandChunk 的录制调用放到 VulkanWorker；shadPS4 的 PM4 解析、资源查找和 Vulkan 命令录制仍在 GpuComm，并未宣称完成整套 Citron scheduler 重写。单一硬件队列的 driver/present 等待仍存在，只是 producer 可在有界预算内继续。同步模式是诊断入口，切换前结束当前 Session，下一轮才生效；本轮未再次做系统/Turnip 或画面 A/B。

`Close` 能唤醒容量等待并排空已接收任务，但不能强行中断一个卡在厂商驱动内部的同步 API。真实 device-loss/永久挂死、桌面完整运行和其他游戏仍不在本轮验证范围。

普通 VkQueue 的 host 外部同步仍是必要条件；binary wait 不能引用尚未提交的 signal：[vkQueueSubmit 规范](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html)。不把“GPU 正在异步执行”误解释成多个 host 线程可无锁调用同一队列。

## 针对性验证

- 新 `submission_worker_tests`：26/0；三 producer 共 900 批次 FIFO；延迟后端时入队与 GPU 完成不混淆；容量包含 active job；Close 唤醒并排空；失败取消 queued captures、唤醒 admission/Wait。
- macOS Homebrew LLVM23 的 ASan+UBSan、TSan 均 26/0。初次 Apple clang sanitizer 失败/卡在 main 之前，保留 runtime 初始化 sample 与原日志；不将其当测试通过，也不归因业务死锁。
- AYN Thor/API33/4KiB/系统 Adreno740：`android_submission_probe` **329/0**。实际 production Instance/Scheduler，持队列锁模拟慢 present 时两 owner 仍入队；修改调用方 SubmitInfo 后 GPU fill/copy、binary semaphore、fence 正确；96 批次与 query 开关/复用；三次设备对象重建，每次 retired93 / pending0 / errors0。
- 上述 probe 的第二轮另注入 backend exception，host wait 与未完成 timeline wait 均返回错误，第三轮重新创建并成功执行。**不声称注入了实际 GPU device loss**。
- 现有 GPU async 加重绘追赶场景 **28/0**；GPU timing **27/0**。host `.so` 与普通 playstoreDebug APK 构建通过。
- 初次 probe 未给 Lite ring 一个 source frame，导致计时未启用的断言失败；保留失败日志。修正 probe 启动条件后全过，没有删除断言或修改生产 ring 的启用规则。

## 中间版本发现

中间 APK `67493b5b` / host `feef0414`，PID18977/gen1/UUID36661bf6621099a2ffe83ba66a9334bf，实际屋顶 MOVE→ATTACK，warmup GAMEPLAY_REVIEWED，普通 UI Stop 到 Stopped/user_stop。

25秒抓取完整拉回（44,776,280 bytes），仅完整 scope 参与统计。GpuComm 入队均值0.019ms，Prepare1.945ms，直接 SubmitLock 已只在 VkSubmit worker；GPU GuestFrame counter 仍62.03ms。可是主帧70.97ms/p95 150.83ms，GNM gate每帧30.66ms；不能据此宣称整体提速。该轮还发现旧帧重绘1548/25s，加真实 present351/25s约76次/s，促成本轮 RedrawPacer 修复。

捕获切换边界有未闭合 scope/unknown GPU context，SDK decoder 标记 truncated；无 gap marker、chunks skipped 为0，并非字节文件被截断的证明。GPU总帧采用原生 counter，不将不完整 GPU zone 数量当覆盖率。最终结果另记如下。

## 最终普通 APK

最终 APK `47e130b004f88d8e873c5cc56a9649e5420b175db2e5d599e028dac908937532`，host `af22780410d2f426d8a6682e2f81a4eb6fbd87b1063aa8664ec67adbe592efaa`，JNI `663ec87a768c5b0d8d21372754e9fca24393d8677d47d13dffe89148b4bb9523`。源基线4da582b7加保留的 dirty work；[本轮增量](async-submission-20260916/implementation.diff)、[源哈希](async-submission-20260916/source-final.json)、[二进制身份](async-submission-20260916/artifacts-final.json) 精确区分，不能只用 HEAD 代表构建源。

PID23969/gen1/UUIDc19eb8831af26e4a984b9b46a1107837，系统 Qualcomm，真实屋顶 MOVE→ATTACK，warmup GAMEPLAY_REVIEWED。最终25秒PROF 50,190,532 bytes、SHA7b2e4585496e3c1481926685d23d53ae9a823a077da01b2c01b6fc5642c16ae3；同匹配SDK decoder，618 chunks / skipped0 / no gap。边界scope与GPUcontext仍不完整，truncated标志保留。

| 指标 | 同系统驱动旧同步基线 | 最终异步＋重绘限频 |
|---|---:|---:|
| guest主帧均值 / p95 | 68.813 / 91.628ms | 63.831 / 71.825ms，392帧 |
| GpuComm VideoOut.Prepare 均值 | 33.675ms | 1.167ms |
| GpuComm 入队均值 | 原来同步调用driver | 0.0148ms，784次 |
| GPU GuestFrame counter均值 | 61.788ms | 61.518ms，387个完整样本 |
| GNM gate每主帧合计 | 约14.73ms | 约33.28ms |

独立43.927秒counter窗口guest flip15.685/s、host present15.662/s；GPU timing无errors/drops。最终25秒GPU.Present389个，GPU.OverlayRedraw没有新样本；5秒handoff也只有真实present，不再是中间版本1548次旧帧重绘/25秒。`overlay_redraw`诊断counter实际记录StatusLayer绘制，包含真实新帧，**不能拿它当旧帧数**。

最终5秒handoff：draw scheduler的实际submit均值0.094ms；其SubmitLock仍约22.16ms，但owner已经是VkSubmit线程24051，GpuComm24049无直接队列锁等待。Presenter driver调用仍62.27ms。**不能将所有GPU/显示等待声称为消失**：新增PM4.WaitVideoOutLabel均值42.865ms，GNM gate仍会受尚未满足的buffer label阻塞；CPU可更早结束本帧准备，所以其等待份额反而增加。各线程时间重叠，不能相加。真实缓冲区复用/flip条件不能仅因host接受批次而提前满足。

这些是同设备同驱动、相同屋顶阶段的先后观察，不是锁频配对性能实验；场景位置、温度及捕获阶段不同，不承诺固定百分比提速。CPU/GPU时钟没有校准扩展，最终捕获估计对齐误差上界达到±69.22ms；**GPU自身区间时长可用，不能用跨CPU/GPU lane精确先后来证明等待因果**。host等待归属来自CPU scope/handoff与源码，非这个估计对齐。

后续性能工作应看GPU主负载及前文未归属的pass间compute/transfer/barrier，再看真实VideoOut label生命周期是否存在额外过度串行；本轮没有为了减少等待而跳过label/fence/guest同步。

## 重启限制与最终现场

最终gen1普通UI Stop在0.771秒到Stopped/user_stop，未强杀。但同PID gen2启动约38秒后，Guest-1出现SIGSEGV/SEGV_ACCERR（address0xa65601a70，PC在FEXMemJIT）。因此**不通过稳定多轮真游戏重启验收**，不把三个Vulkan probe generation冒称三个真游戏重启。

同一最终APK切`async_submit=0`后，新PID27609/gen1与gen2均正常启动，Stop分别0.426/0.458秒；该对照首轮未进入屋顶，运行时序不同，不能据此证明异常由worker引起或证明它与worker无关。原始crash、host尾日志与中间/最终性能证据保留；这是未完成归因的一次真实失败。

精确源文件与 APK/DSO 身份、失败记录和统计汇总归档于 `async-submission-20260916/`；大体积原始 PROF 留在 `build/async-submission-20260916/` 并记录 SHA。

本轮无 FEX/Foundation 子仓修改、无全量回归、无画面错误 A/B、无 commit/push。系统驱动原有画面块状错误仍待用户指定的 RenderDoc/GPU Reshape 线处理。
