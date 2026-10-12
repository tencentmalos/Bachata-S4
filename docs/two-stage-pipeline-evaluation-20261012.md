# 两阶段绘制流水线：bbport 与 GCN v2 的对比和取舍

整理日期：2026-10-12。问题：bbport 把 GPU 命令线程拆成“解码”与“绘制录制”两段，它和本仓 GCN v2 的目标架构切分点不同，本仓是否值得进一步规划、实施类似的拆分。本文只做源码与既有测量的对照，没有新的设备测量或代码改动。

来源：

- bbport：`references/bloodborne_pc`（0.51），主要是 `docs/parallel_gpu.md`、`gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h`、`vk_constant_ring.h`、`vk_rasterizer.cpp`、`amdgpu/liverpool.cpp`、`docs/EMULATION_REMOVAL_PLAN.ru.md`、`docs/CHANGES_2026-10-0{2,3}.md`。
- 本仓：`src/video_core/amdgpu/liverpool.cpp`、`renderer_vulkan/vk_rasterizer.cpp`、`vk_command_recorder.*`、`vk_scheduler.cpp`，[GCN v2 spec](specs/gcn-translation-layer-v2-20261003.md)，以及 `docs/validation/android-native-host/` 下的性能报告。

## 结论

**现在不值得照搬 bbport 的拆分，也不建议马上启动 GCN v2 阶段 B。** 值得做的是三件小事：查一个同类的 fence 时序风险、先做单线程上的减负（bbport 自己收益同样大的那几项）、定下“什么时候 GpuComm 成为瓶颈就重新评估拆分”的判据。理由如下。

1. **本仓目标设备上 GpuComm 多数时候不在关键路径上。**

   | 设备与场景 | 帧长 / 帧率 | GpuComm | 实际瓶颈 |
   |---|---|---|---|
   | Thor 血源（10-06/07，两次测量：大教堂附近与录制线程提交 A/B） | 约 28 FPS / 帧 39.4 ms | 26.5–36.7 ms | GPU 86–99% 忙，GPU 受限 |
   | Thor 血源战斗（10-01） | 14 FPS，71 ms | CPU 34 ms，睡眠 37 ms | guest 工作线程 → Guest-1 → 提交线程的供给链 |
   | Pocket DS 血源（10-02/03） | 26–30 FPS（30 封顶） | 约 22 ms，忙约 55% | Guest-1 |
   | Swan 血源诊所（出厂频率，09-24） | 约 23 FPS | 未单独测（同场景锁频时录制线程使其 44 → 33 ms） | GPU 97–99% 忙 |
   | Swan 血源 30 帧封顶场景（09-28） | 30 FPS | CPU 13.5–14.7 ms | GPU 84–93% 忙，Guest-1 24–27 ms |
   | Swan 血源诊所（锁频，09-23） | 14–17 FPS | 49–65 ms，CPU 占 93% | GpuComm，SRT 修复后与 Guest-1 并列 |
   | Odin3 KH3 觉醒之所（10-11） | 29 FPS，34 ms | 23.6 ms（其中纹理重传 10.3 ms） | 正在转向 GPU 与平台频率 |
   | 桌面 RX 7600M XT 血源（10-02） | 60 FPS 封顶 | 11.8–12.7 ms | 已封顶；封顶前是 GPU |

   GpuComm 真正成为瓶颈的只有锁频的 Swan 诊所和 09-28 的 Thor，这两处之后都被其他改动缓解。bbport 的收益来自另一种场景：RX 7800 XT 上不封顶跑到 80–110 FPS，GPU 命令线程是唯一瓶颈。

2. **拆分不减少工作量，只缩短串行链；移动端多一个忙线程未必划算。** bbport 只在进程可用 ≥8 个硬件线程时默认开启（`DrawPipeWanted`）。4 核 8 线程下仍有 +18%，但那是桌面 x86 核。本仓的移动设备大核有限：Thor 快核需求约 3.4 个、只有 5 个快核；Odin3 的大核簇被 core_ctl 挂起；Pocket DS 处于温控。B 段在 bbport 里 85–99% 忙，在这些设备上它会和 Guest-1、guest 工作线程、VkRecord 争核，还可能因发热降频。

3. **两种切分本身的工作分配不均衡。** 本仓 GpuComm 的大头在资源侧。桌面：`BindResources` 38%、`ObtainBuffer` 23.8%、`FindImage`/`ImageInfo`/`FindView` 约 20%、`GetGraphicsPipeline` 14.2%。Pocket DS：`ObtainBuffer` 26%、`GetGraphicsPipeline` 20%、CopyShader 13%、`BindTextures` 11%。
   - **bbport 的切分**：A 段只做 PM4 解码与管线选择，B 段做资源绑定和录制。B 很快成为瓶颈（85–99% 忙、A 等待 25–40%），作者的下一步是把绑定解析挪回 A（`EMULATION_REMOVAL_PLAN.ru.md` 方案 b），到 0.51 仍未实施。
   - **GCN v2 的切分**：前端解析资源，录制线程只做 Vulkan，相当于 bbport 打算走到的位置。但在前端减负之前，这种切分下前端仍是长杆，拆出去的 Vulkan 部分本来就已经在 VkRecord 上（Thor 上 VkRecord 每帧 10–16 ms，75% 在 Turnip 的描述符更新）。

   所以拆分要有收益，前提是先把前端每 draw 的工作降下来，这正是 GCN v2 阶段 C/D 的内容。

