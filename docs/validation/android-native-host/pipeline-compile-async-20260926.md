# Pipeline 编译 P1a：有界 worker 与准确异步（2026-09-26）

分支 `feature/malos/mhw_fix`，本地未提交。接续 [P0 驱动 cache 持久化](pipeline-cache-p0-20260926.md)，对应远端 spec `docs/specs/pipeline-compile-cache-20260926.md`（`origin/malos/main` `14306f95`）的 P1a。可选的 graphics skip（P1b）与计算 HLE 前置见 [pipeline-skip-hle-first-20260926.md](pipeline-skip-hle-first-20260926.md)。

## 1. 结构

| 部分 | 实现 |
| --- | --- |
| 接口与原生对象分开 | 图形/计算管线构造函数仍在 GpuComm 上完成所有读取 guest 状态的工作（descriptor/pipeline layout、顶点输入、辅助 TCS/TES/丢弃 FS 的 SPIR-V、多重采样、附件格式、混合、动态状态）。`vkCreate*Pipelines` 读取的全部内容放进管线自有、堆上且不再移动的 `CreateState`（含 per-stage 结构、pNext 链、辅助 shader module 所有权、creation feedback、失败诊断所需的 FS runtime 副本），只剩 `CreateNative()` 一个驱动调用。`BindResources` 只用 layout，因此不受影响。 |
| 构建状态 | `Pipeline` 基类：`Pending → Building → Ready`。`TryClaim()` 以 CAS 认领；`BuildClaimed()` 完成创建、调用观察者（统计/日志/驱动 cache 保存计数），最后在持锁时发布 Ready 并 notify（等待方返回后发布方不再访问对象）。 |
| 绑定 | 四处 `bindPipeline(pipeline->Handle())` 改为 `pipeline->Bind(cmdbuf, point)`：已就绪直接录 `bindPipeline`；未就绪录一条 `Custom` 命令，**执行时**才调用 `WaitHandle()` 取句柄，不会把空句柄捕获进命令。开录制线程时由 VkRecord 等，不开时 `Custom` 立即执行、由 GpuComm 等。 |
| 首个使用者插队 | `WaitHandle()` 若发现任务还在排队（未被认领），当前线程直接认领并自己创建，不排在其它任务之后；只有已在 worker 上构建的才等待。worker 只调用驱动，不碰 scheduler、guest 内存和缓存锁，等待链不会成环。 |
| `PipelineCompiler` | 固定 worker（`shadPS4:PipelineCompile`）：Android 2 个，桌面 `核数/4` 取 1–4，高通专有驱动 1 个；`Vulkan.pipeline_compile_workers` 非 0 时覆盖。队列上限 64，满了由提交方当场创建（与同步模式相同，spec 的背压）。高通专有驱动下所有 guest 管线创建另经一个全局互斥串行化（同 citron 不信任其并发创建）。 |
| 生命周期 | 析构 `PipelineCache` 时先 `Sync()` 再 `Stop()`：清空队列、join 正在构建的 worker；尚未开始的管线保持 Pending（若之后还有人用，由使用者自己建）。`compiler` 成员声明在管线 map 之后，先于管线销毁。调试用 `ReplaceShader` 删除管线前 `Forget()`：从队列移除，并等到没有 worker 手上还拿着它，再 `WaitHandle()`，避免释放后访问。 |
| 不丢命令 | 所有 draw/dispatch 按原顺序保留，未就绪只是等待；没有 skip、没有替身管线。驱动创建失败仍与同步模式一样 ASSERT，并写 `failed-graphics-pipeline.json`。 |
| 设置 | `Vulkan.pipeline_compile_mode = sync（默认）| async_accurate`，`Vulkan.pipeline_compile_workers`（0 = 设备默认）。DebugBus `pipeline_cache mode sync|async_accurate|config` 运行时切换，只影响之后新建的管线。Android 设置页 GPU 下新增“Pipeline Compilation”（sync / async_accurate，默认 sync，经 JNI `nativeSetPipelineCompileMode` 传入）。`pipeline_cache status` 增加：实际模式与是否被覆盖、线程数、延后的图形/计算数、入队数与峰值深度、队列满次数、由 worker / 首个使用者构建的数量、绑定等待次数与时长；新 scope `Pipeline.WaitReady`，counter `Pipeline.CompileQueue`。 |

