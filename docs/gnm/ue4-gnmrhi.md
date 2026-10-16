# UE4 PS4 后端（GnmRHI）：版本记录、公开钩子与 KH3 eboot 证据

后续已将公共 UE 接口与 KH3 入口直接关联，并补了 [UE 风格 C++ 参考版](kh3-gnmrhi-ue-reference.md)。目标是 KH3 的 4.17 分支，本文的 4.18 公共源码只作接口与通用行为参照。

## 1. 能拿到什么

- **代码**：GnmRHI 源码不公开，只通过 Sony/Epic 主机开发者渠道发放（S5）。公开 UE4 只有通用 RHI 和少量 `PLATFORM_PS4` 钩子。
- **发布说明**：4.15 起，PS4、XB1、Switch 的说明改为只对批准的主机开发者开放（4.15 发布说明 `ReleaseNotes_4_15.INT.udn:1710`）。因此 KH3 基线所在的 4.15–4.18 没有公开的 PS4 渲染条目。
- **KH3 自身**：
  - eboot 里有 `++UE4+Release-4.17`；
  - 源码路径 `D:\Tres\UE4\JK\Engine\Source\Runtime\PS4\...`；
  - GnmRHI 源文件 GnmCommands / GnmManager / GnmMultiBuffer / GnmShaders / GnmTexture / GnmUAV / GnmVertexBuffer.cpp。
  - Epic Japan 2019 讲座写的是 UE4.17.2，每约 3 个月合并一次引擎（S7，对应 KH3 为推断）。

## 2. 发布说明中的 PS4 条目（S3，论坛 raw 导出）

主题 URL 前缀 `https://forums.unrealengine.com/t/`：
- 4.9 `unreal-engine-4-9-released/40020`
- 4.10 `unreal-engine-4-10-released/46843`
- 4.11 `4-11-released/59818`
- 4.12 `4-12-released/65294`
- 4.13 `4-13-released/72651`
- 4.14 `4-14-released/78333`
- 4.15 `4-15-released/85321`
- 4.16 `4-16-released/92826`
- 4.17 `unreal-engine-4-17-released/97742`
- 4.18 `unreal-engine-4-18-released/101661`
- 4.19 `unreal-engine-4-19-released/107803`
- 4.20 `unreal-engine-4-20-released/113712`

原文副本在 `build/validation/kh3-gnmrhi-20261016/gnm-research/dl-forum/`。

