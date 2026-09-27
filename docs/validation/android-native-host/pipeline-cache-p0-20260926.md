# Pipeline 缓存 P0：驱动 PSO cache 持久化、预载修正与测量（2026-09-26）

分支 `feature/malos/mhw_fix`（基于 `malos/main` `b4972cbd`），本地未提交。对应远端主干新 spec [pipeline-compile-cache-20260926.md](../../specs/pipeline-compile-cache-20260926.md)（`origin/malos/main` `14306f95`，本分支尚未合入该提交）的 P0，并提前做了 P2 中的并行预载。P1a（有界编译 worker、准确异步）见 [pipeline-compile-async-20260926.md](pipeline-compile-async-20260926.md)；可选 graphics 跳过未做。

## 1. 修改前的问题（源码核对）

- Android 上两层缓存都没有生效：`pipeline_cache_enabled` native 默认 false，Android 路径不调用 `EmulatorSettings.Load()`，Kotlin 的 `applyAndroidCompatibilityProfile` 只有测试调用。每次运行都重新翻译、重新让驱动编译。
- `PipelineCache` 构造时 `WarmUp()` 在创建 VkPipelineCache **之前**，预载的管线拿到的是空句柄；VkPipelineCache 从未保存（全仓没有 `getPipelineCacheData`）。
- `cache_storage` 的 IO 线程：`num_requests` 在锁外递减（数据竞争）；`Close()` 请求停止后直接退出，队列中尚未写的 blob 丢失；松散文件直接覆盖写，进程被杀会留下半个文件，而读取端 `Serialization::Reader` 越界即 ASSERT。
- profile 不一致（换驱动、改 Render Scale/MSAA）时关闭整个 store，本次会话不再缓存，且旧文件一直留着，下次仍不一致——缓存实际永久失效。
- 预载中某条管线的后续 stage 读取失败时，前面 stage 的 `infos/modules` 不清空，会串到下一条管线上。
- 图形管线的辅助 TCS/TES/丢弃 FS 模块每条管线泄漏一个；`CompileModule` 在 `RegisterShaderBinary(std::move(spv))` 之后才把 `spv` 交给 shader collect。

## 2. 实现

| 项 | 实现 | 位置 |
| --- | --- | --- |
| 驱动 cache 持久化 | 新 `DriverPipelineCache`：`<CacheDir>/<serial>_<vendor>_<device>.vkpipelines`（设备 ID 在 P1a 时加入文件名，本节测量时仍为 `<serial>.vkpipelines`）。文件 = 应用信封（magic、格式、vendor/device/driverVersion/pipelineCacheUUID、长度、XXH3，逐字段小端编解码，不把文件字节强转为结构体）+ 驱动数据；再校验驱动数据自带的 `VkPipelineCacheHeaderVersionOne`。任一不符、读失败或驱动拒绝 → 空 cache，原文件改名 `.rejected` 保留。上限 128 MiB。 | `vk_driver_pipeline_cache.*` |
| 保存 | 后台线程 `shadPS4:PipelineCacheSave`：新建管线累计 16 条且距上次 ≥30 s 时、预载结束后、析构时。`vkGetPipelineCacheData` 两步查询，`VK_INCOMPLETE` 最多重试 4 次、超限跳过本次保存保留旧文件；大小不变不写。写入经临时文件 + fflush + fsync（Windows `_commit`）+ rename。不设 EXTERNALLY_SYNCHRONIZED，与编译并发读取由驱动内部同步。 | 同上、`cache_storage.cpp` `WriteFileAtomic` |
| 共享范围 | guest 图形/计算管线与 `TileManager` 的 tiling 管线都用同一个 cache（Rasterizer 构造后注入，析构前摘除）。 | `tile_manager.*`、`vk_rasterizer.cpp` |
| 预载顺序 | 先建驱动 cache，再 `WarmUp()`。 | `vk_pipeline_cache.cpp` |
| 并行预载 | shader 读取与模块创建仍按顺序（program/permutation 是共享状态），只把 `vkCreate*Pipelines` 分到 worker：桌面 `min(核数-2, 8)`，Android `min(核数-3, 4)`，高通专有驱动 1 个（同 citron）。构造只读已载入的 info/module 副本；插入 map 与计数回到调用线程。 | `vk_pipeline_serialization.cpp` |
| recipe store | IO 队列改为单锁 deque，`Close()` 排空后才返回；`PipelineCache` 析构时 `Sync()`（Android 会话结束即落盘）。profile 头追加三个序列化版本号；profile 或版本不符时删除该 title 的旧 blob（仅 `.meta/.spv/.key/.bin/.tmp`）后重建，而不是整个会话关闭缓存。无 serial 时不打开。 | `cache_storage.*`、`WarmUp` |
| 设置 | `Vulkan.pipeline_cache_enabled`（guest recipe，桌面默认仍 false）与新 `Vulkan.driver_pipeline_cache`（默认 true）分开。Android：设置页 GPU 下“Shader Cache”“Driver Pipeline Cache”两项，默认都开，全局/按游戏覆盖，经 JNI `nativeSetPipelineCacheEnabled` / `nativeSetDriverPipelineCacheEnabled` 在会话启动前写入。 | `emulator_settings.h`、`fex_session_jni.cpp`、`FexSessionService.kt`、`PipelineCache.kt`、两份 settings json |
| 测量 | 每次创建挂 `VkPipelineCreationFeedbackCreateInfo`（1.3 核心，带 per-stage 数组）：API 墙钟与驱动 feedback 时长分列，只在 VALID 时采用 hit/duration。Profiler scope `Pipeline.CreateGraphics/CreateCompute/CreateTiling/CacheLoad/CacheSave`（`GPU.CompileGuestShader` 保留），counter `Pipeline.GuestTranslations/Created/DriverCacheHits`。DebugBus `pipeline_cache status`：实际生效的两层开关、store 是否打开、翻译/各类管线次数与耗时、预载（条数、墙钟、线程数）、驱动 cache 身份、启动时加载结果、保存次数与字节。 | `vk_pipeline_stats.*`、`diagnostics_commands.cpp` |
| 顺带修复 | 预载失败时清空 stage 状态；辅助模块在管线创建后销毁；shader collect 在 move 之前取 SPIR-V。 | 见上 |

