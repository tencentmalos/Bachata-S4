# XR 重复帧不拷贝与减少拷贝的研究（2026-10-07）

对应 [AstroQuest 参考引入规划](../../specs/astroquest-reference-import-20261006.md) WP-C 的 C1。本轮没有 Swan，**只做了实现和编译**（Android host 库与桌面 exe 均编译链接通过），没有上机验证；下文的带宽数字是按图像尺寸算出的估计，不是测量值。

## 1. 原来的拷贝链

XR 模式下（`src/video_core/renderer_vulkan/openxr/runtime.cpp`、`vk_presenter.cpp`），一帧游戏画面经过：

| 步骤 | 线程 | 内容 | 频率 |
|---|---|---|---|
| 合成 | GPU 命令线程（`PrepareVrFrame`） | 采样 guest 双眼图（纹理缓存 view，无拷贝），后处理/超分写入 presenter 的 `frame.image`（SBS，`FrameExtent`） | 每个游戏帧，必需 |
| 拷贝 1（Publish） | Present 线程 | `frame.image` → mailbox；提交等 guest timeline，**CPU 等 fence** | 每个游戏帧 |
| 拷贝 2 | XR 线程 | acquire swapchain → mailbox 整图拷入 → **CPU 等 fence** → release | **每个 shouldRender 的 runtime 帧**，重复帧也拷 |
| 镜像 | Present 线程 | 把 `frame.image` 画到 Android 窗口并 present | Surface 可用时每个游戏帧 |

Swan 推荐档每眼 2592×2400，SBS 5184×2400×4 B ≈ 49.8 MB；不压缩时一次整图拷贝约读写各 50 MB。runtime 90 Hz、游戏 30 FPS 时，拷贝 2 每秒 90 次（约 9.0 GB/s 读写），其中 60 次是内容没变的重复帧；每次还在 XR 线程上 CPU 等 GPU 完成。

## 2. 本轮实现（未上机）

1. **重复帧不拷贝**：`Publish` 给 mailbox 计数（`mailbox_serial`），XR 线程记下最后拷入 swapchain 并 release 的那一帧（`shown_serial`）。两者相同时不 acquire、不拷贝，直接再次提交引用该 swapchain 的 layer。OpenXR 合成时使用最后一次 release 的图像，layer 用的渲染姿态/FOV 就是那张图的 mailbox 快照（序号相同即内容相同）。AstroQuest 的做法相同（`openxr_host.cpp` 的 `TakeFrame`/`CopyFrame` 只在有新帧时拷贝）。会话重新 begin 时清零 `shown_serial`，强制先拷一次。
2. **XR 线程不再 CPU 等拷贝完成**：拷贝提交到会话绑定的同一个队列，release 前只需已提交，不需已完成（hello_xr 的 Vulkan 插件也是提交后直接 release，只在复用命令缓冲前等 fence）。`FinishCopy(..., block=false)` 只记 `pending`，下次复用该命令缓冲前 `Settle` 再等（放在拿 mailbox 锁之前，避免 Publish 等它）。生产端拷贝写 mailbox 时的屏障（`ALL_COMMANDS`）在同一队列上排在消费端读取之后，读后写顺序不变。
3. **2D 影院按原尺寸放入 mailbox**：原来 2D 帧（例如 2592×1458）被拉伸 blit 到整张 5184×2400 的 mailbox，合成器显示 quad 时再采样一次。现在放进左上角原尺寸区域（格式相同用拷贝，不同用 1:1 blit；比 mailbox 大时才缩小），quad/projection 的 `subImage` 与消费端拷贝都只用这块区域：影院模式两次写入均约少 3.3 倍像素，也少了一次重采样。PSVR 帧本来就等于 `FrameExtent`，不受影响。
4. **presenter 帧图保留 UBWC**：`RecreateFrame` 的 `frame.image` 带 `MUTABLE_FORMAT` 却没有格式列表（来自上游 #1559，现在只建同格式 view），与 B1 同一原因会在 Turnip（A740 无 `ubwc_all_formats_compatible`）上关掉 UBWC，而 XR 下它是全尺寸 SBS 图、每帧被整图读取。现附 `{表面格式, 其 sRGB/UNORM 兄弟}` 格式列表。2D 模式同样受益。
5. **开关与计数**：DebugBus `xr_frame_copy status | new | every`，属性 `debug.shadps4.xr_copy_every_frame=1` 启动即用旧行为（每个 runtime 帧都拷），用于 A/B 和 Pico 合成器不接受时回退。`status` 输出累计 `copied` / `repeated`。LiteP scope：`XR.Mailbox.ConsumeSubmit`（提交）、`XR.Mailbox.SettleWait`（复用前等待，正常应接近 0）。
6. XR 截图（`capture_source xr`）在消费端命令缓冲里从 mailbox 拷出，开着截图的帧仍走拷贝路径，所以**用 XR 截图看不到重复帧路径的画面**，验收重复帧须用系统合成器截图/录像（`capture_headset_view` 等）。

