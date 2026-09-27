# Pipeline 缓存设备补测、跳过模式的缺失内容传播、每 draw 管线查找开销、手柄震动（2026-09-27）

分支 `feature/malos/mhw_fix`，本地未提交。设备 AYANEO Pocket DS（adb `01108YHE01017563`，SoC SG8275，Turnip mainline `86ca472fc2`，驱动报告 Adreno 740），血源 CUSA03023（ZAR 31,143,442,634 B 推送后大小一致；存档 CUSA01363 从桌面运行目录复制）。前序：[P0](pipeline-cache-p0-20260926.md)、[P1a](pipeline-compile-async-20260926.md)、[P1b](pipeline-skip-hle-first-20260926.md)、[P2](pipeline-preload-tiers-20260927.md)。

## 1. 设备补测（Turnip 首次实测）

同一存档、同一路线（标题 → 继续 → 大教堂区灯下），各轮到达场景后读 `pipeline_cache status`：

| 轮次 | 条件 | 结果 |
| --- | --- | --- |
| 1 冷 | 删除缓存，`sync` | 翻译 370 个 shader 1045 ms；graphics 256 条 3647 ms（均值 14.2 ms，最长 134 ms），compute 32 条 1070 ms（最长 391 ms），tiling 13 条 168 ms，全部在 GPU 命令线程上同步完成；驱动缓存保存 7 次，3.2 MB |
| 2 暖 | 同上，重启会话 | 启动预载 288 条共 287.5 ms（4 线程；读 recipe/module/layout 282 ms，驱动创建 graphics 256 条 7.7 ms、compute 32 条 0.8 ms，全部驱动缓存命中）；进入场景后运行中新建 0 条，只翻译 4 个 shader |
| 3 冷 | 删除缓存，`async_graphics_skip` + 录制线程 | 185 graphics + 27 compute 全部延后构建（worker 210 条、首个使用者 2 条，队列峰值 13）；绑定处等待 101 次共 1635 ms（最长 349 ms）；28 个 draw 被跳过（22 条管线，15 帧，最长连续 2 帧），第 1138 帧因缺失内容跨帧被读而自动切回等待（见 §2） |

- Turnip 在冷缓存时对 graphics 管线也报告 `APPLICATION_PIPELINE_CACHE_HIT`（256/256），compute 冷时 0/32、暖时 32/32。graphics 的冷命中来自 Turnip 在同一 `VkPipelineCache` 内按 shader 变体复用，不代表磁盘缓存；暖轮真正的差别是创建耗时 3647 ms → 7.7 ms。
- 轮 1 的约 4.7 s 管线创建全在 GPU 命令线程上；轮 3 同一段内容在该线程上等待 1.6 s，其余由 worker 完成。两轮进入场景的操作相同但不是严格的帧对帧 A/B。
- 缓存按精确路径删除（`files/host/cache/CUSA03023_5143_43050a00.vkpipelines` 与目录 `files/host/cache/CUSA03023`），模式经 DebugBus 覆盖，未改设备设置。

## 2. 跳过模式的缺失内容传播与自动切回（spec §4）

新增 `Vulkan::MissingContent`（`vk_missing_content.{h,cpp}`）：以 guest 地址区间记录“内容不完整”，跟随 GPU 工作传播。

- **标记**：跳过的 draw 按寄存器计算它本应写入的颜色附件（`slice_size × slices`）、深度平面、写模板时的模板平面区间（不查找或创建图像）。
- **动作**：每个 draw/dispatch/DMA 拷贝是一个动作。读取（采样/存储图像、`ObtainBuffer` 读、深度/模板测试、混合读目标、间接参数、拷贝源）若碰到已标记区间，本动作的全部写入（附件、存储资源、拷贝目的）都被标记，来源帧取最早者。深度/模板“测试”只在比较函数不是 Always 时算读，模板“写”要求写掩码非 0 且操作不全是 Keep。
- **解除**：只在证明整块覆盖且不读旧值时解除——整图 `Image::Clear`、渲染区覆盖整张单层单级附件的 clear load、拷贝未标记的源（DMA `CopyBuffer`、compute 整图拷贝 HLE）、`FillBuffer`。普通 draw、部分 scissor、混合一律不能解除。
- **切回等待**：标记内容进入 CPU 回读（`BufferCache::ReadMemory`、`ProcessDownloadImages`、copy HLE 把结果写回 guest 内存）、间接 draw/dispatch 参数、或在比丢失更晚的帧被读时，本会话关闭跳过（`skipping off (missing content …)`），之后未就绪管线在绑定处等待并由绑定线程自建。已丢的内容不补回，状态中仍列出。
- 关闭跳过后不再跟踪（标记冻结供状态查看，动作不再付查询开销）；未开跳过时每个动作只多一次原子读。
- `pipeline_cache status` 增加：区间数/字节、跳过标记数、传播次数、整块解除次数、各出口首次地址与帧、最大的 16 个区间。