## 3. 桌面验证（RX 7600M XT，AMD 驱动 `0x80018b`，血源 CUSA03023，同一存档）

每轮启动后约 2 分钟（第 4 轮 1 分钟）取 `pipeline_cache status`。后几轮加载更快、两分钟内走得更远，所以各轮的新增内容不同，不是同一场景的严格 A/B。

| 轮次 | 条件 | 预载 | 运行期新建 |
| --- | --- | --- | --- |
| 1 | 无任何缓存 | — | 翻译 323 个（463 ms，最长 108 ms）；图形 216 条 2481.8 ms（均 11.5 ms，最长 97.7 ms）；计算 32 条 3.7 ms |
| 2 | 串行预载 | 248/248，1379.5 ms | 60 个 shader、55 图形、4 计算。与第 1 轮新建管线重合 0；10 个 FS/CS 以新排列重新编译（特化与已存的不同） |
| 3 | 并行预载（8 线程） | 307/307，408.0 ms | 29 个 shader、24 条管线，与前两轮重合 0（缓存命中正确，新增均为新内容） |
| 4 | 驱动 cache 文件翻转 1 字节 | 347/347，538.9 ms（8 线程） | 1 分钟内 guest 管线 0 条，tiling 8 条 |

第 4 轮：加载结果 `length or checksum mismatch`，原文件保留为 `.rejected`，回退空 cache；预载结束立即保存 188,628 字节。测试后删除该 `.rejected` 文件，桌面 `config.json` 按备份逐字节恢复（SHA `274611bc…`，`pipeline_cache_enabled` 回到 false）。

**AMD Windows 驱动不设置 APPLICATION_PIPELINE_CACHE_HIT**：四轮 feedback 全部 VALID、命中 0；其 VkPipelineCache 数据只有 129–189 KB，实际依赖驱动自己的磁盘缓存。驱动 cache 的收益与命中率必须在 Turnip/高通上测，桌面数字只证明 recipe 预载和并行化。

## 4. 与 spec 的差异 / 未做

- **未在 AYN 上运行**（本轮设备断开，adb 只见一台不相关的 Pocket DS，未动）。host `HOST_LINK_PASS`、playstoreDebug APK 编译通过，Kotlin 设置测试 PipelineCache 4/0、ForceDisableMsaa/SilentDialogs/TextureQuality 各 3/0。Turnip 上的 feedback 命中、预载耗时、`.vkpipelines` 大小、被杀后是否保留，均待实机。
- 保存与编译并发调用 `vkGetPipelineCacheData`（规范允许），没有做 spec 建议的“编译空闲边界导出”；若实测驱动锁串行化明显再加门控。
- 只接入 tiling 这一种 host 管线；其它 host utility pipeline 仍用空 cache。
- recipe store 内容仍无校验和：原子写避免了新的半文件，但历史损坏文件仍会在 Reader 越界时 ASSERT。
- 暖启动仍有“特化不同→新排列”，排列序号与首次出现顺序有关（spec P2 的稳定 key 未做）。
- P1 全部未做：`PipelineInterface/Record/Recipe` 拆分、有界 worker、准确异步（VkRecord 等待）、graphics skip 模式、HLE 前置于 compute PSO 创建。血源 320/326 ms 两处 compute 卡顿需要在 AYN 上用新 scope 重新归因。