顺带修复两处：`vk_pipeline_cache.h` 把 `AmdGpu::Liverpool` 前向声明为 `class`，而定义是 `struct`。MSVC 名字修饰区分二者，新增源文件使 unity 分批重排后，`Presenter` 构造函数在两个编译单元里的修饰名不一致，导致链接失败；已改为 `struct`。驱动 cache 文件名加入 vendor/device ID（`<serial>_<vendor>_<device>.vkpipelines`），桌面在独显/核显之间切换时不再互相作废。

## 2. 桌面验证（血源，recipe 缓存关、驱动 cache 开，`pipeline_cache mode async_accurate`）

这一轮 Vulkan 设备实际是 **Radeon 890M 核显**（device `0x150e`，驱动 `0x80018b`），不是前几轮的 7600M XT，原因未查。流程均为：启动 → “未经退出游戏结束”提示 → 标题 → 离线游玩 → 继续，进入亚楠街道；截图画面、HUD、角色完整。

| 轮次 | 条件 | 新管线 | 驱动创建总耗时 | 绑定处实际等待 | 其它 |
| --- | --- | --- | --- | --- | --- |
| A | 开录制线程；AMD 驱动内部缓存对该核显为冷 | 184 图形 + 31 计算，全部延后 | 11.23 s（图形均 55.5 ms，最长 179 ms） | 113 次，3.68 s（均 32.6 ms，最长 442 ms） | 214 条由 worker、1 条由首个使用者构建；队列峰值 18，未满 |
| B | 不开录制线程；AMD 内部缓存已热 | 同一组 215 条 | 1.85 s（图形均 7.4 ms） | 177 次，1.69 s | 203 由 worker、12 由 GpuComm 自己认领；峰值深度 1 |

- A：约 2/3 的驱动编译时间与 GPU 线程其余工作、以及 4 个 worker 之间的并行重叠掉了。
- B：GpuComm 请求后很快就要绑定，几乎全部暴露，与同步模式相当，符合预期。**准确异步必须配合录制线程**（`vk_recorder on`，目前默认关）才有明显收益。
- 两轮的绝对数字不可直接比较：AMD 驱动有自己的磁盘 shader 缓存，第二次运行自然更快。桌面也无法做干净的同步/异步 A/B。
- tiling 管线仍同步创建，A 轮 13 条共 355 ms 落在 GpuComm 上。
- 测试后桌面 `config.json` 未改动（SHA `274611bc…`，模式只经 DebugBus 覆盖）；删除了两份按旧命名、由测试产生的驱动 cache 文件。

## 3. 未做 / 边界

- **未在 AYN/Turnip 上运行**（设备未连接）。host `HOST_LINK_PASS`、APK 编译通过，Kotlin 设置测试 PipelineCache 5/0 等均过。真实收益需在 Turnip 上以“recipe 与驱动 cache 均冷”并开录制线程做同场景 A/B；Turnip 没有驱动自带磁盘缓存，结果比桌面干净。
- 录制线程在提交、`RawCommandBuffer()` 时排空，所以可隐藏的只是“请求到下一次排空”之间的工作（spec §2 所述）。跨提交继续解码是 P3。
- GCN→SPIR-V 翻译仍在 GpuComm 上（本轮核显冷启动 282 个共 0.9 s，最长 160 ms）；status 里的等待只是原生创建等待。
- tiling 管线与其它 host 管线仍同步；计算 HLE 前置已在后续补上。
- 没有 Failed/Cancelled 状态：驱动失败仍 ASSERT；停止时排队任务保持 Pending 而非取消。
- 默认模式保持 `sync`（spec：切换默认值需单独验收）。