4. **bbport 单线程减负的收益和拆分相当。** 常量环 +11%、渲染状态记忆 +9%、纹理集记忆 +5.6%、待写重叠检查加速约 +4%、间接 draw 流水 +1.4%，合计与拆分的 +19–32% 同一量级，而且不引入线程间的所有权问题。本仓对应的是 GCN v2 阶段 C/D：permutation 边读边比、T# 绑定缓存、流式页都已部分完成，`GetGraphicsPipeline` 降到 3% 的目标还没达到。

5. **拆分的正确性成本高。** bbport 为此加了十几条排空规则、一组逐项开关和寄存器/SRT 校验模式（`BB_PIPE_VERIFY`），仍出过两次由拆分引起的游戏数据损坏：GPU idle 早于延迟 fence 导致堆损坏；B 段在 A 已覆盖常量 RAM 后才读顶点 V# 表，即 DoF 画面错误 #44。本仓的情况更复杂，拆分要重审下面这些所有权点：
   - Android 写跟踪缺页（纹理缓存 mutex 同时被缺页处理和 GpuComm 使用）；
   - `SendCommand` 回读；
   - copy-shader HLE；
   - 异步管线编译的跳过模式与缺失内容跟踪；
   - GpuComm 上的 pass 上提。

## 现在可以做的事

1. **查 GPU idle 与延迟 fence 的时序（桌面）。** 完成 fence 打开时（桌面默认），`Liverpool::Process` 在一轮解码结束后直接发 `GpuIdle`（`liverpool.cpp:541`），没有等 `FenceLabels` 里还没写的 label。只有 flip 会等（`WaitFences`），完整等待的 `DrainFences` 只在切换模式时用。这与 bbport 的堆损坏是同一时序：游戏在 GPU idle 后回收对象，迟到的 label 写进已释放内存。本仓 99.7% 以上的 fence 由 CP 提前执行（`SettleWaits`），所以风险窗口小，也没有观察到对应崩溃；Android 默认用解析模式（立即写），不受影响。
   - 建议先加计数：`GpuIdle` 时 `Outstanding()` 非零的次数与数量。
   - 若常见，再对比“GPU idle 前 `DrainFences`”的崩溃率与帧率（等待会减少 CPU/GPU 重叠）。
2. **单线程减负，按测量排序。**
   - 对照 bbport 的渲染状态记忆和纹理集记忆，看本仓 `PrepareRenderState`、`BindTextures` 在 Thor 回放里的命中潜力；本仓的 T# 绑定缓存粒度是单个 T#，它们是整组。
   - KH3 每帧约 10 张整图重传（问题 #12）是 Odin3 上 GpuComm 最大的单项，优先于任何拆分。
   - 进展（10-12）：
     - **#12**：已修复，见 [kh3-pool-undefined](validation/android-native-host/kh3-pool-undefined-20261012.md)。“觉醒之所”每帧上传约 12 → 0，帧率约 +15%，GpuComm 约 −56%；`RefreshImage` 在 GpuComm 采样中由约 56% 降到 1.3%。GpuComm 新的最大项是等 MemoryManager 锁的 `sched_yield`（约 20%）。
     - **渲染状态记忆、纹理集记忆**：同一场景的 simpleperf 中，`PrepareRenderState` 低于报告阈值（< 2.5%）；`BindTextures` 去掉其中的刷新约 2–3%；`GetProgram` 与 `RefreshGraphicsKey` 各约 7%（这一组数据是开着 `upload_diag` 采的）。在 KH3 上，这两种记忆的收益上限明显小于剩下的上传。Thor 回放上的命中潜力尚未测。
3. **定下重新评估拆分的判据。** 当某个目标场景同时满足下面几条，再按 GCN v2 阶段 B 规划拆分：
   - GpuComm 在 CPU 上的时间占帧长 80% 以上；
   - GPU 忙低于约 85%；
   - Guest-1 不是更长的杆；
   - 设备在该场景下有空闲大核。

   桌面配 60 FPS 补丁、跑在 30 FPS 以上档位，或 Android 上 GPU 与 Guest-1 问题解决之后，都可能出现这种情况。已写入 [GCN v2 spec](specs/gcn-translation-layer-v2-20261003.md) 的“阶段 B 的启动条件”。

## 若将来实施，可直接借鉴 bbport 的部分

- **包格式**：寄存器只传上一包之后写过的 32 字块；按 stage 传 user data 与扁平 user data 快照；包数据存活到后段经过为止（`RetireSubmission`），提交结束不必排空。
- **有序任务**：EOP/EOS、WRITE_DATA、DMA_DATA、flip 中断作为有序任务交给后段；`WAIT_REG_MEM` 遇到已交给后段的 fence 视为满足；GPU idle 前等延迟信号。
- **排空规则清单**：bbport `gpu/shadps4/video_core/amdgpu/liverpool.cpp:342-381` 的 `PipelinedOpcode`，以及光栅器各入口。
- **校验与诊断**：每 N 包附完整寄存器文件、在后段比对（`BB_PIPE_VERIFY`）；每个特性一个开关位，便于同进程 A/B。本仓已有的确定性回放可以承担同样的校验。
- **线程门槛**：按可用硬件线程数决定是否启用；线程不足时完全退回单线程路径。

另：`swan-bloodborne-recorder-hoist-20260924.md` §2.1 记载的录制线程默认关闭、持有上限 8 块，与现在的代码不符：录制线程已默认开启（`emulator_settings.h:507`），持有上限 32（`vk_command_recorder.h`）。