估计效果（90 Hz / 30 FPS、PSVR）：消费端拷贝 90 → 30 次/s（约 9.0 → 3.0 GB/s 读写），XR 线程每个重复帧不再有 acquire/拷贝/CPU 等待；UBWC 生效时实际带宽更低。影院模式两次拷贝都按 2592×1458 计。

## 3. 进一步减少拷贝的研究

按收益/风险排序，均未实施：

### 3.1 Publish 不在 CPU 上等（Present 线程）

`Publish` 先 `DrainSubmissions`，提交的拷贝要等本帧 guest 渲染的 timeline，再 CPU 等 fence：**Present 线程被阻塞到整个 guest 帧在 GPU 上做完**。等待的理由是 presenter 在 `Publish` 返回后会复用 `frame.image`，而复用前只等 `present_done`：镜像 present 的提交（vkQueueSubmit 的 fence 覆盖同队列此前所有提交）能覆盖这次拷贝，但 `CanPresent()` 为假、`StopRequested`、acquire 失败等路径不提交就把帧放回空闲队列，下一次写入只在同队列上排序，后续写入的首个屏障若不是 `ALL_COMMANDS` 起点就有读后写冲突。

做法：runtime 持有一个 timeline semaphore，生产端提交 signal 一个值并记在 `Frame` 上（例如 `xr_read_value`）；`GetRenderFrame`/`RecreateFrame` 在复用前等这个值（或下一次写该帧的提交以 GPU 等待方式 `AddWait`），所有放回空闲队列的路径都被覆盖。收益：Present 线程不再与 GPU 串行，空闲帧池不被它卡住。需要 Swan/Thor 上 `XR.Mailbox.PublishSubmitWait` 与 `Present.FreeFrameWait` 的前后对比。

### 3.2 去掉 mailbox：直接合成进 XR swapchain（零拷贝）

`PrepareVrFrame` 的最后一个 pass（后处理或超分输出）直接以 acquire 到的 swapchain 图像为目标，GPU 命令线程提交后 release，并在 mailbox 锁下发布该帧的姿态/FOV；XR 线程只提交 layer。比 AstroQuest（渲染进自己的槽位，再拷一次到 swapchain）还少一次拷贝，并省掉 mailbox 的约 50 MB 显存。要解决：

- **格式/伽马**：后处理输出的是已编码伽马的值，写进 UNORM 图；swapchain 是 sRGB。要么建带 `XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT` 的 swapchain，通过 `XR_KHR_vulkan_swapchain_format_list` 给出 `{sRGB, UNORM}` 并用 UNORM view 写入（需确认 Pico runtime 支持）；要么让后处理在目标为 sRGB view 时输出线性值、由硬件编码。
- **镜像与截图**：Android 镜像、`capture_recorder`、截图都读 `frame.image`；swapchain 图像 release 后归 runtime。镜像不可见且没有截图时才走零拷贝，否则保留 `frame.image`（此时一次拷贝）。
- **线程**：acquire/wait/release 在 GPU 命令线程上调用（OpenXR 只要求同一 swapchain 的这三个调用外部同步，xrEndFrame 不在其列），仍需持队列锁；`xrWaitSwapchainImage` 用短超时，拿不到就退回 mailbox 路径，不能卡住 GPU 命令线程。
- 重复帧照旧不拷贝（本轮已实现）。

### 3.3 XR 下的 Android 镜像

`Present` 在 `Publish` 之后继续把帧画到 Android 窗口并 `vkQueuePresent`（Thor 慢帧分析里 `vkQueuePresent` 阻塞是三类慢帧之一）。头显里通常看不到这个窗口；若 Swan 上 XR Activity 的 Surface 不可见，可在 XR 模式下不画镜像（保持空闲帧池流动），只在截图/录像或用户需要时画。先用 `Present.*` scope 在 Swan 上量它占多少。

### 3.4 其他

