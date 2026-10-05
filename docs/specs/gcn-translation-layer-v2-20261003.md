# GCN 翻译层 v2：PM4/GCN → Vulkan 的系统设计（2026-10-03）

状态：设计与分阶段实施 spec，本轮未实现。

依据：
- 对本仓 `b62109895`（`feature/mac_fex_arm64`）`src/video_core` 与 `src/shader_recompiler` 的只读审计。
- 对 DXVK（Proton fork 2.7.1，`a6764047`）、vkd3d-proton（`212991fc`）同类机制的对照，两者源码位于 `D:\workspace\xrgame_native\references\proton\`。
- Citron 的同类设计：`D:\workspace\citron\docs\plans\20261003-gpu-translation-layer-v2-design.md`。

与现有文档的分工：

| 文档 | 范围 |
|---|---|
| [GCN 模拟实现](../guides/gcn-emulation.md) | shader 层面的语义、数值、lane/LDS 回退、插值与固定功能，以及它的改进清单 |
| [Pipeline 编译与缓存](pipeline-compile-cache-20260926.md) | 驱动 PSO cache 持久化、预热顺序、有界后台编译 |
| 本 spec | 翻译层架构：线程划分、寄存器状态、资源发现与绑定、Guest 内存一致性、同步语义、render pass 与 barrier、提交节奏，以及迁移与验证方法 |

下文路径均相对于 `src/`；DXVK 路径相对于 `dxvk/src`，vkd3d 路径相对于 `vkd3d-proton/libs/vkd3d`（另有注明的除外）。

---

## 0. 结论

本仓的 GPU 层比 yuzu 系实现新，已经有几样做得对的东西：

- 按 VA 索引的 sparse arena；
- 从 DXVK 移植的 buffer 冒险跟踪器；
- dynamic rendering；
- 录制线程上的 pre-pass 上提；
- 页 watcher 引用计数与写故障预测；
- 静态资源发现（`SharpFetch`）。

但它和 Citron 有同样的根本问题，而且实测已经显示，局部记忆化的收益见顶：

1. **GpuComm 线程上，每个 draw 都在重建、重解析、全量推送。** 管线 key 每个 draw memset 后重建；每个 stage 每个 draw 都要走 SRT、匹配特化、推送全部描述符，同时还在这个线程上做 Vulkan 编码。
2. **shader 翻译同步阻塞在 GpuComm 上**，只有 `vkCreate*Pipeline` 是异步的。
3. **EOP/EOS 在解析时就 signal**，并且默认关闭回读。GPU 的结果对 CPU 不可见，只能靠 `GpuByteKeeper`、缺失内容跟踪等一层层补救；真正需要回读时，又是从缺页的 Guest 线程发起一次 `Finish()` 全停。
4. **barrier 粒度粗**：buffer 冒险时发一个全局 ALL_COMMANDS barrier；image barrier 由 `Transit` 立即逐子资源发出；上传、拷贝、dispatch 都会打断 pass。
5. **PM4 在 Guest 内存里原地解析**，与 Guest 存在竞争（为此才有 `pm4_validate`）。

实测（[血源桌面瓶颈](../validation/android-native-host/bloodborne-desktop-bottleneck-20261002.md)）：

- **桌面**：GpuComm 每帧约 12–15 ms CPU，`Rasterizer::Draw` 占 83%。其中：
  - `BindResources` 38%（`BindTextures` 20.9%，`BindBuffers` 15.5%）；
  - `ObtainBuffer` 23.8%（`CopySparseMemory` 9.1%、`SynchronizeMemory` 9.1%、`IsRegionGpuModified` 4.8%，含 FutexMutex）；
  - `GetGraphicsPipeline` 14.2%（`RefreshGraphicsKey` 13.7%，其中 `GetProgram` 12.8%、`StageSpecialization` 6.2%）；
  - `FindImage`、`ImageInfo`、`FindView` 合计约 20%。

  另外，GXWorker 有 12–16% 的运行样本在处理写跟踪的异常。
- **Android（AYN Thor）**：GpuComm 每帧约 23 ms，900 个 draw 共 16.7 ms（约 18.6 µs/draw）。Guest-1 加速之后，它就是下一个瓶颈。
- **局部缓存的收益**：按 T# 缓存纹理绑定，命中率 99.9%，桌面上每帧省 0.77 ms，Android 上没有可测差别。

v2 的方向与 Citron 一致：

- 把 Vulkan 决策整体移出 GpuComm；
- 把资源解析从“每个 draw 拉取”改成“写入或变化驱动”；
- 同步以真实完成为准；
- shader 一次翻译、链接期特化；
- 先建确定性回放，再按接缝整体替换子系统。

---

## 1. 根本问题与证据

| 根本问题 | 本仓现状（证据） | 参考答案 |
|---|---|---|
| GpuComm 既解析 PM4 又做全部 host 工作 | 绘制同步调用 `Rasterizer::Draw` 等（`video_core/amdgpu/liverpool.cpp:597-794`）；寄存器翻译、绑定、Vulkan 编码都在该线程 | DXVK：API 线程只做前端，Vulkan 全在 CS 线程（`dxvk/dxvk_cs.cpp:117-275`） |
| 没有逐寄存器的脏跟踪 | `RefreshGraphicsKey` 每个 draw memset 并重建（`renderer_vulkan/vk_pipeline_cache.cpp:608-715`）；`UpdateDynamicState` 全量重算（`vk_rasterizer.cpp:2727-3012`）；`BuildRuntimeInfo` 每个 stage 每个 draw 都跑（`vk_pipeline_cache.cpp:99-258`） | DXVK D3D9：两层脏标记（`d3d9/d3d9_device.cpp:2336-2700,7627-7822`） |
| 资源解析与特化匹配逐 draw 进行 | `GetProgram` 对每个候选 permutation 都重跑 SRT 并构造 `StageSpecialization` 比较（`vk_pipeline_cache.cpp:900-941`）；`BindResources` 每个 draw 全量推送（`vk_rasterizer.cpp:898-1112`，`vk_pipeline_common.cpp:99-126`） | DXVK 惰性绑定（`d3d11/d3d11_context.cpp:4533-4562`）；vkd3d bindless，draw 时只推 table 偏移（`command.c:7329-7389`） |
| shader 翻译同步 | `CompileModule` 在 GpuComm 上（`vk_pipeline_cache.cpp:863-898`）；默认 `async_graphics_skip` 丢弃 draw（`vk_rasterizer.cpp:150-261`） | DXVK：注册即编译、绑定时提升优先级、GPL 快速链接 + 异步优化版（`dxvk/dxvk_graphics.cpp:1077-1107`） |
| 解析时 signal | EOP/EOS/RELEASE_MEM 在解析到时立即写 fence 并发中断（`liverpool.cpp:839-876,1315-1328`）；回读默认关（`core/emulator_settings.h:396-478`）；回读路径 `SendCommand<true>` → `Finish()`（`buffer_cache/buffer_cache.cpp:438-518`） | vkd3d：虚拟 fence 映射到 timeline，由 fence worker 在完成时回调（`command.c:1111,617`）；DXVK：按序号等待（`d3d11/d3d11_context_imm.cpp:899`） |
| barrier 与 pass | 冒险时全局 ALL_COMMANDS（`vk_runtime.cpp:276-347`）；image `Transit` 立即发出（`texture_cache/image.cpp:627-819`）；storeOp 永远是 STORE；拷贝、上传、dispatch 结束 pass | DXVK 默认状态 acquire/release + 合批（`dxvk/dxvk_context.cpp:8878,8990`）、`finalizeLoadStoreOps`（`:2462`）、init 命令缓冲（`:9596-9666`） |
| PM4 原地解析 | 默认直接读 Guest 内存（`liverpool.cpp:1530-1543`） | 提交时拷贝成自有副本，同时也是确定性回放的前提 |
| 桩与启发式 | `SET_PREDICATION`、`COPY_DATA` 未实现，streamout flush 直接报完成，occlusion query 返回合成值（`liverpool.cpp:585-591,822-835`）；NOP 尺寸提示缺失即断言（`:497-557`）；按代码字精确匹配的 HLE；魔术地址 `0x3022C`（`:879`） | 按游戏的策略表 + 诊断；用条件渲染和 Vulkan query 正确实现 |

---

## 2. 目标架构

```
 Guest（GNM 提交、内存映射、等待）
        │ 提交拷贝成自有 DCB/CCB/ACB 副本（带序号）
        ▼
 ┌────────────── GpuComm：PM4 前端 + 翻译器（不碰 Vulkan）───────────────┐
 │ 协程按队列解析 PM4（保留）· 寄存器文件 + 脏位图 · 状态块               │
 │ 变化驱动的资源解析：SRT 读集合受页 watcher 覆盖、sharp → 描述符槽       │
 │ 产出类型化 POD 后端命令（状态增量 + draw 参数 + 已解析句柄）            │
 └──────────────────────────────┬─────────────────────────────────────────┘
                                ▼ 命令块（序号）
 ┌────────────── 录制线程 → 后端上下文（独占 Vulkan）─────────────────────┐
 │ 管线变体查找 · 动态状态 · 描述符/推送 · 惰性 pass · 延迟 clear          │
 │ buffer + image 冒险跟踪与合批 barrier · init 命令缓冲（泛化现有上提）   │
 └──────────────────────────────┬─────────────────────────────────────────┘
                                ▼ EXECUTE / SIGNAL / PRESENT 条目
     提交线程（已有 SubmissionWorker）──▶ 完成线程：EOP/EOS 写入与中断、回读、释放
     旁路：GCN 翻译 worker（有优先级）· 管线 worker（已有）· 呈现线程（已有）