| 版本 | 条目（转述） | 类别 |
|---|---|---|
| 4.9 | 关闭 LCUE 的 DispatchDraw 以降 CPU 开销（说明当时用 LCUE） | 命令缓冲 |
| 4.9 | 减少 context roll：只在需要时设 CbControl、ActiveShaderStages 和环形缓冲 | 命令缓冲 |
| 4.9 | GPU 分配器池化小分配；修临时块分配器的竞争 | 内存 |
| 4.9 | ContinuousSubmit 开关在 shipping 版可用 | 提交/同步 |
| 4.9 | 并行 translate 修正（去掉 late resolve）；bound shader state cache 支持并行 translate | 命令缓冲 |
| 4.9 | VB/IB 写锁不再多一次 memcpy；修 GNM scissor 校验 | 内存/修复 |
| 4.10 | “unsafe command buffers”选项，绘制线程省约 0.4 ms | 命令缓冲 |
| 4.10 | 命令缓冲块缩小到 64 KB，减轻 Onion 池压力 | 命令缓冲/内存 |
| 4.10 | VB/IB 默认放 Garlic，除非需要 CPU 回读 | 内存 |
| 4.11 | **去掉 compute 前后的自动 GPU flush，此后必须显式调用 RHITransitionResource** | 同步/异步计算 |
| 4.11 | fetch shader 放 Garlic；新增 GPU 池；命令缓冲分配器缩小 | 内存 |
| 4.11 | 并行渲染上下文有线程局部帧分配器；锁单 mip 2D 纹理不再触发并行渲染 flush | 命令缓冲 |
| 4.11 | 修一种 “SubmitDone not called”；unsafe command buffers 下 DMA copy 预留空间 | 同步/命令缓冲 |
| 4.11 | 修 CMask 清除时的通道映射与内存跟踪 | 纹理 |
| 4.11 | 实验性 Async Compute command list 接口 | 异步计算 |
| 4.12 | **实验性流式纹理 GPU 碎片整理** | 碎片整理 |
| 4.12 | 主区用尽时退回 flexible 内存（多 100–200 MB）；OOM 日志细化 | 内存 |
| 4.12 | 存档对话框导致 SubmitDone 未调用的问题已修；SubmitDone 间隔超过 2 s 记日志；GPU flush 后立即调 SubmitDone | 同步 |
| 4.12 | DrawIndirect 后重置 NumInstances；支持 BC6H/BC7 | 修复/纹理 |
| 4.13 | **修 async texture realloc 与帧末重叠导致的偶发 GPU hang** | 碎片整理/同步 |
| 4.13 | 并行上下文未关闭 GS 模式（状态跨上下文残留） | 命令缓冲 |
| 4.13 | 修 Garlic/Onion 初始分配大小 | 内存 |
| 4.14 | `r.PS4DefragPoolSize` 与 `r.Streaming.PoolSize` 解耦 | 碎片整理 |
| 4.14 | **async compute 清除半透明光照体积的 fence 错误，在 PS4 上表现为闪烁或偶发 GPU hang** | 异步计算/同步 |
| 4.14 | AsyncCompute command list 上 `SetShaderValue()` 的 bool 特化 | 异步计算 |
| 4.14 | DrawIndirect / DispatchIndirect 可能 GPU hang | 同步 |
| 4.14 | PS4 GPU 计时扣除 command list 之间的空泡 | 计时 |
| 4.14 | Pro 上的 4K 后缓冲（`r.PS4Allow4kOutput`）；Pro 渲染目标对齐 | flip/纹理 |
| 4.15 | 主机说明移至 UDN/主机论坛；通用 growable heap GPU 分配器 | — |
| 4.16 | PS4 SDK 4.508.001 | — |
| 4.17 | 无 PS4 平台条目（只有 Unreal Audio 支持 PS4） | — |
| 4.18 | SDK 4.508.111 / 固件 4.730.001；LLM 内存跟踪支持 PS4 | 内存 |
| 4.19 | PS4/XB1 动态分辨率；`r.GTSyncType` | flip |
| 4.20 | Niagara GPU 模拟回读支持 PS4 | 异步计算 |
| 4.26 | UE-100910：GnmRHI 的 RHICopyTexture 对 PF_FloatRGB 出错；UE-102262：PSVR 在 GnmContextCommon.cpp:1012 崩溃 | 纹理/崩溃 |

## 3. 4.18 公开代码中的 PS4 钩子（S6 @ `b50e6338`）

