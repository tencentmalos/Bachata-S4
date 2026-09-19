# Qualcomm 系统驱动屋顶长帧与提交／呈现隔离

2026-09-16。沿用本轮 [Int64 正向兼容实现](shader-int64-system-driver-2026-09-16.md) 的最终 APK，仅分析当前性能，不继续做画面错误 A/B，不修改线程、同步、shader 精度或 CPU 策略。原有 dirty work 全部保留；没有完整回归、commit/push。

## 结论

**shadPS4 已有独立 PresentThread，缺少的是 Citron 式的 Vulkan 录制／提交 worker 隔离。** 高通驱动上真正的 `vkQueueSubmit` 很短，但 `vkQueuePresentKHR` 在持共用队列锁期间阻塞，使 GpuComm 的后续 Flush 无法提交，再经 GNM 准入门控传到 guest。这不是“Vulkan 要求所有已提交任务等 present 才能继续”的语义。

GPU 本身的 guest 帧区间也约 **61.8 ms**；CPU 主帧约 **68.8 ms**。减少锁阻塞值得做，但本轮不能承诺仅迁移线程就达到 30 FPS。GPU 区间是 device elapsed，可能包含依赖、内存、屏障等等待，尚不能全部归为 shader ALU 工作。

## 运行身份与采集

- AYN Thor `9c2841a4`，Android13/API33/4KiB，Adreno740。实际 `source=system`，Qualcomm512.676.53，Vulkan1.3.128，build `69e13475cb`，`shaderInt64=0`，使用新 u32-pair 降级。
- APK SHA `8ecd6059bd2e9f3ba848a207106fd92e23b854c444c72e8b8fd35f16a1e80cb0`；host SHA `5cae668644561c607d1ae7b76160a16131da4828b03eb1b38a8e84ce373ac5cb`。源码清单重新核验全部匹配。
- 所有本报告实测：PID **10213** / generation **1** / UUID `26e9c15d50b45de8cdc5c875729cbb82`；屋顶 ATTACK，自动输入停止。画面仍有错误，不能作正确画面的驱动性能比较。
- 保留既有 `tmnt_frame_recompiled_v2`（SHA `882c3181b0b20eaa165fb337884e9335d88b699e75c37adcf502f3fb1a88eb02`）及其 frame/begin/submit/jobs/fmod 标定；auto-tag `disabled_at_startup`，未挂 debugger，未开 RenderDoc/GPU Reshape 抓帧。
- litep 25秒 coarse、20秒 detail；另取同会话5秒 handoff、约2秒调度 trace，以及30秒 `cpu-clock:u / 100Hz / fp` simpleperf。它们是相邻但不完全相同的窗口，不能拼成同一帧。所有短对照均不含自动输入。
- [归档与哈希](qualcomm-performance-20260916/artifacts.json)。大文件留在 `build/qualcomm-performance-20260916/`，归档保留汇总、脚本、身份、局部等待证据和截图。

## GPU：主要时间仍在 guest 区间

| 区间 | coarse 平均 | detail 平均 | 解释 |
|---|---:|---:|---|
| GPU.GuestFrame | 61.788 ms（360） | 61.819 ms（282） | 原生完整帧累计 counter，非单次 command buffer |
| GPU.HostPrepare | 0.643 ms | 0.644 ms | 当前含后处理 |
| GPU.PostProcess | 0.642 ms | 0.643 ms | 嵌套在 HostPrepare 内，不重复相加 |
| GPU.Present | 0.419 ms | 0.420 ms | GPU 命令区间，不是 host present 调用耗时 |
| GPU.OverlayRedraw | 0.432 ms | 0.435 ms | 单次旧帧重绘，频率较高 |

`GPU.DrawBatch` 均值约31ms不能当一帧：每帧通常有一个约62ms批次和一个几乎为空的批次。没有开启 FSR 实际工作；其约0.00016ms时间标记不代表正在进行有效 FSR。

15次独立 sysfs 采样按 busy/total 加权约 **89.2%**，采样频率均 **680 MHz**。这是整个设备的 GPU busy，不是本进程独占 busy，也不识别 ALU/带宽瓶颈。

detail 可配对数据中有223个完整长 guest 批次，每批73–77个 render-pass 区间：

