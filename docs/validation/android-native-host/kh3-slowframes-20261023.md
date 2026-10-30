# KH3 / Odin3：慢帧采集与第一轮优化（2026-10-23）

## 现场与口径

Odin3 `6edb0593`，KH3 CUSA15072 01.10，PID 19591 / generation 1，
run UUID `dbe306f0a42b30714093133759b652b7`。起点 `22bd5a9d7`，
分支 `feature/malos/swan_performance`。采集时 APK SHA 为
`6ad827014af5f1ebd0137d5ce65d73b03a5443c25d51544a2c50dc339c4c44a8`。
Turnip 为 `68dafb89a26`，设备驱动 SHA
`d7e6746a5eb02500f8d4ac3022ae8459a3e683b56ba55b6dc2705b5706f46099`。

用户自行游玩黄昏镇；没有重启游戏、清缓存、改变渲染设置或自动操作角色。
后续状态读取显示用户已把游戏切到后台：仍是同一 PID / generation，
`Paused / background`；测试后用户自行恢复，最终同 PID / generation 为 `Resumed`，
guest_flip 84264 / host_present 84228，仍保留原会话。失声按用户要求等待再次出现后排查。

帧号是 `sceGnmSubmitDone` 的 SDK 提交周期，**不是成功上屏帧**。
PROF 头部的 app_ver 为基础包的 01.00，不能用它覆盖实际选中的 01.10。
所有 scope 时间均为经过时间，不是 on-CPU 时间；跨线程耗时不能相加。

本地证据根目录：`build/validation/kh3-slowframes-20261023/`（不入仓）。
原始 PROF、sidecar、SQLite 索引、分析 JSON、截图均已保留。

## 样本完整性与分布

| 样本 | 完整提交周期 | 结果 | 边界 |
|---|---:|---|---|
| 驻留环形快照，Guest-31 连续约 48.98 s | 1759 | 平均 27.85 ms；P50 27.14、P95 35.92、P99 42.07 ms；>33.33 ms 221 次，>50 ms 9 次，>100 ms 3 次；最大 149.38 ms | 235 chunks / 0 skipped / 0 pairing errors；每线程留存长度不同 |
| 后续 20 s 连续文件 | 721 个留存的完整周期 | 最大 111.61 ms；用于核对管线等待 | 961 chunks / **44 dropped markers**，影响区间未知，不能当无损频率统计或拼接 flow |
| 自动慢帧快照 | 6 个触发窗口，各 8 个完整周期 | 85.33–449.60 ms | 每份 0 skipped / 0 pairing errors；仍有窗口尾开放 span |

环形快照排除了 TID 19809 的启动帧 1/2。统计只取 TID 19810 / Guest-31 的完整周期。
环形快照中的 GpuComm 只留存末尾约 0.23 s，Vulkan recorder 只留存末尾约 1.55 s；
不能用它们在早期帧的缺失记录认定没有工作。连续文件的丢块记录已保留，
Litep 将其 flow 标为 data_loss；本页不把这些边补成已验证依赖。

慢帧触发配置：factor=2、min_ms=33、pre=5、post=2、count=6、mib=8；
检测 11 次、保留 6 次、抑制 5 次、失败 0 次。已停止触发（enabled=false），
原本开启的驻留 ring 继续保持开启。触发快照是筛选样本，不能计算全局卡顿发生率。

## 第一类：管线创建与录制/提交排队

| SDK 帧 | 帧长 / 邻帧中位数 | 主要等待，ms |
|---|---|---|
| 34619（ring） | 149.38 / 23.09 | Pipeline.CreateGraphics 整次 144.85，窗口内 143.61；提交 worker 等 recorder 145.56；Present 等提交 133.12；帧 owner 等 equeue 137.60 |
| 38340（continuous） | 95.28 / 31.71 | recorder 的 Pipeline.WaitReady 86.78；创建窗口内 86.75；worker 等 recorder 94.14；Present 等提交 82.25 |
| 38346（continuous） | 111.61 / 34.11 | recorder 等管线 109.50；两个编译线程分别约 111.59 的创建覆盖；worker 等 recorder 110.53；Present 等提交 97.20 |

