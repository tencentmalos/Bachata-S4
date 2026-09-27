# Pipeline 编译 P1b：可选跳过未就绪 draw；计算 dispatch 先 HLE 再建管线（2026-09-26）

分支 `feature/malos/mhw_fix`，本地未提交。接续 [P1a 准确异步](pipeline-compile-async-20260926.md)，对应 spec `docs/specs/pipeline-compile-cache-20260926.md` 的 P1b 与 §5（`origin/malos/main` `14306f95`）。

## 1. 计算 dispatch：HLE 判定先于管线创建

`PipelineCache::GetComputePipeline` 拆为两步：

1. `PrepareComputeProgram()`：翻译 shader，得到 `Shader::Info` 与管线 key，不创建管线。
2. `GetPreparedComputePipeline()`：按 key 取出或创建管线。

`DispatchDirect` 拿到 info 后先依次尝试 `ExecuteShaderHLE`、`TryComputeImageFill`、`TryComputeImageStoreFill`；任一接管就直接返回，不创建驱动管线（也不进入编译队列）。只有全部不接管时才取管线并绑定资源。

- `TraceAction` 改为接收 stage 列表，HLE 接管的 dispatch 仍按原样写进 PM4 trace。
- `DispatchIndirect` 没有 HLE，行为不变。
- 这些 shader 仍会翻译，因为 HLE 判定依赖翻译得到的资源表。

桌面血源同路径对比：之前 31 条计算管线，现在 28 条。少掉的 4 条中，3 条是常量 `image_store` 清图 kernel cs `0x9a9cf8a9` 的三个排列（由 `TryComputeImageStoreFill` 取代），1 条是 cs `0xfefebf9f`；新增的 1 条是新场景内容。

## 2. `async_graphics_skip`（有损，可选）

模式 `Vulkan.pipeline_compile_mode = async_graphics_skip`（Android 设置“Pipeline Compilation”第三项，JNI 值 2；DebugBus `pipeline_cache mode async_graphics_skip`）。在 `async_accurate` 基础上，直接 draw 的管线还没建好时丢弃这个 draw，而不是等待。默认仍为 `sync`。

**判定位置**：`FilterDraw` → 取管线之后，`PrepareRenderState / BindResources / BeginRendering` 之前（`Rasterizer::SkipUnbuiltDraw`）。被跳过的 draw 在跳过点之前只经过了 PM4 状态解析、shader 翻译和管线 key，所以：

- 不查找/创建渲染目标，不上传纹理；
- 不消费 CMASK/HTILE 清除，不改变图像内容或 dirty 状态；
- 不暂存访问集，不调用 `NoteDraw/RecordAttachmentDraw`，不推进 HostDraw 诊断；
- 不影响此前其它命令已产生的 pending 操作。

**门控**：以下情况不跳过（管线未就绪时在绑定处等待），并按原因计数：

| 原因 | 条件 |
| --- | --- |
| 副作用 | 任一 stage 写 buffer/image、用原子操作或翻译失败。构造时静态计算，含 ES/GS ring、GDS 等写入的特殊 buffer。 |
| predicated | draw 包头的 predicate 位（Liverpool 在三种直接 draw 包上记录）。 |
| stream-out | `VGT_STRMOUT_CONFIG` 任一流启用。 |
| clear/meta | depth/stencil clear、copy、resummarize、decompress，或颜色操作模式不是 Normal/Disable。 |
| 跳过过久 | 同一管线已连续跳过 3 s 仍未建好，改为等待。 |
| 记录表满 | 受影响目标表满（256 个）后，本会话不再跳过。 |

间接 draw 不走跳过路径。

**帧内稳定**：Liverpool 在 GFX 流里每处理一个 PatchedFlip NOP 就把 `flip_epoch` 加一，这就是帧边界，与 draw 同线程同顺序。本帧已跳过过的管线，即使中途建好，本帧剩余的 draw 仍跳过，下一帧起再画，避免物体在同一帧中途出现。若一直没有 flip，这种保持最多 50 ms。必须等待的 draw（门控不通过）不受此限制。

**可追溯**：每个被跳过的 draw 记录它本应写入的颜色附件与（写深度时的）深度附件 guest 地址，以及次数和首末帧。每条管线首次被跳过和恢复绘制时各写一条 INFO 日志。`pipeline_cache status` 输出：

- 跳过次数（其中多少是“建好后本帧保持”）、涉及管线数、已恢复数；
- 受影响帧数、最长连续帧数；
- 各门控原因计数；
- 受影响附件前 16 个。

PM4 trace 为被跳过的 draw 写 `HostEvent::Skipped`（解码器显示为 `xx skipped`），另有 profiler counter `Pipeline.SkippedDraws`。

**未做（spec §4）**：
- 缺失内容沿采样、拷贝、resolve 的传播跟踪；
- 进入回读、indirect 或跨帧反馈时自动切回等待；
- 跳过候选的每帧等待预算（当前为 0）。

这是有损模式：被跳过的阴影、深度预渲染或后处理输入会影响之后读取它们的 pass。

## 3. 桌面验证（血源，890M 核显，recipe 缓存关，录制线程开）

| 模式 | 新管线 | 绑定处等待 | 跳过 |
| --- | --- | --- | --- |
| `async_graphics_skip` | 213 图形 + 28 计算，240 由 worker、1 由首个使用者构建；队列峰值 8 | 2 次，225 ms | 1406 个 draw（其中 528 个为“建好后本帧保持”），211 条管线，204 条已恢复；67 帧受影响，最长连续 8 帧；门控拦下 1 次（clear/meta）；记录 87 个附件 |
| `async_accurate`（validation layer 下） | 202 + 28，全部由 worker 构建 | 130 次，1.15 s | — |

- 同类场景在 P1a 的 `async_accurate` 冷启动轮为 113 次、3.68 s 等待。本轮 AMD 驱动内部缓存已热，绝对值不能比。
- skip 轮进入场景后截图画面完整；热身期缺失的内容未在截图中捕捉到。
- validation 轮没有任何与管线创建、creation feedback、绑定相关的 VUID。出现的 `vkCmdCopyImage-srcImage-01548/01551`（约 3800 次）、`vkAcquireNextImageKHR-semaphore-01779`、`vkCmdCopyBufferToImage-dstImage-07975`、`VkDeviceCreateInfo-pNext` 都在本次未改动的路径上。
- 测试后桌面 `config.json` 未改变（模式只经 DebugBus 覆盖）。
- Android host `HOST_LINK_PASS`、APK 编译通过；Kotlin `PipelineCacheTest` 5/0、`RuntimeSettingCatalogTest` 3/0；PM4 trace 解码器 selftest ok。AYN 未连接，未实机运行。