| 文件:行 | 含义 |
|---|---|
| `Core/Public/Stats/Stats2.h:2053-2054` | `STATGROUP_PS4RHI`（显示名 Gnm）与 GnmVerbose，唯一的 PS4 统计组 |
| `RHI/Public/RHIDefinitions.h:502` | “On PS4, BUF_Dynamic do not support multiple updates per frame”：这是 API 使用限制，不能推出内部按帧切换一份。KH3 普通 Dynamic 实际是三份、每次写 Lock 轮换，Dynamic\|UAV 单份；见[二进制还原第 4 节](kh3-gnmrhi-reconstruction.md#4-fgnmmultibufferresource-与-lock) |
| `Renderer/Private/DistanceFieldLightingShared.h:469`、`DistanceFieldObjectManagement.cpp:54,82` | Volatile 在 PS4 上支持一帧多次写；“PS4 volatile only supports 8Mb”，即有界池 |
| `Engine/Private/SkeletalRenderGPUSkin.cpp:72-77` | PS4 上 compute 写的 morph target 缓冲必须是 `BUF_Dynamic \| BUF_UnorderedAccess` |
| `Engine/Private/UnrealClient.cpp:533-560` | `stat unit` 在 `r.PS4ContinuousSubmits=0`（利于 profile、不利于高帧率）或 `r.PS4StallsOnMarkers=1` 时警告 |
| `Renderer/Private/CompositionLighting/PostProcessAmbientOcclusion.cpp:28` | “AyncCompute is currently only functional on PS4”（SSAO compute） |
| `Renderer/Private/PostProcess/PostProcessMorpheus.cpp:255` | PSVR 畸变由 SDK 完成 |
| `Engine/Private/GPUSkinCache.cpp:116,123` | PS4 上 GPU skin cache 关闭 |
| `RHI/Public/RHI.h:137-150` | PS4 不走 MSAA 路径 |
| `RHI/Public/RHIDefinitions.h:649` | `TexCreate_ReduceMemoryWithTilingMode`（128³ 体纹理在 PS4 tiling 下膨胀 4 倍） |

**以下 cvar 在公开代码中不存在**，只在 GnmRHI 里：r.PS4CommandBufferSplitSize、r.PS4EventBasedFrameEnd、r.PS4DoComputeAutoFlush、r.PS4.AsyncComputeBudgetMode、r.PS4.AllowDefrag、r.PS4FlushForAsyncReallocateTexture2D。

**PS4 后缀的类不是平台变体**。`FPostProcessTemporalAAPS4*`、`FShadowProjectionPS4*`、`FPostProcessUpscalePS4` 都是 token 拼接出的着色器排列：
- `FPostProcessTemporalAAPS##A##B`（`PostProcessTemporalAA.cpp:203-213`）；
- `FShadowProjectionPS##Quality##UseFadePlane`（`ShadowRendering.cpp:196-212`，Quality 4）；
- `FPostProcessUpscalePS##A`（`PostProcessUpscale.cpp:220-228`，4 = Lanczos 3）。

## 4. 4.17/4.18 的帧执行模型

**游戏线程 → 渲染线程**：
- `FFrameEndSync::Sync(r.OneFrameThreadLag)`（`LaunchEngineLoop.cpp:3436-3444`）。
- 默认 OneFrameThreadLag=1，游戏线程最多领先渲染线程一帧；4.18 里游戏线程这里不等 RHI 线程或 GPU。

**渲染线程 → RHI 线程**：
- `FRHICommandList::EndDrawingViewport`（`RHICommandList.cpp:1600-1633`）插 RHI 线程 fence，然后等上一帧的 fence，保证渲染线程最多领先 RHI 线程一帧。
- GPU 可以落后 RHI 线程多少，由平台的 `RHIEndDrawingViewport` 决定（Gnm 提交 + flip，不公开）。
- 最坏情况：游戏 N+1、渲染 N、RHI N−1、GPU N−1−k。

**并行 translate**：
- 各 pass 的 command list 在任务线程上 translate 到 deferred context，再由 RHI 线程按序提交（`RHICommandList.cpp:1051-1110`）。
- 这对应 KH3 一次提交多段 DCB（#17：72 段）以及多个提交线程。

**提交提示**：`RHISubmitCommandsHint` 在 `FinishOcclusion` 调用（`DeferredShadingRenderer.cpp:529-547`），目的是让下一帧读查询结果时少等。

## 5. 资源生命周期约定：平台 RHI 必须多缓冲的东西

**uniform buffer 不可变**：
- 本次读过的 4.18 接口提供 `RHICreateUniformBuffer`，没有找到 `RHIUpdateUniformBuffer`；这里不推断后者首次引入的版本。KH3 创建时复制的实证见[二进制还原第 5 节](kh3-gnmrhi-reconstruction.md#5-uniform-buffer-和临时块寿命)。
- `UniformBuffer_SingleDraw` / `SingleFrame` / `MultiFrame` 三种用途（`RHIDefinitions.h:333-341`）。

**view 常量每帧在新内存里新建**：
- `FViewInfo::InitRHIResources` 以 SingleFrame 创建（`SceneRendering.cpp:1127`）；
- 同一帧内还会再建：全局距离场之后，以及每个阴影深度 view 用光源矩阵（`ShadowDepthRendering.cpp:2010-2021`）。
- 创建路径调用 RHI，平台须保证引用它的 GPU 工作完成前 backing 有效；不能用固定“2–3 帧”代替 KH3 已恢复的 ticket / 块池寿命条件。
- view 常量成员表在 `SceneView.h:586-650`：开头是 TranslatedWorldToClip 等矩阵，表末是 FrameNumber 与 StateFrameIndexMod8，可以用来判断 draw 读到的是哪一帧的常量。KH3 改过引擎，偏移需另行核实。
- 同类每帧新建的还有：camera motion 缓冲（TAA/SSAO，取自 `PrevViewMatrices`）、forward light 数据、GBuffer 资源、GPU 粒子 per-view 缓冲。

**per-object 常量**：transform 变化时新建 MultiFrame 对象（`PrimitiveSceneProxy.cpp:256-274`），旧对象的内存要等 GPU 用完才能释放。

**loose 常量**：非 Bypass 时，`SetShaderParameter` 的值先拷进 command list 内存（`RHICommandList.h:2073-2083`）；执行时再写 Gnm CPU shadow，draw / dispatch 准备阶段复制到新的上传区并绑定 descriptor。KH3 CS 入口为 `0x377bc0`，shadow 写入 `0x368f70`，上传 `0x36d740`。已替代早先“可能嵌在命令缓冲”的猜测，详见[三层复制](kh3-gnmrhi-ue-reference.md#3-loose-常量的三次复制要分开)。

**VB/IB**：
- `BUF_Volatile`：只活一帧，每帧都必须写。
- `BUF_Dynamic`：偶尔写，活到下一次更新。
- `FGlobalDynamicVertex/IndexBuffer` 是 64 KB 对齐的 Volatile，每帧对同一 RHI 对象 `RLM_WriteOnly` 重锁，平台必须每次换新的后备内存，即环形分配或重命名。
- 骨骼矩阵用 `BUF_Dynamic`，引擎自己按帧奇偶保留当前/上一帧两份。

**RHI 线程写路径**：有 RHI 线程时，`LockVertexBuffer_RenderThread` 先给一块 malloc 暂存，Unlock 时排一个更新命令，真正的 `RHILock` 在 RHI 线程上执行（`RHICommandList.cpp:2092-2129`）。

**延迟删除**：通用 UE 的 `GRHINeedsExtraDeletionLatency` 按 `FlushPendingDeletes` 帧龄控制 RHI 对象删除；KH3 平台 backing 另有 `0x37d6210` ticket 入队和 `0x37d6b20` 按 GPU 完成值回收。对象引用、帧龄和实际 GPU backing 寿命须分开，详见[回收还原](kh3-gnmrhi-reconstruction.md)。

**纹理**：
- `RHIUpdateTexture2D`、`RHIAsyncReallocateTexture2D` 只同步 RHI 线程，不等 GPU。
- 碎片整理在平台 RHI 内实现（通用 `appDefragmentTexturePool` 是空函数），要移动活着的纹理，需要 GPU fence 保护的拷贝。

## 6. 异步计算与查询

**异步计算**：
- `FRHIAsyncComputeCommandListImmediate::ImmediateDispatch` 把 compute list 排进图形 immediate list，保证之前的资源更新先执行（`RHICommandList.cpp:189-221`）。
- `FRHIComputeFence` 每帧只写一次；`RHITransitionResources(..., WriteComputeFence)` 发信号，`RHIWaitComputeFence` 等待。
- 使用者：async TAA、SSAO compute、半透明光照体积清除（4.14 的 PS4 闪烁问题即出在这里）。

**遮挡查询**：
- `r.NumBufferedOcclusionQueries` 默认 1，每帧以 **bWait=true** 读上一帧的结果（`SceneVisibility.cpp:750-760`、`SceneOcclusion.cpp:214-235`）。这就是 KH3 渲染线程在 `RHIGetRenderQueryResult` 里空转的来源。
- 结果为 0 个样本时下一帧剔除，覆盖：
  - 图元；
  - per-object / spot 阴影；
  - 点光源（`SOQ_LightInfluenceSphere`）；
  - 方向光第二级以后的级联（仅 `r.Shadow.OcclusionCullCascadedShadowMaps=1`，默认 0）。

**时间戳**：`FRealtimeGPUProfiler` 以 bWait=true 收集（`SceneUtils.cpp:243-251`）。

## 7. TAA、阴影投影、天空依赖的数据

**TAA**：
- 输入：场景色、`TemporalAAHistoryRT`（镜头切换时换成当前场景色）、velocity。
- 还读 view 常量里的 TemporalAAJitter、ClipToPrevClip、PrevViewProj，以及 camera motion 缓冲。
- `PrevViewMatrices` 在 InitViews 中轮换（`SceneVisibility.cpp:2362-2392`）。

**阴影投影**：
- 读 view 常量（深度到世界的变换）、GBuffer/深度、loose 的 `ScreenToShadowMatrix`、阴影深度图。
- 阴影深度 pass 用自己的 single-frame view 缓冲（光源矩阵）。

**天空与太阳**：view 常量里有 `SkyIrradianceEnvironmentMap[7]`、`DirectionalLightDirection/Color`、`AtmosphericFogSunDirection`，以及大气纹理。

如果某些 draw 读到了另一帧的 view 常量，可能出现：
- 相机角度不对；
- 阴影投影错位或丢失，因为 loose 矩阵与 view 常量不一致；
- 天空 SH 或太阳方向异常。

## 8. KH3 eboot 中的 GnmRHI

偏移是模块偏移，运行时地址 = 0x400000 + 偏移。证据文件在 `build/validation/kh3-gnmrhi-20261016/`。

**cvar 与帮助文字**：
- **r.PS4CommandBufferSplitSize**：命令缓冲达到该大小时自动拆成一次单独 submit。
- **r.PS4ContinuousSubmits**：GPU 何时从 CPU 拿到命令，即边录边交。
- **r.PS4DoComputeAutoFlush**：开启时 compute dispatch 前后做 GPU wait + cache flush（模拟 DX11）。
- **r.PS4EventBasedFrameEnd**：提交线程基于事件，0 为轮询。
- **r.PS4TimeSubmitWaits**：统计“提交来得不够快”造成的 GPU 饥饿，从 stat unit 的 GPU 时间中扣除。
- **r.PS4.AsyncComputeBudgetMode**：0 = CU masking。
- **r.PS4WindowedVSync**：flip 用 SCE_VIDEO_OUT_FLIP_MODE_WINDOW；另有 Top/Bottom margin。
- **r.PS4DefragPoolSize**：碎片整理池大小（MB）。
- **r.PS4.AllowDefrag**、**r.PS4.GPUDefragPerfTest**：每帧用 GPU memmove 搬一大块，用于测试。
- **r.PS4FlushForAsyncReallocateTexture2D**。
- **r.GPUDefrag.EnableTimeLimits / MaxRelocations / AllowOverlappedMoves**。
- **r.PS4.DumpPM4**、**r.PS4MixedModeShaderDebugInfo**、**r.PS4DumpShaderSDB**。

**静态初值**：
- 来源是 `fn_3a9470` 中 `RegisterConsoleVariableRef` 引用变量的初值，ini 可能覆盖。
- 取值：
  - SplitSize = 32768
  - ContinuousSubmits = 1
  - EventBasedFrameEnd = 1
  - TimeSubmitWaits = 1
  - DoComputeAutoFlush = 0
  - AsyncComputeBudgetMode = 1
  - StallsOnMarkers = 0
  - WindowedVSync = 0
  - DefragPoolSize = 2300

**其他字符串**：
- “FGnmMultiBufferResource created without Static, Volatile, or Dynamic specified”
- “async submitCommandBuffers failed / async compute submit failed”
- “Final submission failed”
- “Error: Neo does not support WindowedVSync”
- “RHICopyVertexBuffer is not supported yet in Gnm”
- “PS4 OOM: CPU, Garlic, Onion, FrameBuffer, Flexible”
- 线程名：SubmitDoneAsyncTaskThreadPS4、SubmitGPUCommandsAsyncTaskThreadPS4

**函数位置**（字符串和导入的交叉引用，尚未反编译）：

| 偏移 | 作用（推测） |
|---|---|
| `fn_3a9470` | cvar 注册（GnmRHI 初始化） |
| `fn_3a5640` | 创建两个提交线程 |
| `fn_39c920` | 帧末提交 + flip（0x39ca47 调 SubmitAndFlip，0x39ca90 调 SubmitCommandBuffers） |
| `fn_39cbb0` | 异步提交线程（0x39d152 SubmitCommandBuffers，0x39d26a SubmitDone） |
| 0x395e74、0x3a8d79 | 其他 `sceGnmSubmitDone` 调用点 |
| `fn_3a3010` | FGnmMultiBufferResource 构造 |
| `fn_3b2c60` | 碎片整理相关 cvar 注册 |
| 0x3a0d10 | `RHIGetRenderQueryResult` 等待循环（已有 guest patch `guest/games/CUSA15072/01.10/query_wait.cpp`） |

反编译分工见 [交接文档](../handoffs/kh3-gnmrhi-ida-20261016.md)。
