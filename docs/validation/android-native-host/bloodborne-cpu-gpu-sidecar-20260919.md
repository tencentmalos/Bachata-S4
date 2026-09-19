# 血源：CPU/GPU、等待链、异步调用与长帧贡献

## 结论

这轮现场不支持“`sceNetEpollWait` 持全局锁，让所有 guest 串行等待”的判断。
网络等待期间主线程和多个 guest worker 持续获得 CPU；提交线程主要等 Guest-1 的
显式 condition 通知，Guest-1 又包含真实执行、调度等待以及对其他 guest worker 的等待。
因此不能通过把 epoll 改回立即返回来解决当前低帧率。

已经修复的网络问题是空 epoll 请求 33 ms 却立即返回，造成约 27,500 轮/秒的 guest
轮询。现在每秒约 30 轮，等待期间释放网络域锁和对象 guard，不占 VM gate/guest pin。
Console Language 的命名配置链路及简体中文也已验证。详见
[语言与 epoll 修复](console-language-offline-epoll-20260919.md)及
[最初 Litep 样本](bloodborne-performance-20260919.md)。

本轮进一步交付的是可重复使用的工具能力和更窄的瓶颈证据，**没有宣称血源低帧率已解决**。
GPU 仍然较重；本次新画面、运行阶段与旧样本不完全一致，不能把 FPS 差异归因于本轮插桩。

## SDK 与工具实现

- Profiler SDK 从本地 `0ef631c` 合入真实远端 `main` 的 `d270d84`，合并提交
  `e00321ea40c4c608aff74a787200bc59493eea19`。保留既有 stage/GPU ring 修复；没有
  用 reset 覆盖本地修复。Litep SDK 和 Foundation 子模块均更新到同一提交。
- Foundation 增加 `LiteTrace::post/beginFlow`。shadPS4 Vulkan 入队前产生唯一 flow ID，
  原有 `Vulkan.WorkerSubmit` 接收它，不增加第二个执行 zone，也没有修改提交等待逻辑。
  ID 跨 recording generation 失效；排队延迟包含生产侧准入、队列与调度。
- Litep 普通解析和 SQLite 索引复用新版 SDK 的异步校验，保留重复/缺失/丢块/循环诊断；
  `query_async_flows` 支持按排队耗时排序。旧索引明确要求重新索引才能补充 flow 信息。
- `analyze_wait_chains` 按 condition、mutex、waiter 身份连接显式选中通知，分别统计
  enqueue→notify、notify→resume、resume→reacquire。`profile_sync` 还增加 Arg3 和
  epoll 的 guest caller，诊断默认关闭。
- `load_kgsl_sidecar`、`analyze_cpu_gpu_bottlenecks`、`close_kgsl_sidecar` 提供真实
  KGSL/sched sidecar 接入。校验 SHA、PID/boot/start ticks 和时钟锚点，拒绝错配、
  未校准时钟与留存窗口外的分析；保留 overrun、缺端点和不支持的信息。
- `analyze_frame_contributions` 使用精确的相邻有效 FrameMark，完整 scope 在帧内
  裁剪，父级扣除子区间，空白保留未归因。按连续邻帧的 exclusive 中位数列出多花时间
  的几笔，保留具体起止和 scope ID，并展示 owner→通知生产者及生产者自身的等待。
  跨帧 Post 和 condition 阶段保留原始端点，另算帧内 overlap，不能把完整跨帧时间
  全部算入本帧。每线程分区独立，不把并行 worker 相加当帧耗时。
- 旧共享 `analyze_serialization` 的 nearest-overlap 只是时间相关。输出已改为
  `overlapCandidates/candidateScope`，`causalityConfirmed=false`；先裁剪窗口，并将
  包含并行/嵌套的合计标为 total，而非 union coverage。分析引擎本地提交
  `7d34b8c`、`4f5a316` 保存实现及工具说明。
- 调度状态查询改用区间前缀索引，避免每个短 HLE scope 扫描完整调度记录。真机数据的
  单帧联合查询约 4–5.3 秒；早期逐段扫描曾超过一分钟，该版本未作为最终安装版本。

源码与使用说明位于
[`litep/docs/bottleneck-analysis.md`](../../../../../../spatial_mcp_publish/dev_tools/mcp/litep/docs/bottleneck-analysis.md)。
本地工具版本为 `0.2.0-local.20260919.bottleneck3`；已有客户端连接需要重连才能使用
新增工具，旧连接不会热替换。

## 同一次采集的身份与质量