- 长批次均值 **61.871 ms**，内部已标记 render pass 合计 **33.562 ms**。
- 余下约 **28.309 ms** 在 render-pass 区间之外；可能涉及 compute、传输、布局／内存屏障等，当前标签不足以进一步归属。
- 223个批次的最大单个空隙均位于 **零基序号22的 pass 之前**，平均 **5.344 ms**。这个序号仅适用于此场景／当前批次，**不是 shader 名或稳定跨帧语义 ID**。后续应关联 PM4/dispatch、pipeline hash、资源和尺寸，再交给 RenderDoc/GPU Reshape。

因此下一步不能只看 fragment shader，也不能把所有区间外时间称为“CPU 未及时提交”。上述空隙在已经提交的 GPU 批次内部。

## CPU：已成功提交与后续 Flush 要分开

coarse 有364个 frame/submit 标定。362个完整配对帧可以无重叠拆分：

| 主线程串行区间 | 平均 |
|---|---:|
| frame 开始 → render begin | 14.297 ms |
| render begin | 1.670 ms |
| 命令构建／任务协调 → submit | 37.545 ms |
| submit wrapper | 15.304 ms |
| 返回前尾部 | 0.035 ms |
| 合计 | **68.849 ms** |

这些是 elapsed 区间，不全是 on-CPU。jobs 平均每帧10.067ms，主要落在命令构建内；主线程 `sem_wait` 约7.98ms/帧。FMOD 标定0.186ms/帧，本屋顶窗口没有重现启动／loading阶段的长 FMOD 等待。

GNM gate 两次/帧，单次平均7.364ms；按帧合计约 **14.727ms**，不要误报成7ms/帧或加到 submit wrapper 之外。当前 `sceGnmSubmitDone` 约0.049ms，不能把它直接当 present wait。

GpuComm 722次真实提交平均 **0.0606ms**；相应 SubmitLock 平均 **19.001ms**。VideoOut.Prepare 平均33.675ms，其中 Prepare.Flush 33.478ms，说明这里也不是后处理 CPU 运算消耗33ms。

### 同时钟调度证据

2秒调度 trace `317494/317494`，无报告 buffer overrun。只使用完整落在 trace 内的 handoff 区间：

| 线程／区间 | 次数 | elapsed 总计 | on-CPU 总计 | 主要解除等待者 |
|---|---:|---:|---:|---|
| GpuComm / SubmitLock | 49 | 744.912ms | 0.635ms | Presenter，睡眠743.744ms |
| GpuComm / Submit | 50 | 3.150ms | 3.150ms | 无观测到的睡眠 |
| Presenter / Submit | 520 | 44.424ms | 42.878ms | 很少睡眠 |
| Presenter / Present.DriverCall | 271 | 1217.196ms | 26.002ms | `kgsl-events` TID867，睡眠1190.127ms |

GpuComm 锁等待743.519/744.912ms与 Presenter 持锁区间重叠。表内时间跨线程重叠，不相加。trace窗口有采集扰动，调用频率不能直接替代25秒coarse均值；中断／计时器上下文中的 waker 名称也不一律解释为业务 owner。

5秒 handoff 中一个具体样本（scheduler1 / tick13372）：

```text
Presenter Present.DriverCall  333660795165059 .. 333660858616986  63.452ms
GpuComm  SubmitLock          333660795233340 .. 333660858680215  63.447ms
GpuComm  vkQueueSubmit       333660858725580 .. 333660858786882   0.061ms
```

此样本来自完整handoff，不冒称它也落在前面2秒ftrace窗口。它说明 **tick13372在等待锁时尚未提交**；不是一个已经返回成功的vkQueueSubmit再次去等显示完成。对应 [原始数值](qualcomm-performance-20260916/representative-wait.json)。

源码顺序是：GpuComm处理PM4和VideoOut.Prepare → Prepare.Flush同步调用Scheduler::SubmitExecution → 拿队列锁并提交 → 软件队列排空和末尾Flush → `GpuIdle`软件IRQ释放GNM gate。另一条PresentThread调用同一VkQueue的present持同一锁；于是先前一帧的present会拖住后续提交和gate。`GpuIdle`在这里不表示调用了vkDeviceWaitIdle。

[Vulkan规范](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html)允许host端present调用有限时间阻塞，同时明确presentation命令本身不阻止随后队列命令处理。当前普通VkQueue仍要求host外部同步，**不能直接删锁并发调用同一个队列**。[vkQueueSubmit](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html)的成功提交与device完成由独立fence/semaphore区分；保留资源直到完成，不等于guest必须等显示。