生产代码的顺序是：Bind 录入 WaitHandle → recorder 等管线 Ready →
submit worker 的 WaitFinished → 队列中的 present 提交。
ring 帧还保留一条已验证的 present Post→WorkerSubmit flow，排队 131.79 ms。
ring 缺少该帧 recorder 本体，连续样本则有明确的 Pipeline.WaitReady，但带丢块告警。
这些证据支持“管线构建等待拖住录制与提交”；不把所有关联线程拼成一条无损关键路径，
也不把多个编译线程的时间相加。

本会话配方缓存和驱动缓存均开启，启动载入驱动缓存 58,268,202 B，
预载 2155 条管线。配置是 async_graphics_skip，但 frame 23700 的缺失内容进入 CPU
回读后，保护逻辑已关闭本会话的 skip。这个行为没有被放开或绕过。

**驱动反馈有误报路径。** 固定的 Turnip `tu_pipeline.cc` 中，shader lookup 失败时
只置 `cache_hit=false` 就 break，没有把初值为 true 的 `application_cache_hit`
置 false；后面的 APPLICATION_PIPELINE_CACHE_HIT 判断也没要求 cache_hit。
因此本次“240 次创建、240 cache hits”不能证明未编译，不能据此断言缓存失效或缓存锁阻塞。
此次主仓把诊断文字改为“reported cache hits”，慢创建（≥20 ms）新增 key、shader 名、
elapsed/driver 时间与原始反馈日志。**未修改、构建或更新 Turnip**；驱动问题仍待单独修复。

## 第二类：游戏侧等待和对象/内存回收

| 触发 ID / SDK 帧 | 帧长 / 邻帧中位数，ms | 已观察到的主要事实 |
|---|---|---|
| 1 / 52234 | 101.75 / 25.87 | 帧 owner 条件等待 101.46；Guest-44 的单次 PoolDecommit 79.04 |
| 2 / 54692 | 85.33 / 32.49 | owner 条件等待 84.52 |
| 3 / 55101 | 449.60 / 24.29 | owner 条件等待 447.39；Guest-1 在此周期销毁 4746 个 mutex，共 56.46 ms |
| 4 / 55208 | 211.17 / 33.75 | owner 条件等待 209.69 |
| 5 / 55812 | 108.11 / 26.67 | owner 条件等待 106.87 |
| 6 / 56565 | 85.43 / 23.29 | owner 条件等待 85.21 |

449.60 ms 周期中，Guest-1 另有条件定时等待 117.18 ms、shadSyncWait 78.61 ms、
22 次 PoolDecommit 合计 23.60 ms、24 次 PoolCommit 合计 8.64 ms。
多条文件读取线程也有 pread；没有 Pipeline.CreateGraphics / WaitReady 记录，
submit worker 的 RecorderFinishWait 只有 0.70 ms。邻近 55103 还有 723 次 mutex 销毁。
这是对象/资源集中回收或场景转换的候选，**没有 guest 函数调用链或 selected-notify
边证明 GC、I/O 或某一个生产者是 450 ms 的根因**。

52234 的 79.04 ms PoolDecommit：VM.RangeRetirement 仅 0.041 ms，
Memory.WriteHold 0.013 ms，VM.DataMapping 0.159 ms。
大段时间在 range retirement 之后、writer lock 之前，生产代码对应 GPU 缓存失效阶段；
既有记录无法进一步区分 buffer tracker、texture mutex、page manager。
新增这些阶段和 texture mutex 获取的 PROF scope，供下一轮直接区分，未改变内存回收语义。

## 已实施的保守优化