- 影院模式 swapchain 仍按 SBS 尺寸分配；拷贝已只拷内容区域，剩下的只是显存，可按内容尺寸另建较小的 swapchain。
- mailbox 不带 `MUTABLE_FORMAT`，本来就可用 UBWC；swapchain 图像由 runtime 分配，不在我们控制内。

## 4. presenter 输出目标：XR 时不再走 Android 窗口（第二步，未上机）

问题在 guest → host 的 presenter 没有输出目标的抽象：XR 模式下 `Present()` 先 `Publish` 给 OpenXR，再完整走一遍 Android 窗口（取 swapchain 图像、建 ImGui 帧、画游戏图、持队列锁 `vkQueuePresent`）；空闲 vblank 还 `DrawLastFrame` 重画；`Flip` 用“Android 窗口是否 present 成功”判定游戏帧是否已呈现。头显里看不到这个窗口，这些都是白做的，`vkQueuePresent` 还会挡住同队列的提交。

**Azahar 的做法**（本机 `D:/workspace/azahar`）：没有独立的 presenter 接口，`RendererVulkan::SwapBuffers`（`renderer_vulkan.cpp:3667–3739`）按全局“OpenXR 显示模式”二选一：XR 时 `RenderToOpenXrGameFrame` 把呈现计划直接画进 XR swapchain 图像（无拷贝），否则 `RenderToWindow`。XR 时 Android Surface 的 swapchain 仍会创建，但从不 present，也没有镜像。渲染线程从不在 CPU 上等 XR：提交后把 tick 交给 XR 协调线程，协调线程轮询 GPU 完成后才 `xrReleaseSwapchainImage`；重复帧直接再提交上次的 swapchain。

**本仓实现**（`vk_presenter.{h,cpp}`、`videoout/driver.cpp`、`openxr/runtime.{h,cpp}`、`capture_recorder.{h,cpp}`）：

- `Presenter::Output { Window, Xr }`：有 XR runtime 时为 `Xr`，除非开了镜像（DebugBus `xr_mirror on`，或 `debug.shadps4.xr_mirror=1` 启动即开）或正在录 canvas 画面（`CanvasCaptureWanted()`，其编码拷贝在窗口路径里）。
- `PresentToXr`：只 `Publish` 给 runtime，并刷新 XR 状态层快照；不取窗口图像、不建 ImGui 帧、不拿队列锁做 present。返回值是 runtime 是否收下了这帧，`Flip` 据此计已呈现帧，不再依赖 Android 窗口。重用帧直接返回（runtime 自己重复最后一张）。
- **Publish 不再在 CPU 上等**：生产端拷贝改为 3 个命令缓冲轮换，提交后再追加一次空提交来 signal 该帧的 `present_done`（空提交的 fence 在队列中此前全部工作完成后才 signal）；`GetRenderFrame` 复用该帧前本来就等 `present_done`，所以各条放回空闲队列的路径都被覆盖（这正是 3.1 节的风险点）。镜像打开时窗口路径会复用 `present_done`，Publish 仍按原来那样等待。
- `Presenter::WantsIdleRedraw()`：XR 输出时为假，present 线程不再 `DrawLastFrame` / `DrawBlankFrame`。
- 影响：XR 下游戏里的交互式 ImGui 对话框（存档、输入法等）原本画在头显里看不到的 Android 窗口上，现在不再画；需要用设备屏幕操作时打开 `xr_mirror`。XR 内显示这些对话框要单独做 XR 面板。

Android host 与桌面均编译链接通过；Swan 未验证。

## 5. FSR/SGSR 与后处理放在哪里（2D 已实施并在 Thor 上验证，见 5.1；XR 仍在 GpuComm）

现状：`PrepareVrFrame` / `PrepareFrame` 在 guest 的 GPU 命令线程（PM4 owner）上录制注视点（FDM）上传、`SpatialUpscalePass`（FSR1/SGSR1，每眼）、`PostProcessingPass`，再 `Flush`。两点会拖慢 guest：

1. **CPU**：这些 pass 用 `Scheduler::RawCommandBuffer()`，开着录制线程时它调用 `CommandRecorder::CheckOutRaw()` → `Sync()`，让 guest 命令线程等录制线程把此前的命令全部回放完（`vk_command_recorder.h:205`）。每个呈现帧一次同步，加上 pass 本身的录制时间，都算在 guest 命令线程上。
2. **GPU**：pass 与 guest 工作在同一个命令缓冲、同一队列里串行；全分辨率 EASU+RCAS 每帧数毫秒，直接排在下一帧 guest 工作之前。

合适的位置是“guest 只交出源，主机后处理在呈现侧做”：