```

与现状相比：

- 录制线程、提交线程、管线 worker、呈现线程都已存在。
- 主要变化有三点：GpuComm 不再碰 Vulkan；录制线程从“回放闭包”升级为“后端上下文”；新增一个完成线程，让 EOP/EOS 以真实完成为准。

---

## 3. 关键设计

### 3.1 后端上下文与命令流

- **录制线程升级为后端上下文**，相当于 DXVK 的 `DxvkContext`。它负责：
  - 应用后端脏标记、查找管线变体、设置动态状态（现有 `Scheduler` 的 `DynamicState` setter 已经会和上次比较，`vk_scheduler.h:108-361`，保留）；
  - 写描述符；
  - 惰性开始和结束 pass（现有 `BeginRendering` 用 memcmp 比较 `RenderState`，`vk_scheduler.cpp:80-211`，保留）；
  - 冒险跟踪与合批 barrier；init 命令缓冲。
- **GpuComm 只产出类型化的 POD 命令。**
  - 每条命令带一个可变长数据尾，最后一条 draw 可以追加参数以合并连续 draw，后端用 `vkCmdDrawMulti*EXT`（DXVK `dxvk/dxvk_cs.h:143-176,284-295`，`d3d11/d3d11_context.cpp:3534-3555`，`dxvk/dxvk_context.cpp:1547-1661`）。
  - 命令块带序号；CPU 回读只同步到相关的块（DXVK `dxvk/dxvk_cs.cpp:117`）。
- **pre-pass 上提泛化为 init 命令缓冲。** 本次提交里还未被使用的资源，其上传、clear 和初始转换都进 init 命令缓冲（DXVK `prepareOutOfOrderTransfer`），不再依赖“独立于当前 pass”的地址区间判断（`Scheduler::BeginHoist`，`vk_scheduler.cpp:385-429`）。

### 3.2 寄存器状态：脏位图与状态块

- **寄存器写入时置脏。**
  - `SET_*_REG` 写进 `Regs::reg_array` 时（`amdgpu/regs.h:21-25,165`），同时在按寄存器区间（context、sh、uconfig）划分的脏位图上置位。
  - 映射表把寄存器映射到脏组：管线 key、动态状态、渲染目标、各 stage 的 user data、各 stage 的 shader 地址（DXVK D3D9 的 26 个脏组，`d3d9/d3d9_device.h:57-88`）。
- **三个状态块，各有变化代际：**
  - **管线 key**：按脏组增量更新，哈希也增量维护；前面放一个直接映射缓存（DXVK 用 4096 项，`dxvk/dxvk_context.cpp:8307-8315`）。这正是[桌面报告](../validation/android-native-host/bloodborne-desktop-bottleneck-20261002.md)第 8 节提到的“寄存器未变化时复用 `RefreshGraphicsKey` 的结果，约占 14%”。
  - **动态状态**：只在对应脏组变化时重算（现状每个 draw 全量重算）。
  - **`RuntimeInfo`**：只在影响它的寄存器组变化时重建（现状每个 stage 每个 draw 都重建）。
- **`StageSpecialization` 的记忆化。** 输入是 `RuntimeInfo`、fetch shader 布局、各 sharp 中与代码生成相关的字段，以及 user data。以“program × 这些输入的代际”为键做记忆化。桌面报告已经注明这需要先审计全部输入，审计结果应写进 spec 附录。

### 3.3 资源发现与描述符

现状：每个 draw、每个 stage 都要做下面这些事：

- `PushUd`；
- `RefreshFlatBuf`（SRT 走表：x86 上是 JIT，其他平台是 `PortableSrt` 解释器，`shader_recompiler/ir/passes/srt_portable.h:174-258`）；
- `BindBuffers`、`BindTextures`；
- 推送全部描述符（`vk_pipeline_common.cpp:99-126`）。

**设计**

1. **按代际门控 SRT 走表。**
   - 记录每个 stage 走表时读到的 Guest 地址集合（SRT 程序是静态的，读集合只取决于 user data 与表内容）。
   - 这些页由页 watcher（`page_manager.cpp:46-174`）覆盖。只有 user data 变化、读集合内的页被写入、或者进入新的 epoch 时，才重新走表。
   - 未变化时直接复用上次的 flat buffer 内容和解析结果，连 stream 拷贝也省掉。桌面报告提到“跳过未变化的 stream 拷贝约 11%”。
2. **sharp → 描述符槽。**
   - 全局描述符 heap：有 descriptor buffer 时直接写映射内存（vkd3d `resource.c:5766-5787,6615-6647`），否则用 update-after-bind 数组。
   - T# 按原始字加 image 集合代际缓存视图（现有 `texture_binds`，`vk_rasterizer.h:233-247`），视图在 heap 里占一个稳定槽位。
   - S# 去重成全局 sampler heap 的 16 位索引（DXVK sampler pool `dxvk/dxvk_sampler.cpp:534-582`，上限 2048）。
   - 每个 draw 只把槽位索引写进 push data 或 flat buffer（vkd3d 的 table 偏移，`command.c:7329-7389`）。
   - 不再每个 draw 推送整个描述符集合。
3. **动态 T# 数组改用 descriptor indexing。**
   - 现有 `DynamicImageTable` 最多快照 32 个 T#，展开成 32 路比较选择链，并且把有效位掩码放进特化 key（`resource_patching_pass.cpp:34-124,1046-1117`）。
   - 改为把快照写进 heap 的连续槽位，shader 以 `NonUniform` 索引访问（vkd3d SM 6.6 heap，`subprojects/dxil-spirv/opcodes/dxil/dxil_resources.cpp:822-830,1084-1144`）。
   - 这样既取消了 32 个的上限，也去掉了选择链和特化维度。
4. **buffer 绑定类型回到正确的 Vulkan 类型。**
   - 现状一律绑成 SSBO，有格式的读取降级为 raw 读取加 shader 内格式转换，于是 buffer 格式也进了特化 key（`lower_buffer_format_to_raw.cpp`，`specialization.h:79-269`）。
   - `s_buffer_load` 读取的小只读常量，绑成真正的 UBO（vkd3d 注明：在 NVIDIA 和 Qualcomm 上，push descriptor UBO 比 BDA 快很多，`state.c:7287-7294`）。
   - 有格式的读取，在格式为原生支持的 texel 格式时用 texel buffer，非原生格式才退回 raw 加转换。
   - 这一项要与 [GCN 模拟实现](../guides/gcn-emulation.md)第 6、9 节的语义要求（越界、特殊值）对齐后再做。

### 3.4 Shader 翻译与特化

1. **GCN 翻译移出 GpuComm。** 交给有优先级的翻译 worker：shader 首次出现就排队，被绑定时提升优先级（DXVK `registerShader`/`requestCompileShader`）。与 [Pipeline 编译与缓存](pipeline-compile-cache-20260926.md)的“自包含 job”约定一致。
2. **把 `RuntimeInfo` 中只影响 I/O 连接的部分改为链接期修正。** 包括 VS 输出映射、PS 输入/插值映射、导出格式、MRTZ 掩码。做法是 IR → SPIR-V 时按小的 linkage 结构修正，不重新翻译 GCN（DXVK `DxvkShaderLinkage`，`dxvk/dxvk_shader.h:220-235`；`getCode` `dxvk/dxvk_shader_ir.cpp:1676-1793`）。
3. **状态类特化用 spec constant + UBO 选择位**（DXVK D3D9 `d3d9/d3d9_spec_constants.h:136-354`）：优化版管线用 spec constant，GPL 基础管线读 UBO，一个库覆盖所有组合；未用到的 spec constant 清零（`dxvk/dxvk_context.cpp:6206-6228`）。
4. **GPL。** Turnip 和桌面支持 GPL 时，按顶点输入、pre-raster、片元、片元输出四部分构建库，绘制时快速链接，优化版异步替换。不支持时，按 pipeline 编译 spec 走有界后台编译。
5. **默认改回保留全部 draw 的准确模式。** `async_graphics_skip` 作为显式选项保留，并保留缺失内容跟踪。要等第 1–4 项把首次阻塞降下来之后，才改这个默认值。

### 3.5 Buffer 与 Guest 内存一致性

1. **arena 保留，并且一开始就提供非 sparse 回退。** sparse 已知在 Turnip/KGSL 上校验失败、Qualcomm 有 2 GiB arena 崩溃、Apple 有 4 GiB 限制（`buffer_cache/buffer_cache.cpp:254-329`）。BDA 页表改为按需分配，现状是启动时就无条件分配。
2. **`ObtainBuffer` 改为按代际判断（现占 23.8%）：**
   - 每个区域维护 CPU 脏代际和 GPU 修改代际，绑定时 O(1) 比较；代际没变就跳过 `SynchronizeMemory` 和 `IsRegionGpuModified` 的位图扫描。
   - 热路径不加锁（现状 `IsRegionGpuModified` 带 FutexMutex）。
   - 小只读常量的快照按（地址，大小，代际）复用，不再每次 memcpy（DXVK D3D9 常量上传只在变化时进行，`d3d9/d3d9_device.cpp:6155-6208`）。
3. **写故障开销（GXWorker 12–16%，Windows 上异常分发加 `VirtualProtect`）：**
   - 合并保护变更，减少 `VirtualProtect` 调用次数；
   - Linux/Android 优先用 userfaultfd（已有后端）；
   - 写故障预测已经做了（`region_manager.h:143-188`）；
   - 对确认为流式写入的 ring 不再保护，按“提交时快照”处理。
4. **回读改为按序号等待并下载。** 不再从缺页的 Guest 线程发起 `Finish()`（`buffer_cache.cpp:441`）。有了 3.6 的真实完成语义，等待的范围可以精确到写入它的那次提交。
5. **统一内存零拷贝（Android）。** PS4 的 CPU 与 GPU 本来就共享 GDDR5，Android SoC 也是统一内存。把 Guest 内存直接导入为 Vulkan 内存（`VK_EXT_external_memory_host`），arena 就是 Guest 内存本身，buffer 不再需要上传和写跟踪。前置条件是 3.6，否则 Guest 会在 host GPU 读完前覆写数据。现状没有任何地方这样做（`host_memory_policy.h` 只是 VMA 策略）。需要先在 Turnip 和 Adreno 闭源驱动上验证扩展、导入对齐与大小限制。

### 3.6 同步：以真实完成为准

1. **EOP/EOS/RELEASE_MEM 改为在真实完成时生效。**
   - 写入要求的 label 或 fence，可以由 GPU 在命令流里按顺序写进 arena，放在 pass 外（vkd3d `WriteBufferImmediate`，`command.c:18138-18218`），或者由完成线程在 timeline 到达后写 Guest 内存。
   - 中断在完成时触发（vkd3d fence worker，`command.c:617,442`）。
   - 取代现在“解析到就写 fence、发中断”的做法（`liverpool.cpp:839-876`）。
2. **默认打开回读。** 有了 1，GPU 写过的数据在完成之后就能对 CPU 可见。现状靠 `GpuByteKeeper`（`buffer_cache/gpu_byte_keeper.h:17-31`）和缺失内容跟踪兜底，它们因此可以从“正确性所必需”降级为优化手段。
3. **`WAIT_REG_MEM` 保留协作式协程加 `IdleBackoff`（这是做得好的部分，`liverpool.cpp:250-274`）。** 但如果等待的值要由 GPU 写入，就由完成回调唤醒，而不是轮询一块 GPU 还没写的 Guest 内存。
4. **帧节奏。**
   - 游戏可能依赖“过早完成的 EOP”来让 CPU 跑在前面。改成真实完成之后，必须同时保证有足够的帧在途：提交队列有界、帧延迟有上限、present 作为队列条目（DXVK frame 线程，`dxvk/dxvk_presenter.cpp:1277`）。
   - 释放时机（present 还是 GPU 完成）作为每游戏选项（xrgame 的 `dxvk-present-gpu-completion.patch` 先例）。
   - 这是本 spec 风险最高的一项，必须用 A/B/A 实测。
5. **异步 compute。** 现状所有 dispatch 都在图形队列的同一个命令缓冲里。可以评估独立 compute 队列加 timeline 依赖（vkd3d 每个物理队列一条 timeline，等待在提交线程里解决，`command.c:941,22437`），但默认保持单队列，直到有证据表明收益。

### 3.7 Render pass 与 barrier

1. **冒险跟踪扩展到 image 子资源。** 现有跟踪器只管 buffer。DXVK 用资源 ID 的高位编码（mip, layer）来覆盖 image（`dxvk/dxvk_barrier.h:104-260`，`dxvk/dxvk_image.h:743-768`）。
2. **image barrier 不再由 `Transit` 立即发出**，改为加入合批，与 buffer barrier 一起在冒险点统一发出。资源有默认的 stage/access/layout，只在超出默认时才 acquire/release（DXVK `dxvk/dxvk_context.cpp:8878,8990`）。
3. **布局。** 有 unified image layouts，或在 Adreno 上（vkd3d 注明 “Adreno hardware ignores layouts”，`resource.c:452-456`），sampled 纹理常驻 GENERAL。
4. **load/store op。**
   - 现状 storeOp 永远是 STORE，loadOp 只在快速 clear 时用 CLEAR（`vk_rasterizer.cpp:1259-1294`）。
   - 在 tiler 上按 DXVK 的做法，把 pass 主体录进 secondary 命令缓冲，结束时根据实际的附件访问改写 load/store op：没用到的降为 NONE/DONT_CARE，render area 收缩到实际绘制范围（`finalizeLoadStoreOps`，`dxvk/dxvk_context.cpp:2462`）。
   - HTILE/CMASK 元数据的“已清除”状态可以作为 load op 的依据。
5. **与上游 #5112 的 barrier interval set 对齐。** 本仓的同步记录里，它正在等 Swan/AYN 上的 GpuComm A/B。本节的目标结构是跟踪器加合批，不是区间集合本身，合入时再取舍。

### 3.8 PM4 解析

- **提交时把 DCB/CCB/ACB 拷贝成自有副本**（`copy_gpu_buffers` 和 owned submissions 已经存在）。默认用副本解析，`pm4_validate` 保留作诊断。代价是每次提交一次 memcpy。
- 这同时也是确定性回放的前提（第 4 节）。计算环自修改的检测（`liverpool.cpp:1178-1193`）需要在副本模型下重新验证。

### 3.9 桩、启发式与配置

- **补齐功能：**
  - `SET_PREDICATION` 用条件渲染实现；
  - occlusion query 用 Vulkan query 实现，结果经完成线程写回；
  - streamout flush 等待真实完成；
  - 实现 `COPY_DATA`。
- **启发式改为每游戏策略表。** 包括按代码字精确匹配的 compute HLE（`vk_rasterizer.cpp:1595-1635`）、魔术地址（`liverpool.cpp:879`）、NOP 尺寸提示。每条都带诊断，缺失时降级而不是 `UNREACHABLE` 或断言。
- **`emulator_settings` 的默认值是在拿正确性换速度：** 回读关闭、DMA 关闭、`async_graphics_skip`。改为显式的每游戏策略，会话启动时形成不可变快照（DXVK 各设备以 `const` 持有选项，`d3d11/d3d11_device.h:503`）。

---

## 4. 验证：确定性回放

Citron 已实现第一版确定性帧回放，并在 BOTW 上实测：完整场景可以在不加载游戏的进程里回放，第 0 帧逐像素一致（`D:\workspace\citron\docs\plans\20261004-gpu-deterministic-replay.md`）。

那份文档的 §9 评估了它在本仓的可用性，结论是**核心思路适用**：
- 回放内存状态，而不是读取记录；
- 在 PM4 线程的边界记录；
- 按物理页存储；
- 用不加载游戏的进程回放，逐帧哈希。

本仓已有的 PM4 trace（[操作入口](../debugbus-gpu-command-trace.md)）记录了实际消费的 packet、shader 代码和 flip，可以作为事件流的基础。但它明确只读命令字本身（`pm4_trace.h:23-24`），不含内存镜像、CPU 写增量和调度日志。

与 Citron 相比，要重新设计四处：

1. **起始状态要强制写回 GPU 独有的结果。**
   - readbacks 默认关闭，渲染目标和 GPU 写过的 buffer 只在 host cache 里。
   - 快照前用 `BufferCache::DownloadMemory`、`TextureCache::RecordImageDownload` 写回，还要另外处理 CMask/FMask/HTile 元数据、GDS 和内部缩放过的图像。
   - 快照点选在 `IsGpuIdle()`：没有存活的协程帧，并暂停 Guest 线程。
   - 另需保存：`Regs`（208 KiB）、每个计算队列的 `cs_state`、CE RAM、`asc_queues` 与读指针、GnmDriver 静态量、VideoOut 端口状态；挂起的 EOP-flip 中断要从闭包改成数据。
2. **录制器自己的写 watch 层。**
   - Guest 代码原生执行，页面不能常驻保护，否则同一条 store 会无限故障。
   - 做法：故障时记下脏页、撤销保护、放行写入；每个边界复制脏页后重新保护。
   - 接入 `GuestFaultSignalHandler`、userfaultfd、FEX `gpu_pages` 三个入口，并过滤 GPU 线程自己的写。
   - 命令缓冲、SRT 表、shader 代码和 label 原本没有任何 watch，要一并覆盖。
3. **按 resume 记录调度。**
   - 边界设在每次 `task.resume()` 之前。
   - 记录 resume 序列、每次等待的结果（WAIT_REG_MEM、MEM_SEMAPHORE、REWIND、CE 计数器）和 `SendCommand` 注入的位置（CPU flip、readback、VR 帧）。
   - 回放按记录的调度执行，而不是让协程自由调度。
   - REWIND、计算 DMA 改写 dispatch、`PatchFlipRequest` 都是自修改命令流，所以每次 resume 前都要刷新增量。
4. **固定 VA 与 host 指针。**
   - GPU VA = CPU VA = host VA，回放进程预留同样的固定地址，按物理偏移恢复 8448 MB 后备里的页（允许别名）。
   - GPU 可达的是所有映射在 2^40 以下的内存，快照会有数 GB。
   - 经典模式的 PM4 里嵌着 host 指针（初始化序列、内嵌 shader、VideoOut label 数组），要么重定位，要么按 host runtime 的布局捕获。

**回放时必须固定的启发式：**
- `pipeline_compile_mode` 改为同步，否则 `async_graphics_skip` 会丢 draw；
- 固定 readbacks 模式；
- 100% 内部分辨率；
- 关闭写故障预测和 GpuByteKeeper（它们让 cache 内容依赖故障时机）；
- 关闭 GC 写回和 deferred-ops 线程的异步提交；
- 时间戳改为确定的值；
- VideoOut label 改为按记录的位置复位。

**可行性先例**：测试 `tests/video_core/android_dma_residency_probe.cpp:120-140` 已经在不加载游戏的情况下建出 `MemoryManager`、`Liverpool`、`Rasterizer`，回放进程可以以它为基础。

**与 Citron 共用**（适合放进 Foundation）：
- 文件容器：带长度的记录、LZ4、后台写线程与背压；
- 页增量编码；
- 比较器：逐帧哈希加容差判定；
- RenderDoc 抓帧对比的流程。

状态段、事件类型、内存模型和写跟踪各自实现。

---

## 5. 分阶段实施

| 阶段 | 内容 | 验收 |
|---|---|---|
| A 地基 | 回放捕获与无头回放；PM4 自有副本；会话级不可变配置；真实完成的 EOP/EOS（第 3.6 节第 1–2 项）与完成线程 | 回放重现血源中央亚楠一段，逐帧一致；回读默认打开后画面正确；帧率与帧间隔按 A/B/A 报告（允许下降，但要有原因） |
| B 后端上下文 | 类型化命令；录制线程承担描述符、管线变体、动态状态、pass、barrier；init 命令缓冲；draw 合并 | GpuComm 上的 Vulkan 相关工作移出；Thor 上 GPU 忙碌 × 频率下降；回放一致 |
| C 状态模型 | 寄存器脏位图；管线 key 增量维护与直接映射缓存；`RuntimeInfo` 与动态状态按脏组重建；`StageSpecialization` 记忆化（附输入审计） | `GetGraphicsPipeline`（14.2%）降到 3% 以下 |
| D 资源模型 | SRT 走表按代际门控；sharp → 稳定 heap 槽 + push 索引；动态 T# 改用 descriptor indexing；UBO 与 texel buffer；`ObtainBuffer` 改为代际判断；非 sparse 回退；回读按序号等待；Android 零拷贝（依赖 A） | `BindResources` 加 `ObtainBuffer`（合计约 62%）降到 20% 以下；Android 每个 draw 从约 18.6 µs 降到 8 µs 以下 |
| E shader 与管线 | 翻译 worker；链接期修正；spec constant 加选择位；GPL；默认改回准确模式（与 pipeline 编译 spec 合并实施） | 首次运行卡顿（固定路线 p99）改善；准确模式下帧率不低于现在的跳过模式 |
| F 提交与节奏 | 有界队列、帧延迟上限、释放时机作为每游戏选项、可选异步 compute 队列 | 帧间隔方差不变差；延迟可观测 |

依赖关系：

- A 是一切验收的基础。
- B 的 barrier 改动要求资源销毁以完成为准（A 中的完成线程）。
- D 的零拷贝和回读改造依赖 A 的真实完成语义。
- E 与 B、C、D 可以并行。

**收益估算**（需逐阶段实测）：GpuComm 每帧 CPU 时间有望降到现在的一半以下。桌面上，GPU 已经是另一个限制（3D 引擎 65–75%），收益主要表现为余量；在 Android 上，GpuComm 是 Guest-1 之后的下一个瓶颈，收益直接相关。

## 6. 风险与待定问题

- **真实完成会改变节奏。** 依赖过早 EOP 的游戏可能变慢，要靠在途帧数和 present 条目化来补；只有 A/B/A 数据才能下结论。
- **零拷贝的可行性**取决于各驱动对 `VK_EXT_external_memory_host` 的支持、导入粒度，以及 FEX 进程内 Guest 内存的分配方式。
- **按代际门控 SRT 的正确性**依赖读集合完整，包括 SRT 程序里的间接读取。需要在回放中与“每个 draw 都走表”逐 draw 对比。
- **与上游的分歧。** 本 spec 的若干改动（后端上下文、同步语义）会让 fork 与上游分叉更大。每阶段完成后，评估向上游提交或维持 fork 的成本。
- **与 Citron 共用组件。** 回放与比较器、冒险跟踪器、命令 arena、描述符 heap 管理都适合放进 Foundation，由两边共用。抽取之前，两边的接口需要先各自稳定一轮。

## 7. 进展

| 日期 | 阶段 | 内容 | 记录 |
|---|---|---|---|
| 2026-10-04 | A | 回放捕获与无头回放，两次回放逐帧、逐事件图像哈希一致（修复 alpha-test 后按像素分支的隐式 LOD） | [gpu-replay-20261004](../validation/android-native-host/gpu-replay-20261004.md) |
| 2026-10-04 | A | PM4 自有副本桌面默认开；EOP/EOS/RELEASE_MEM 进有序队列，GPU 完成后由完成线程执行，flip 完成排在其后；桌面默认开，Android 默认关 | [gcn-stage-a-fences-20261004](../validation/android-native-host/gcn-stage-a-fences-20261004.md) |
| 2026-10-05 | C | 排列匹配边读边比（`Matches`）、binding 起点与 fetch shader 预检、program 级 fetch shader 缓存、GPU 修改查询按区域标志跳过：`GetProgram` 约 12% → 9%，`GetGraphicsPipeline` 约 14% → 11.8%，未达 3% | [gcn-stage-c-permutation-match-20261005](../validation/android-native-host/gcn-stage-c-permutation-match-20261005.md) |
| 2026-10-05 | D | 流式页（3.5 第 3 项）：按代复制会读到过期副本，改为每次绑定比较内容哈希；写缺页 −70%，但哈希抵消写保护的节省，桌面无净收益，默认关；回放改为按写缺页通知缓存 | [gcn-stage-d-stream-pages-20261005](../validation/android-native-host/gcn-stage-d-stream-pages-20261005.md) |

3.6 第 1 项的实测结论：CP 先行解析并用待定值通过 `WAIT_REG_MEM` 时，等待之后立即可见的写（`WRITE_DATA`、信号量）必须排在等待所依赖的 fence 之后，否则游戏会按这条写回收仍有 label 待写的内存（血源中央亚楠必现崩溃）。现在的做法是在这类写之前由 CP 提前执行这些 fence；血源每帧末尾都有这种写，99.7% 以上的 fence 因此提前执行。要得到真实完成的时机，CP 需要真正等待 GPU，这依赖阶段 B/C 先把 GpuComm 的录制工作移走。3.6 第 2 项（回读默认打开）尚未开始：开之前，提前执行遇到未完成的异步下载时要改为真实等待。