实测（桌面 890M 与设备一致）：血源 logo 阶段第 2 帧跳过了一个只写模板的 draw（模板平面 2304 KiB），之后没有任何整块清除该平面，数百帧后一个模板测试读到它，判为跨帧反馈而切回等待（桌面第 523 帧，设备第 1138 帧）。这是规则下的保守结果：普通 draw 的完整覆盖无法证明。修正前的版本把模板测试一律当读、模板启用一律当写，桌面第 3 帧就切回；按比较函数/写掩码/操作精确判断后推迟到数百帧。

## 3. 每 draw 管线查找开销（第 1 项）

设备 simpleperf（`--call-graph fp`，20 s，116,726 样本 0 丢失，暖缓存游玩，GpuComm 线程占全部 11.5%），GpuComm 线程内：

| 项 | 占 GpuComm |
| --- | --- |
| `GetGraphicsPipeline` | 21.6% |
| └ `GetProgram` | 18.5% |
| 　└ `RefreshFlatBuf`（SRT 扁平化） | 11.1%（`PortableSrt::Run` 8.8%：自身 1.8%、`SrtReadBatch::Read` 2.3%、`Compute` 1.0%） |
| 　└ `StageSpecialization` 构造 | 4.3% |
| 　└ 排列查找（`ranges::find` + `operator==` + `RuntimeInfo==`） | 约 2.6% |
| 对照：`CopySparseMemory`（小 UBO 走 stream 拷贝，持 MemoryManager 锁） | 9.4%，其中 `mutex::unlock` 的原子交换 8.4%（锁争用），不属于管线查找 |

修改：

- `PortableSrt::Run`：sharp 在表内是连续常量偏移、写入连续 flat 槽的逐 dword Copy。把这样一段合并为一次读取（最多 64 dword）；读失败时逐 dword 回退，结果与逐个读取相同（不可读 dword 为 0）；任何其他命令执行前先写出这一段（其表达式可能读 flat）。
- `SrtReadBatch::Read` / `TryReadSrtMemory`：单次上限 8 字节 → 256 字节（窗口内一次 memcpy；跨段仍回退逐次路径）。
- `FetchShaderData::attributes`：`std::vector` → 内联 16 项的 `small_vector`。每个 draw 在特化、取程序和刷新 key 时各拷贝一次，原来每次都堆分配；序列化格式不变。

测试：`srt_tests` 新增批量段、回退、段内单个不可读 dword、空表指针与“flat 依赖必须先写出”的检查；设备上 486 项 0 失败（进程退出时 `SaveDialogUi` 静态析构碰到已析构的 ImGui 层互斥量而 abort，与本改动无关，已另列任务）。

同一站位、同一设置（`async_graphics_skip` + 录制线程，暖缓存）重新采样 20 s（115,803 样本 0 丢失），GpuComm 线程内：

| 项 | 优化前 | 优化后 | 按样本数 |
| --- | --- | --- | --- |
| `GetGraphicsPipeline` | 21.6% | 17.4% | |
| `GetProgram` | 18.5% | 14.7% | 1692 → 1262（−25%） |
| `RefreshFlatBuf` | 11.1% | 6.9% | 1011 → 594（−41%） |
| `PortableSrt::Run` | 8.8% | 5.0% | |
| `SrtReadBatch::Read` | 2.3% | 1.3% | |
| `operator new` | 1.4% | 0.5% | |

- `srt_batch status`：每次扁平化平均 1.5 次读取（取表指针 + 一整段），0 次回退；`srt_batch verify on` 10 s 内逐次路径复核 710,938 次读取，0 次不一致。
- 两次采样是不同会话，不是严格 A/B；画面与游戏正常。
- `Run` 剩余：每次运行一次窗口解析 `ResolveSrtWindow`（1.1%，批量对象按次创建）、`thread_local` 备忘缓冲的动态 TLS 解析（0.4%）、`StageSpecialization` 构造 4.0%（未改）。

## 4. 手柄震动

用户报告两个平台都没有震动。