- **guest 命令线程只做必需的**：解析眼图/VideoOut 图的纹理缓存 view、记下参数和本帧 guest 工作的 timeline tick、钉住这些图像，然后像普通 draw 一样结束本帧（不 `RawCommandBuffer`、不同步录制线程）。
- **呈现侧（present 线程，`present_scheduler` 自己的命令缓冲）**：等 guest tick 的 GPU 等待，录 FDM 上传、FSR/SGSR、后处理，输出到帧图（以后可直接是 XR swapchain 图像，见 3.2）。PSVR 输入的 `complete` 改在这份提交退休时调用；2D flip 缓冲本来就在 `Flip` 之后才释放给 guest。
- **要解决的**：(a) 纹理缓存图像的寿命：图像目前按 draw 调度器的 tick 退休，跨调度器使用前需要一个“呈现中”引用，退休要等呈现提交完成；(b) `PostProcessingPass` 依赖调度器的描述符分配（`BindHostDescriptors`）、`DeferOperation`，换到 present 调度器即可，但要保证这两个 pass 只在一个线程上使用；(c) GPU 侧仍是同一队列，若要让 guest 优先，可再评估 Turnip 的队列全局优先级（KGSL 上下文优先级/抢占，Swan 上抢占与 GPU hang 有过关联，需谨慎）。
- 次选（改动小）：把 pass 的录制包成录制线程的 `Custom` 延迟命令，免掉 `Sync()`；但 pass 内部会调用调度器（描述符、tick、延迟操作），需要先把这些挪到录制前算好，收益只有第 1 点。

### 5.1 2D 窗口：主机 pass 移到 present 线程（已实施，Thor 实测）

- **GpuComm（`Presenter::CaptureFrame`）**：找图、`UpdateImage`、把图像转到 `ShaderReadOnly`（录在录制线程上，不借 raw 命令缓冲），用 `Scheduler::HoldRetirement(tick)` 钉住本帧 guest 提交之后排队的延迟操作，`Flush`，把 `PresentSource`（view、尺寸、sRGB、guest timeline 信号量与 tick）交给 VideoOut。需要回读时才借 raw 命令缓冲。
- **present 线程（`Presenter::Compose`）**：取帧图，在 `present_scheduler` 自己的命令缓冲里录 FSR/SGSR（`SpatialUpscalePass`）与 `PostProcessingPass`、内嵌截图，等 guest tick 后提交；`SetRetirementReader` 让被钉的延迟操作在 present 提交退休后才执行。
- 两个线程都会用 `pp_pass`/`spatial_pass`（XR 的 `PrepareVrFrame` 仍在 GpuComm 上），用 `host_pass_mutex` 串行。
- 开关：Android `debug.shadps4.present_compose=0`、桌面 `SHADPS4_PRESENT_COMPOSE=0` 回到旧路径（默认开），日志 `VideoOut host passes on the present|GPU command thread`。

Thor（AYN，Turnip 本地 `01a3548f`，血源中央亚楠，30 FPS 上限，每轮 6 s LiteP）。on1 为安装后第一次会话，on2/off1 为之后的会话：

| 轮次 | GpuComm 帧长 | draw/帧 | Draw 合计 | µs/draw | VideoOut.Prepare | 其中 RecorderSync | GpuComm 上 FSR+PP | present 线程 Compose |
|---|---|---|---|---|---|---|---|---|
| on1（开） | 49.66 ms | 1798 | 30.64 ms | 17.0 | 1.76 ms | 1.69 ms | — | 未统计 |
| off1（关） | 39.52 ms | 1799 | 27.03 ms | 15.0 | 1.59 ms | 1.40 ms | 0.057 + 0.023 ms | — |
| on2（开） | 39.55 ms | 1885 | 27.86 ms | 14.8 | 1.08 ms（13 帧窗口） | 1.16 ms | — | 0.21 ms（FSR 0.068、PP 0.029） |