| 项目 | 身份 / 测量 |
| --- | --- |
| 设备 | AYN Thor `9c2841a4`，Android 13 / Adreno 740 |
| build / boot | `Thor_V1.0.0.377_20260206_165408_user` / `8bb14501-5800-4b9b-a9ab-7173603812f7` |
| 游戏与配置 | Bloodborne CUSA03023，病房面对桌子；中文，0.5 / High 1×1 / FDM OFF |
| 运行 | PID 20429 / generation 1 / UUID `ca5cadec43fed1cfd3abb7302cc10ca7` |
| 驱动 | Turnip Mesa 26.0.0-devel `5ac41be677`，SHA `fdd37852…` |
| APK / host / JNI SHA 前缀 | `a507ca92` / `33f1e89a` / `3f7cedcc` |
| PROF | `flow-20429-1.prof`，17,571,306 字节，SHA `3cee9701c35be2350647c42e655421f9739c77d48035388cf0ce9afe9e008698` |
| PROF 数据 | 10.000 s，2,126,054 事件，210 chunks / 0 skipped，47 个完整 source 帧 |
| GPU 数据 | 474 对 zone / 0 orphan，独立 GPU 时钟未校准，不与 CPU 帧强行拼接 |
| KGSL | 18 秒、256 MiB **总**预算；SHA `3080439fa418c6585fa494a1355540d91f556ee17f391302065975260a2ead2c` |
| 调度质量 | overrun 0、dropped 0、无未解析行/调度断裂；保留 1,381 条 zero-timestamp batch 观察，未强行配对 |
| 时钟 | tracefs mono + 同次 CNTVCT 双锚点；报告不确定度 1,134 ns |
| 联合窗口 | `[611019269924118, 611027582841881)`，8.312917763 秒 |

PROF 尾部超过 KGSL 停止时间，因此联合分析只取交集；并非声称整份 10 秒都覆盖调度。
采集首尾仍有未配对 CPU scope，`truncated=true`，不冒充全量无损。KGSL 独立 instance
`kgsl_dbg_3b0e140ae766` 已冻结、压缩收集、删除；全局 tracing/clock/buffer 前后实测相同。
没有启用 GPU snapshot、debugbus fulldump 或全局 tracefs。

## 当前瓶颈的实测拆解

整份 PROF 的成功 guest present 约 **4.704 FPS / 212.58 ms**；44 个 GPU.GuestFrame
样本均值 **68.81 ms**，最大 81.39 ms。这说明 GPU 仍是成本项，但不能单独解释全部
CPU 帧产出间隔；GPU elapsed 包含 GPU 依赖，不能等同着色器独占执行或 GPU 饱和率。

在 8.313 秒共同窗口内：

| 线程 | on-CPU | runnable 未获调度 | sleep |
| --- | ---: | ---: | ---: |
| Guest-1 / TID 21163 | 4,347.86 ms | 1,235.57 ms | 2,729.49 ms |
| Guest-20 / TID 21218 | 2,383.50 ms | 1,049.15 ms | 4,880.26 ms |

其他渲染 worker 同时执行，不是等待 epoll 退出后才一起运行。Guest-1 的 on-CPU 中
737.57 ms 落在 HLE，3,610.29 ms 在 HLE 外；后者包括 guest JIT/FEX 和未插桩 host，
**尚不能全部命名为某个 guest 函数**。完整 PROF 的 epoll 298 次，平均 33.204 ms，
其网络线程 TID 21270 不是 main Guest-1。

721 条 Vulkan Post→执行边全部匹配，排队中位数 0.0386 ms，P99 72.78 ms，最大
80.64 ms。KGSL 提交到 retired 的完整批次在共同窗口内有 608 个，最大 82.30 ms；
最慢 KGSL wait 82.43 ms 的 CPU 执行仅 0.025 ms。这些确实是驱动/GPU 等待，
不是那条 scope 在 CPU 上执行了 82 ms，也不能把批次延迟当成 GPU busy。

## 长帧：从一笔大等待继续定位生产者

选取 **KGSL 完整覆盖范围内最长**的 source 帧 2884：
`[611027259444587, 611027545614066)`，286.169479 ms；邻近完整帧时长中位数
251.318073 ms。GNM source 帧是提交 epoch，本表不把它称为精确上屏区间。

| owner Guest-19 的区间 | 本帧 exclusive | 相比连续邻帧中位数 |
| --- | ---: | ---: |
| `HLE.scePthreadCondWait` | 239.864 ms | +24.991 ms |
| 尚未插桩/端点不完整区间 | 35.574 ms | +14.623 ms |
| `HLE.sceGnmSetPsShader` | 1.339 ms | +0.501 ms |
| `HLE.pthread_mutex_lock` | 0.800 ms | +0.495 ms |

