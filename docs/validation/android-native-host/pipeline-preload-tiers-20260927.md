# Pipeline 缓存：blob 完整性校验与 P2 分级预载（2026-09-27）

分支 `feature/malos/mhw_fix`，本地未提交。接续 [P0](pipeline-cache-p0-20260926.md)、[P1a](pipeline-compile-async-20260926.md)、[P1b](pipeline-skip-hle-first-20260926.md)，对应 spec `docs/specs/pipeline-compile-cache-20260926.md` 的 P2（`origin/malos/main` `14306f95`）。

## 1. recipe blob 完整性

原来 recipe 缓存（`.meta/.spv/.key/.bin`）直接存序列化内容，读取用 `Serialization::Reader`，越界即 ASSERT。历史上写到一半的文件或损坏文件会让每次启动都崩溃，Android 用户难以自行清理。

- **信封格式**：现在每个 blob 带一个 24 字节信封：magic `SPRB`、格式号、长度、payload 的 XXH3，逐字段小端编码。写入时在 IO 线程加信封，读取时先校验。
- **API**：`DataBase::Load` 返回 `Missing / Damaged / Ok`。`ForEachBlob` 只交出校验通过的 payload，并返回跳过的损坏数量。
- **失败处理**：损坏的 blob 按缺失处理（打 WARNING），不再交给 Reader。
- **版本联动**：`Storage::BlobFormatVersion` 并入 profile 里的版本数组。旧格式缓存的 profile 会被判为 damaged，整个 title 的缓存重建。

实测：
- 旧缓存启动时报 `profile.bin is damaged` 后重建。
- 手动翻转一个 `.meta` 的 1 字节，启动时报 damaged 并跳过，324 条中预载 323 条、1 条计为过期，游戏正常。

## 2. P2 分级预载

原来 WarmUp 在启动时把缓存里的全部历史管线建完才开始游戏，缓存越大启动越久。现在的流程如下。

**使用记录**：新增 blob 类型 `Usage`（`usage.use`）。每条管线记录：
- 最近一次被使用的会话里首次绑定的序号和时间（ms，从 PipelineCache 创建算起）；
- 距今几个会话未用；
- 最近一次驱动创建耗时。

首次绑定在 `Pipeline::Bind` 里记录（每条管线每会话一次）。保存时机：
- `Sync()`（会话结束）；
- 运行中每 4096 次取管线检查一次，若距上次超过 60 s 且有新的首次使用，则存一次检查点，应对 Android 进程被杀。

编码为带边界检查的小端格式，不经过会断言的 Reader。超过 32 个会话未用的记录丢弃。

**预载顺序与等待**：
1. 并行读取 recipe、创建 shader module 和 layout，但**不调用驱动**：管线以 deferred 状态构造。
2. 排序：有使用记录的在前，按（多少会话未用、首次使用时间、序号）升序；没有记录的保持文件顺序放最后。
3. 全部放入编译器的 **backlog**。启动阶段 backlog 可以用满所有 worker，主线程也按顺序参与构建或等待（`WaitHandle`），直到用完预算 `Vulkan.pipeline_preload_wait_ms`（默认 3000 ms）。
4. 之后 backlog 并发降为 1，余下的在后台慢慢建，不挤占游戏。

**按需插队**：
- 运行时取到的管线若还在 backlog，就把它移到按需队列最前面（`PromoteIfPending`，每条只做一次）。
- 绑定时仍未建好：在准确/同步模式下，由绑定线程直接认领构建（与 P1a 相同）；在 skip 模式下按 skip 规则处理。
- worker 永远先处理按需队列。

**状态**：`pipeline_cache status` 的 preload 行增加：
- 有使用记录的条数；
- 读取 recipe/module/layout 的耗时；
- 启动前建好的条数与耗时；
- 留给后台的条数、后台已建条数、被 draw 提前的次数；
- 已保存的使用记录条数、本会话首次使用的管线数。

## 3. 桌面验证（血源，890M 核显，同步模式，recipe 缓存临时打开）

| 轮次 | 设置 | 结果 |
| --- | --- | --- |
| A 冷 | 旧缓存 → 重建 | 运行中检查点保存 297 条使用记录；正常关闭后生成 384 spv/meta、298 key、1 usage |
| B 暖 | 默认预算 3 s | 298 条中 297 条按使用记录排序；读取 110 ms；298 条在 621 ms 内建完（预算未用满），0 条留后台 |
| C | 预算 100 ms | 88 条在 101 ms 内建好，236 条留给 worker，最终 323 条都由 worker 建完；0 次被 draw 提前（后台先建完了）；到标题菜单画面正常 |
| D | 预算 0、1 个编译线程 | 0 条启动前建，324 条全部后台；游戏首次需要之前已全部建完，0 次等待、0 次提前 |

桌面 AMD 驱动自带缓存是热的，加上游戏开头有一段加载，后台总能先完成，所以“绑定时管线仍在 backlog”的路径没被游戏触发。该路径与 P1a 的延后绑定是同一机制（执行时取句柄、排队中的由使用者自建），P1a 已经验证过。

测试后桌面配置按备份恢复（SHA `274611bc…`）。Android host `HOST_LINK_PASS`、APK 编译通过。

## 4. 未做

- 在 AYN/Turnip 上验证预算与后台节奏：Turnip 没有驱动自带磁盘缓存，驱动 cache 冷时差异最明显。
- 使用记录按 key 哈希折叠了类型：本会话没有加载的旧记录（例如预载失败的）在保存时会被丢弃，而不是猜测其类型。
- 仍未做：
  - GCN 翻译后台化；
  - tiling 管线预热；
  - 稳定的排列哈希；
  - 每个 draw 的管线查找开销（GetProgram 在 GpuComm 上约 16%）；
  - skip 模式的缺失内容传播与自动切回。