- 新增统计：`pad_vibration status | test LARGE SMALL [HANDLE]`（DebugBus，桌面与 Android 都注册），按结果计数（送达/无句柄/未连接/无振子/宿主拒绝），前 8 次与结果变化写日志；Android `NativePad` 日志记录手柄注册时的 rumble 能力与振子数。
- 桌面：`SDL_RumbleGamepad` 的低频（大马达）与高频（小马达）参数传反了——游戏的主效果多在大马达，结果只驱动小马达，手感很弱。已改为 large→低频、small→高频，并记录 SDL 失败原因。
- Android（Pocket DS）：“AYANEO Controller” 注册 `rumble=true, vibrators=2`。血源每帧调用 `scePadSetVibration`；空挥、开枪、菜单调节振动强度都只发 0/0（游戏本身不在这些时候震动）；被敌人击中时出现非零值（该局 83 次），全部送达手柄振子，用户确认手感。Android 链路本身无需修改。
- 逆向核对（eboot，§5 工具）：`0x269cca0` 设备 SetVibration ← 每帧播放器更新 `0xf95eb0`（读本帧累积强度、发送、清零）← 强度只由 `0xf91c10` 写入 ← 混合器 `0x1571810` 消费震动请求队列（`[mgr+0x4010]`）← TAE/事件生产者（如 `0x1a256c2`）。空挥时队列从未入队；游戏内 Controller Vibration 选项为 10。

## 5. 工具

- `tools/ps4-guest-code/ps4_guest_code.py`：`elf` 生成的分析 ELF 现在应用模块自身重定位（RELATIVE 与自定义符号，基址 0）并按 NID 命名导入（`__imp_<name>` GOT 槽、`<name>` PLT 桩）；新增 `imports [--calls]`、`xrefs [--data]`。血源 eboot：231,468 条重定位、3,522 个导入槽、661 个 PLT 桩，1.5 s。新增 skill `.claude/skills/ps4-ida-analysis`（IDA 打不开原始 PS4 ELF 的原因、reverse_study 大文件超时、运行时核对）。
- native-debugger MCP 的 `permission_denied` 来自缺少 `%APPDATA%\SpatialDebugTool\McpPermissions.yaml`；已按最小权限创建（端口转发、调试控制、内存读取）。

## 6. 默认开启跳过模式与录制线程

用户要求默认打开。此前 `Vulkan.pipeline_compile_mode` 默认 `sync`，录制线程没有设置项、只能用 DebugBus `vk_recorder on` 临时打开。

- 核心设置：`pipeline_compile_mode` 默认 `async_graphics_skip`；新增 `Vulkan.command_recorder`（默认 true，可按游戏覆盖），`CommandRecorder` 构造时读取，DebugBus `vk_recorder on|off` 仍可运行时切换。
- Android：设置目录 `android.json` 与 `shadps4.json` 同步改默认值并新增“Recording Thread”项；`PipelineCache.resolveCompileMode` 无配置时返回 2，新增 `resolveRecorder` 与 JNI `nativeSetCommandRecorder`；`PipelineCacheTest` 改为断言新默认值并覆盖录制线程的全局/游戏覆盖。
- 桌面运行目录 `config.json` 与设备 profile 均未显式保存这两项，新默认值直接生效。

## 7. 纹理上传卡顿

### 7.1 数据

同一存档在大教堂区沿路行走（DebugBus 手柄，前进+转向 13 轮），`upload_diag` 记录 400 次上传：

- 几乎全部是**新纹理首次上传**（流式加载），不是重复上传：291 次统计中 first 241、GPU 驻留 47、真正重传 3（同一张 1024² R8 图被 CPU 写）。StatusLayer 原来把所有上传都叫“Re-uploads”，误导。
- 上传集中成突发：第 9533–9557 帧 316 次/88.8 MiB，单帧最多 42 次/14.4 MiB；格式 BC1/BC4/BC7 为主，tile mode 13，全部走计算 detile。
- 突发期间按日志时间戳估算帧时间平均 71 ms（平时 67 ms），个别帧 140 ms；最重一帧 42 次上传在 GPU 命令线程上约 28 ms（含日志）。同期 22 s LiteTrace（`trace_dump` 解析，Litep MCP 仍不认 SDK 线格式）恰好落在突发之前，未包含 `Texture.*` 区段。

### 7.2 每次上传的固定开销（代码核对）

- `TileManager::DetileImage` 每次 `vmaCreateBuffer` 一个与纹理等大的设备本地临时缓冲，GPU 完成后销毁（VMA 诊断账本加锁、建/毁 VkBuffer）；`gpu_memory` 中 `scratch/detile-tile` 分组峰值 264 MiB 且全部处于待退役状态——突发时临时缓冲堆积，VMA 需要新建设备内存块，空了又释放，下次突发再建。
- `RefreshImage` 与 `Image::UploadRegions` 先 `EndRendering(ImageUpload)` 结束当前 pass；上传后图像处于 `eGeneral`，随后绑定纹理时转换到 `eShaderReadOnlyOptimal` 又以 `SampledImage` 结束 pass。流式纹理恰好在 draw 绑定时首次上传，此时上一个 pass 仍开着，每张新纹理打断一次 pass（tile store + 重开 load）。