各项中位数独立计算，差值不能机械相加成时长中位数的差；所有 owner 区间分区总和
精确等于 286.169479 ms。父子 inclusive 与其他线程数据不重复累加。

显式条件变量 `0x10004dcc00`（十进制 `68724575232`）的这次等待从
`611027259525681` 到 `611027499372816`，共 **239.847135 ms**：

- enqueue→Guest-1 selected-notify：239.787083 ms。
- notify→resume：0.055156 ms。
- resume→guest mutex reacquire：0.004896 ms。

对应 guest 返回地址为 wait `0x24852bd`、notify `0x247fc6c`；报告保留父调用链与
精确对象身份，这些是 guest runtime 地址，不是已经完成语义命名的函数。

在通知发生前，生产者 Guest-1 实测 **124.065883 ms on-CPU、30.557 ms runnable、
85.1642 ms sleep**。它自身在本帧有 61.06 ms 和 12.49 ms 的 condition 等待由
**Guest-56 / TID 21712** 通知，另有 Guest-51/52/39 的较短通知。它们属于同一帧内
进一步值得定位的任务链；不能把这些重叠时间再次加到 owner 的 239.85 ms 上。

旧 PID 4603 的帧 1662 是另一组有效例子：427.90 ms 中 condition 等待 397.84 ms，
比邻帧增加 147.03 ms；明确的通知前阶段 397.794 ms，而通知到恢复/重获锁仅约
0.032 ms。旧 KGSL 128 MiB/25 秒采集曾覆盖前缀，所以只使用所有 CPU 留存交集，
不能称旧采集零丢失。新 256 MiB/18 秒样本解决了这次覆盖问题。

## 推进顺序

1. 沿 Guest-1→Guest-56 的精确条件对象和 guest caller 标定生产任务，补上其 HLE 外
   的真实主函数。原 ELF SHA `cc2826ae…`；返回地址必须先对应实际装载基址，再做 C/C++
   拦截/替换。不要依据通用 cond wrapper 名称就把所有等待当成一个全局锁。
2. 分别考察生产者的任务依赖与 runnable 延迟。当前数据支持调度竞争，但还不能断言
   固定绑核、提高优先级或删除同步会改善游戏；需要保持场景和配置一致的验证。
3. Vulkan 异步排队的约 80 ms 尾部与 KGSL 等待继续保留为独立问题。先验证它是否
   反馈到实际生产关键路径，再修改 driver poll/队列策略；本轮没有替换驱动或删队列锁。
4. 暂不扩大 FDM、shading rate、画质、FEX 同步策略或图形 copy/blit 改动。

## 验证与交付边界

- SDK `tools/verify --scope all --serial 9c2841a4 --api 33`：specs、harness、Python、
  host、Release 编码基准及 Android 真机全部 passed。报告
  `sdk/build/verification/20260919T144847Z-f50d2129/report.json`；最初缺 OpenSpec/
  GoogleTest 的 blocked 记录保留，补齐依赖后的结果才作为通过依据。
- Litep Python **86 / 0**，覆盖跨帧裁剪、父子去重、邻帧断点、跨帧通知/flow、迟到
  大排队不被 top 隐藏、错误身份、调度迁移/抢占/缺 wake、缺失/重复/循环 flow。
- 共享引擎 build、**82 / 0**、self-test **42 tools**；Android host/APK 构建通过。
- 包管理器安装、Codex 配置及全部 11 个 Doubao wrapper 重新生成并执行初始化/列工具。
  Litep 与其他 9 个通过；独立 webaccess 后端报 `fetch failed`，重试仍失败，未改动
  它的配置。不能把整体集成写成 11/11 通过；Litep 新接口另用安装版本完成真实数据验收。
- 大 PROF、SQLite、压缩 ftrace 和中间 APK 留在 `build/validation/bloodborne-perf-20260919/`。
  小型证据清单见 [evidence/bloodborne-perf-20260919/sidecar-manifest.json](evidence/bloodborne-perf-20260919/sidecar-manifest.json)。
- SDK 合并和引擎小提交仅在本地；shadPS4/Foundation 与其它既有修改未统一提交、未推送。
  没有完整游戏回归，也没有将诊断插桩前后的 FPS 变化声明为优化收益。

最终安装版本通过真实 JSON-RPC 调用核验：721 条匹配 flow，KGSL 加载 9.276 秒，帧2884 联合查询 5.309 秒，综合窗口 8.313 秒，sidecar 正常释放。最终设备 PID20429/gen1 已正常 `Stopped/user_stop`，`profile_sync=0`、GPU timing OFF、file capture inactive、ring ON、TracerPid0；独立 tracefs instances 为空，无持续自动输入。设备可能仍显示停止前最后一帧，运行状态以服务快照为准。