- on1 每个 draw 都慢约 13%，与 Prepare 无关（同一会话 Prepare 只 1.76 ms），是安装后首次会话的冷缓存/频率差异，不作为对比依据；on2 与 off1 帧长相同。
- **结论**：迁移后 GpuComm 上只剩 `Flush` 里的录制线程同步（`RecorderSync`，约 1–1.4 ms/帧，两种模式都有）；FSR/后处理在 GpuComm 上本来只占约 0.08 ms 的 CPU，迁走后的 CPU 收益可以忽略，帧长无可测差别。GPU 侧 pass 仍在同一队列上执行，GPU 时间未单独测量。
- 真正压在 guest 命令线程上的是 `RecorderSync`：GpuComm 结束本帧时要等录制线程把命令全部回放完才能结束并提交命令缓冲。要去掉它，需要让录制线程自己结束并提交命令缓冲（Citron `VulkanWorker` 的做法），GpuComm 只交出一个“提交请求”；尚未实施。
- 画面：开启时截图正确（24.9 FPS 场景、世界内 28.6 FPS）；APK `2b3ff7f7` / host `4245f4be`（含 `host_pass_mutex` 与 7 节的层宿主改动）在世界内画面正确，状态层与 ImGui 通知（System RAM 提示）照常显示。安装前存档备份在本地 `build/validation/present-compose-20261007/`（两份），设备属性 `debug.shadps4.present_compose` 已清空（默认开）。

### 5.2 录制线程自己结束并提交命令缓冲（已实现，Thor 已测）

5.1 的数据说明 GpuComm 每帧结束本帧时等录制线程回放完（`Vulkan.RecorderSync`，约 1–1.4 ms/帧）才能 `vkEndCommandBuffer` 并入提交队列。现在改为（Citron `VulkanWorker` 的思路）：

- **GpuComm**：提交时把 profiler 的收尾时间戳（`GpuProfiler::EndBatchWith`，经录制线程写入）和一条“结束命令”录进当前块，交出全部块后照常把提交任务放进提交线程的 FIFO，然后直接开始下一个命令缓冲，不再 `Sync()`。
- **录制线程**：每块带着录制时的目标命令缓冲（`Job{chunk, cmdbuf}`），回放到“结束命令”时结束 debug label、`vkEndCommandBuffer`，再 `MarkFinished(serial)`。
- **提交线程**：任务开头若该缓冲尚未结束，`WaitFinished(serial)`（`Vulkan.RecorderFinishWait`），然后 `vkQueueSubmit`。FIFO 顺序不变，之后的提交（present）排在它后面。录制线程出错时等待方抛错，走提交线程原有的失败路径，不会死等。
- **命令池同步**：录制线程可能还在结束之前的缓冲时，GpuComm 已经 `begin` 下一个。命令池要求外部同步，因此带录制线程的调度器用 4 个池轮流取缓冲；取某个池之前，录制线程必须已结束同池的上一个缓冲（`WaitFinished(last_finish - 3)`，`Vulkan.RecorderLag`）。第一版只有 2 个池：每帧有两次提交（guest 命令与 VideoOut 取帧），GpuComm 开始下一帧时录制线程常常还在回放上一帧的尾部，`RecorderLag` 157/170 帧出现、平均 0.94 ms/帧，几乎抵消了收益；录制线程本身只有约 33% 忙（`Vulkan.RecordChunk` 165 ms/500 ms），是突发而不是吞吐不够。改为 4 个池后 `lag_waits` 为 0。
- **退回旧路径**：录制线程关闭、本缓冲被借出过 raw 命令缓冲（`CheckOutRaw`）、没有提交线程（桌面与 `debug.shadps4.async_submit=0`）或开 Tracy GPU 时，仍由 GpuComm 排空并结束缓冲。DebugBus `vk_recorder submit on|off`（默认 on），`vk_recorder status` 增加 `worker_submits`、`finish_waits`（提交线程实际等待次数）、`lag_waits`。
- **验证**：Android host、APK 与桌面编译通过。桌面没有提交线程，新路径不生效，只有池轮流；桌面血源（测试目录、存档副本）进世界 60 FPS、画面正常。12:34:58 第一次在 Thor 装包时结束了另一个任务正在跑的 TMNT 会话，之后停用设备，用户确认空闲后（19:03 起）再测。
- **Thor 实测**（APK `53dd63f1`，4 池；血源中央亚楠，同一会话 `vk_recorder submit on/off` 交替，各 6 s LiteP，GpuComm）：