### 7.3 修改

- detile 输出改用 `StagingBufferPool` 的设备本地环（16 MiB 块，GPU 完成后复用，>16 MiB 用可复用的专用缓冲，闲置 300 帧才释放），不再逐次分配。
- 上传放到当前（被录制线程持有的）pass 之前：pass 已记录的访问与图像内存不冲突时，暂存、detile、拷贝和布局转换都进 pre-pass，不结束 pass；源来自 buffer cache（GPU 写过）时按“读且可能先写”检查。冲突、录制线程关闭或访问集溢出时保持原来的结束 pass 路径。
- `Image::Transit`：pass 未读写过的图像（按 guest 范围判断，别名同样冲突）的布局转换放到 pass 之前，覆盖上传后到采样布局的那次转换。
- MSAA 辅助 pass（`BlitHelper` 两处）不登记访问，开 pass 后标记为不可跟踪，禁止任何操作提前到它之前；`Scheduler::BeginRendering` 在提前操作内被调用时先结束持有的 pass，避免嵌套。
- 计数：`gpu_memory` 的 `texture_uploads` 增加 `new`/`new_bytes`/`reuploads`/`upload_ms`（GPU 命令线程在 `RefreshImage` 中的耗时）；StatusLayer 分为“Texture uploads N/帧 (MiB, ms)”与“Re-uploads”，橙色与提示只看真正的重传。

### 7.4 设置面板

Graphics 页原来直接显示 `sync / async_accurate / async_graphics_skip`。`RuntimeSettingSpec` 新增可选 `choiceLabels`（与 `choices` 一一对应，存储值不变，只影响显示；非枚举或数量不符时拒绝加载）：

- “Shader Compilation”：Wait / Background / Skip until ready（默认），说明写明各自代价（卡顿 / 物体稍晚出现）。
- “Recording Thread”：On（默认）/ Off，说明它提高帧率并让后两种编译方式能藏住卡顿，只在画面异常时关闭。

设备截图确认两项显示正确且默认值高亮。设置与配置测试 47/0。

### 7.5 设备验证（Pocket DS，同一存档与路线）

新 APK 安装后直接从库启动（未用 DebugBus 覆盖任何模式）：

- `pipeline_cache status`：`async_graphics_skip (configured async_graphics_skip)`；`vk_recorder status` chunks/held_passes 持续增长，`pass_hoist unavailable=0`，即录制线程默认在工作。本次暖缓存下仅 1 个 draw 被跳过，之后因缺失内容跨帧被读而本会话切回等待（§2 的保守规则）。
- 行走 13 轮前后：上传 +545 次（新纹理 +537，重传 +8）、+206 MiB，GPU 命令线程在 `RefreshImage` 中合计 **27 ms**（约 0.05 ms/次）。加载阶段 1841 次上传共 484 ms（含暂存环首次扩容）。改动前同类突发按日志时间估算约 0.6 ms/次（含日志开销，非严格 A/B）。
- pass 打断：整个会话 158,962 次 pass 中 `image_upload` 打断 3 次、因其重开 3 次；`sampled_image` 重开 71 次。改动前会话为 1,420,525 次 pass 中 `image_upload` 打断 432 次、重开 400 次，`sampled_image` 重开 21,673 次（按 pass 数折算约为原来的 1/30）。
- `scratch/detile-tile` VMA 分组不再出现；detile 输出走 `buffer/DeviceLocal`（暂存环，当前 6 块 200 MiB）。
- 画面：行走后静止截图纹理与场景正常，无错乱。会话随后 UIStop，`user_stop`、guest return=0。
- 清理：上一轮 45 MB 采集文件及其 `.json` 已按精确路径删除；该轮会话在设备断开期间已退出，`upload_diag` 随进程结束。

## 8. 未做

- 跳过模式下普通 draw 的整块覆盖证明（例如全屏三角形），以及更细的模板/深度平面覆盖判定；当前保守规则使血源在数百帧内切回等待。
- 间接 draw 的参数本身是否被标记由读取路径覆盖，但 predication 读取未接入。
- 第 1 项之外的 GpuComm 热点（stream 拷贝的 MemoryManager 锁争用、`BindTextures` 的 `FindImage`）。