## Citron 具体差异及后续方向

对照本机 Citron `3da08ee52d` 工作区源码；不将其未提交改动当远端可用 pin：

| 环节 | 当前 shadPS4 | 本机 Citron |
|---|---|---|
| 呈现线程 | `VideoOutDriver::PresentThread` 已存在 | `PresentManager::PresentThread`，由async_presentation选择 |
| Vulkan录制／提交 | GpuComm在调用栈同步执行 `Scheduler::Flush/SubmitExecution` | `Record/CommandChunk → DispatchWork → VulkanWorker` |
| 呈现入队 | PrepareFrame同步Flush结束后push request | scheduler.Record回调按worker顺序推present_queue |
| submit/present外部同步 | 共用submit_mutex、同队列 | 也保留scheduler.submit_mutex；不能假设两个独立硬件队列 |
| 反压传播 | 驱动／队列锁等待立即阻塞命令处理，拖延GNM gate | worker可先承受等待，producer可在资源/队列预算内继续；显式WaitWorker/资源复用仍会等待 |

源码入口：shadPS4 `driver.cpp:57,381,410`、`vk_presenter.cpp:811,1164`、`vk_scheduler.cpp:203`、`liverpool.cpp:189`；Citron `vk_scheduler.cpp:126,208,352`、`vk_present_manager.cpp:184,206,304`、`vk_swapchain.cpp:183`。

**应迁移命令批次的所有权与worker交接，而非只新增一个present线程。** 后续实现应让已接管的PM4/host命令数据拥有明确寿命，区分“已接收／已提交／GPU完成／flip完成”，在有界队列下推进guest，只在真实PS4同步、资源复用或容量耗尽时反压。不能简单提前发GpuIdle而让guest覆写尚未消费的DCB/CCB或资源，也不能把flush改成异步后继续沿用调用栈引用。

较小的系统驱动验证方向是让Presenter在拿队列锁之前等待本次present提交的`present_done` fence，再观察驱动present是否仍阻塞；这样可能把已知GPU就绪等待移出队列锁，仍须保留binary semaphore、frame/WSI寿命及Stop语义。它尚未实施，不能认定能消除所有WSI阻塞或取代worker架构。

GPU侧优先补齐pass间compute/transfer/barrier归属以及pipeline/guest marker/资源维度；特别是上述第22号pass之前的稳定5.34ms区间。CPU侧可以再沿命令构建37.5ms及高频HLE优化，当前不宜只追帧尾。

## 扰动、完整性与最终状态

- simpleperf 5820 samples / lost0。叶子占比：`ValidateRangeLocked`3.09%、`__aarch64_cas2_acq`5.41%、clock4.52%；这是所有采样线程的用户态份额，原子指令不能全部归给guest mutex，也不是主帧墙钟百分比。未解析JIT部分保留，不作全栈归因。
- 只做性能扰动短对照，不做画面A/B：overlay show/hide/show的guest flip为 **13.961/14.801/13.751 FPS**，提交频率156.3/44.3/133.6次/s。少重绘有收益，本次短窗口约6.8%，不足以解释30FPS缺口；最后恢复显示。
- coarse GPU query ON/OFF/ON为 **13.747/13.928/14.474 FPS**；波动大于开关差异，不能声称精确零开销。coarse/detail设备guest区间均约61.8ms。最后保留coarse，detail关闭。
- 两份PROF均完整拉取且捕获状态ready，chunks_skipped0，无gap marker；decoder仍报告边界未闭合scope并标记truncated，未伪装成全链路无损。仅完整scope参与统计。GPU counter数量高于可配对zone数量，切换期context/时间线覆盖不完整；GPU总帧用原生counter，pass分析仅用同context完整嵌套批次。
- 系统驱动没有calibrated timestamp扩展，跨CPU/GPU对齐为估计，报告误差约±1.25ms。GPU批次内部耗时有效；跨context亚毫秒先后不用于证明因果。
- 安装的Litep MCP拒绝该SDK revision的wire provenance；保留错误，使用仓库匹配SDK `368177d` 的decoder及已有gap-aware分析脚本。未改Foundation/SDK以绕过校验。
- 游戏保留同一PID/gen继续运行，ring ON、coarse GPU query ON、overlay显示；file capture停止、handoff结束、simpleperf结束、临时logcat结束、自动输入OFF、TracerPid0。tracefs恢复off/local/4KiB。没有更改驱动选择或系统频率。