| 轮次 | 帧长 | PM4.Resume | 其中 draw | 其余 | RecorderSync | SubmitExecution |
|---|---|---|---|---|---|---|
| on1 | 39.87 ms | 34.48 ms | 27.01 ms | 7.47 ms | — | 0.06 ms |
| off1 | 39.37 ms | 36.35 ms | 27.57 ms | 8.78 ms | 0.94 ms | — |
| on2 | 39.38 ms | 35.89 ms | 28.05 ms | 7.84 ms | — | — |
| off2 | 39.76 ms | 36.95 ms | 27.76 ms | 9.19 ms | 1.06 ms | — |

  开平均 PM4.Resume 35.19 ms、关 36.65 ms（−1.46 ms），去掉 draw 波动后其余部分 7.66 → 8.99 ms（−1.33 ms，即 RecorderSync 的约 1 ms 加两次提交本身）。帧长不变（GPU 86–94% 忙，帧率由 GPU 决定）。提交线程上 `Vulkan.RecorderFinishWait` 平均每次 0.68 ms，present 线程的 `Vulkan.WaitSubmitted` 每次约 0.17 ms，与改前相当；画面正确，`lag_waits=0`。2 池版本（APK `57b86989`）同法测得 on 35.3 / off 36.2 ms 帧长、`RecorderLag` 0.94 ms/帧。

## 6. 上机验证清单（Swan，Beat Saber 与一款 2D 影院游戏）

1. `xr_frame_copy status`：`copied` 增速应约等于游戏 FPS，`repeated` 约等于 runtime 帧率减游戏 FPS。
2. LiteP：XR 线程每帧 CPU 时间、`XR.Mailbox.ConsumeSubmit`/`SettleWait`、GPU 忙碌率，`xr_frame_copy new` 与 `every` 同会话交替 A/B。
3. 画面：系统合成器截图/录像（不是 `capture_source xr`）看重复帧有无闪黑、旧帧或撕裂；Pico 合成器是否接受重复提交未 acquire 的 swapchain（规范允许）。不接受时用 `debug.shadps4.xr_copy_every_frame=1` 回退。
4. 影院：画面比例、清晰度与改前一致，眼动 foveation 的影院 UV 映射不变。
5. Turnip `TU_DEBUG=perf` 下 `frame.image` 不再报 UBWC 关闭。
6. XR 输出：LiteP 中 present 线程不再有 `Present.AcquireImage` / `Present.DriverCall`，`Present.Xr` 很短；`XR.Mailbox.PublishSubmitWait` 不再包含 GPU 等待；`xr_mirror on/off` 切换后画面与帧率正常、帧池不卡（`Present.FreeFrameWait`）。
7. present 线程的主机 pass（5.1）：XR 的 `PrepareVrFrame` 仍在 GpuComm 上，未迁移；迁移后需在 Swan 上复核 FDM/ETFR 与每眼 UV。

## 7. 系统对话框与 ImGui 层在所有输出上可用（已实现，仅编译，未上 XR 设备）

问题：输入法、存档/消息/错误对话框、奖杯与通知都是 `ImGui::Layer`，原来只在窗口帧（`ImGui::Core::NewFrame`）里画。XR 输出时 Android 窗口不再呈现（第 4 节），这些对话框在头显里看不到，游戏会卡在等待对话框结果上。

做法：层的“宿主”与输出解耦，任一时刻只有一个宿主画层（对话框的按键边沿检测每帧只跑一次）。

- `ImGui::Core::SetExternalLayerHost(bool)` / `ExternalLayerHost()` / `DrawLayers()`：默认窗口帧画层；有外部宿主时 `NewFrame` 只处理增删、不画层。
- XR 宿主 `Vulkan::OpenXr::LayerPanel`（`openxr/layer_panel.{h,cpp}`）：独立 ImGui context，字体与窗口 context 同序（层用索引引用 `IMGUI_FONT_MONO`/`TEXT_BIG`，并按主机语言加载字形），1600×900 画布、1.28×0.72 m 的 LOCAL quad。有内容时出现在当时视线正前方 1.4 m、略低 0.15 m（只取偏航角），内容消失后下次重新定位；控制器 aim 射线命中时移动指针、扳机点击（射线到达时已按住不算点击）。对话框照常读虚拟手柄，XR 控制器本来就写入虚拟手柄，所以手柄操作与窗口模式一致。
- 渲染在 present 线程：`Presenter::PresentToXr` 与镜像分支、`DrawBlankFrame`/`DrawLastFrame` 在 XR 输出时调用 `Runtime::UpdateLayers()`（try_lock，非阻塞 `XrImguiVulkanLayer::Render`），XR 泵线程在 `xrEndFrame` 前把最近一次已 release 的图像作为最上层 quad 加入（错误面板显示时不加）。
- 会话开始（无错误）时设为外部宿主，会话结束或 LayerPanel 创建失败时还给窗口；`xr_mirror on` 时窗口镜像不画层（层仍在头显里）。
- 未验证：Swan 上对话框显示位置、可读性、射线点击、输入法候选，以及 LayerPanel 每帧开销。