`GuestMutexDomain` 的不可变 bucket directory 已保存所有 mutex 身份到 session 结束，
并具有原子的 retired 状态；原先又用一棵 std::map 记录活跃对象。删掉重复的树，
Init / Destroy / CondWait 的活跃检查复用 directory + retired。
保留原注册锁、忙锁/条件等待者检查、退役握手、guest 写回及 observer 路径。
这减少每个对象额外的一次 tree-node 分配/释放；不是把真实游戏同步等待跳过。

Odin3 上以单独进程运行生产实现：
- native mutex 65,504 checks / 0 failures，包含并发退役、条件等待、2 万存活对象、
  旧 handle 别名拒绝与重新初始化。
- condition 67 / 0；services 82 / 0；真实 FEX guest fastpath 80 / 0。
- 新增 `guest_vm_mutex_bench --lifecycle`：2048 个普通 VM mapping + 6000 个 mutex，
  打乱顺序销毁；不计初始化时间。两版二进制留存于本地 mutex-bench/。
- 固定 CPU 7，A/B/B/A 各 7 轮的中位数：**9.456 / 9.289 / 8.700 / 9.457 ms**。
  前后 A 稳定，B 有波动，观察到约 2–8% 的 host 销毁成本下降。
  不固定核的结果波动更大，不能据此报告确定的提升比例。

这只是很小的宿主成本优化，**不代表 450 ms 卡顿消除或游戏 FPS 提升**。
未为取得相同场景而覆盖用户当前后台会话；游戏内前后对照尚未做。

## 候选包与交付状态

Android host 编译链接、APK assemble 和 feature/session 18 个单测通过。
APK SHA：`7fc595d01fa7957b373bfb8a51eb22f72b6af9187d8baa4992b9d46bb80dc41b`；
包内 host SHA：`b01ebb49b889da69ecbe30245f0ea3e3fdc856249e26e41cbadff338d0e97e9e`，
Build ID：`e9e33e682d9bac6180e6da6cff243a5221bae1aa`，与本次 native 构建一致。
路径为 `android/shadps4-app/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk`。
为保留正在运行的原局，**没有覆盖安装候选包**；设备仍运行采集时的 `6ad82701`。
设备上的本轮独立微基准/测试程序目录已清理，所有结果与二进制留在本地；慢帧触发已关闭。
当前修改未提交，未改子仓引用；同目录之前的会话/崩溃修复一起打入候选包。

## 后续验证重点

2026-10-24 已按用户保存后的授权安装包含本轮修改及 AJM 修复的 `ac5b5a96`，
新会话采集与自动捕获漏报修复见 [续验记录](kh3-slowframes-20261024.md)。
上面的 `7fc595d0` 是历史中间候选，未安装。

1. 在正常保存/结束当前局之后部署候选 APK，核对 SHA，重复同一黄昏镇路线和转场。
2. 用新分段定位 PoolDecommit 的长等待；不可直接删锁或把需保留的数据当作 discard。
3. 管线侧保留 CPU 回读正确性保护，先修驱动反馈并分清首次编译、已有 shader 重建、
   cache serialization；不凭 reported hit 给出缓存命中结论。
4. 需要进一步区分 on-CPU / runnable / sleep 或 GPU 工作时，再采同步 sched/KGSL/GPU
   query。当前采集的 GPU query 为 0，也没有校准过的 KGSL sidecar，不能判断 GPU 饱和。

## 证据定位

- ring：`ring/6edb0593-19591-ring-1792768024301697-1.prof`，
  SHA `8684758f02ececf50714bde693b91ba2d7888bbeba666a80024b08ac7163dca9`。
- continuous：`continuous/6edb0593-19591-file-1792768104823148-1.prof`，
  SHA `18b75ab10f59f27f3ab2e02613b7bf882604766e626bf6156896021447e12e55`。
- 六个窗口的完整文件路径、SHA、frame、index 和边界在 `analysis/slow-manifest.json`。
- 每帧 Litep 分析为 `analysis/slow{1..6}analysis.json`；其余分析、SQL 结果同目录。
- `slow-stopped.txt`、`session-end.txt`、`pipeline-final.txt` 保存结束状态。
