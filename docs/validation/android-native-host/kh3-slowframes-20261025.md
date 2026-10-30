# KH3 Odin3 慢帧采集（2026-10-25）

> 后续离线对齐发现：本页原始数据的 GNM marker 不能当作呈现周期。见下方
> “呈现节奏复核与采集改良”；本轮改良采集判定，尚无游戏性能改善结论。

用户正常游玩时采集，未暂停、重启、输入控制、修改游戏设置或安装 APK。
18:03:57（Asia/Shanghai）截图为《玩具总动员》胡迪与索拉的剧情，叠层约 30 FPS。
18:07 完成留档，同一 PID/generation/UUID 保持运行。

## 身份和方法

- Odin3 `6edb0593`，PID `13851`，generation `1`，UUID
  `70e66592b20a73fbea91baa71af8f515`，CUSA15072。
- 设备 APK SHA `607e97614cf556ca157dd21f2b3b3ee8dfa4b7ed3c0f2a30c28ce5e580652143`；
  驱动 `d7e6746a…` / Mesa `68dafb89a2`。并未安装上轮静态 STL 候选包。
- LiteP target `kh3-odin3-20261025-13851`，每次操作核对 PID。
  常驻 ring 原已开启，1 MiB/thread；不启用 Streaming 或 fine scopes。
- 先导出全部 ring，再开启有界自动慢帧捕获。初始 2 倍基线、至少 33 ms；
  根据现有 50–63 ms 周期，改为 1.5 倍且至少 50 ms，前 5 / 后 2，
  5 s 冷却，最多 8 份、每份最多 8 MiB。

## 已保存数据

证据目录：`build/validation/kh3-slowframes-20261025/`。原始 PROF、身份 sidecar、
SQLite 索引、截图、帧贡献 JSON 和状态快照均已保留。

| 数据 | 字节 | SHA-256 | 索引 |
| --- | ---: | --- | --- |
| ring | 62,472,502 | `8ebb73634dcf2ca952de01547a102d9673db80134345e0017f0cf3e5e7f504c8` | `a5bb33941ac24c2bb8e1d99bec3392da` |
| 慢帧 45696 | 1,132,180 | `ba693a3bb98a5c2036e27f22df882c536e2f98ac509d1ff0120b03778676d039` | `9a405352104e4ba2b8caf2426957d8c6` |
| 慢帧 46435 | 1,389,268 | `f7be01eedb0eaa5f41100c370549189dd620cfc69c9712a51f3968abc996c053` | `b733579223824b869191c33e05e33359` |

三份均 data_complete，0 skipped chunks、0 scope pairing errors、未截断。
边界尚未闭合的 scope 与缺少端点的 flow 保留诊断，不等于丢块。
两个自动窗口各有 **8 个完整 GNM 提交周期**，包含 GpuComm、Guest-1 等高频线程。

自动捕获一度记录 4 个窗口（另两笔 54.73 / 61.35 ms）；**只将上表两份计为
成功导出**。停止捕获后，后续查询中内存列表已清空，第三笔导出返回
`slow_frame_capture_unavailable`；后两笔仅留状态元数据，不能用于线程分析。
后续批量收集应在停止/配置切换前逐份导出确认。

## 初步盘点

ring 中有旧 owner TID 14014 的启动记录，两个约 1 s 的周期不属于当前现场，
以及 owner 改变/末端边界，均不计入本次分布。实际 owner TID 14015：

- 1,091 个完整周期，CPU-clock 区间 `[312325110798620,312361475293776)`，36.36 s。
- 均值 33.33 ms，中位数 33.32 ms，P95 37.45 ms，P99 41.02 ms，最大 63.04 ms；
  8 个超过 50 ms。
- 这些是 `sceGnmSubmitDone` marker 周期，**不是逐帧成功上屏时刻**。
  ring 的各线程保留时长不同：GpuComm 仅保留末尾约 0.8 s，不能用整份
  1091 帧的 owner 分布推断所有慢帧的跨线程原因。

自动窗口的初步贡献：

| 原始 SDK frame | 周期 | owner 条件等待 | 比连续邻帧的等待中位数多 |
| --- | ---: | ---: | ---: |
| 45696 | 62.354687 ms | 61.691248 ms | 30.477968 ms |
| 46435 | 58.657968 ms | 58.114009 ms | 25.044061 ms |

等待为 `HLE.pthread_cond_wait` 的 exclusive elapsed。没有完整 selected-notify
关系，不能直接命名阻塞它的线程或函数，也不能把其他线程的 elapsed 累加为帧耗时。
两窗口提交 flow 的最大排队延迟分别约 0.139 / 0.391 ms；不是几十毫秒的提交队列积压。
第一窗口没有 Pipeline/Compile scope，从采集起点到中间快照的新 graphics 数量 242、
等待构建次数 153/2629.1 ms、driver cache 保存次数 7 均未增长。收尾时则变为
253、154/2656.4 ms、8：后半段确有 11 条新 graphics 和一次 27.3 ms 的构建等待，
不能把“前段未增长”推广为整轮没有编译。预载 3051、启动 driver cache
70,330,182 B 均有状态记录，新增工作与已保存慢帧的因果关系尚未确定。

本次未采 sched/KGSL sidecar、未启用 GPU timestamp；因此不判断 CPU on-CPU、
调度饥饿、GPU 饱和或可优化收益。只收集并初步分类，未修改实现。

## 收尾

- 同 PID/session 持续 Resumed，guest flip/present 正常前进。
- AudioOut/Oboe 状态 stalls/error/reconnects 均为 0；未采 PCM，不作听感结论。
- GPU faultcount 前后均 15。
- 自动慢帧已停止，常驻 ring 保持原先开启；Streaming/capture_active=0。
- 三个设备端导出文件逐一核对 SHA 与本地一致后删除，见 `cleanup.json`；
  本地原始证据和索引保留，游戏、配置、存档、已有其他诊断文件不变。

## 呈现节奏复核与采集改良

使用 LiteP 公共 `open_trace_index` / `query_prof_sql` 重开上述原始索引，比较
`VideoOut.PresentedGuestFrames` 的连续递增时刻与 GNM markers。
完整查询结果保留在证据目录的 `present-analysis.json`。没有直接用私有解码器处理 PROF。

同一 CPU-clock 区间内，成功呈现计数有 1,090 个完整间隔；它表示宿主成功接受新游戏
画面的时间，不是物理屏幕扫描时间，也不能证明每个画面内容不同。

| 指标 | GNM 提交周期 | 成功呈现间隔 |
| --- | ---: | ---: |
| 完整间隔数 | 1,091 | 1,090 |
| 均值 | 33.331 ms | 33.334 ms |
| P50 | 33.321 ms | 33.327 ms |
| P95 | 37.453 ms | 34.164 ms |
| P99 | 41.024 ms | 39.111 ms |
| 最大 | 63.037 ms | 55.754 ms |
| >50 ms | 8 | 2 |

两个自动窗口里的 62.355 / 58.658 ms GNM 周期，前面分别紧邻
7.142 / 8.957 ms 的短周期。同窗连续成功呈现仍为：

- 45696 窗口：32.935–33.709 ms；Guest-1 的几次 usleep 约 17–24 ms，呈现线程持续推进。
- 46435 窗口：32.656–33.797 ms；Guest-1 的 usleep 约 22–25 ms。

自动快照会在窗口首部写入计数器的保留值，故排除首个以窗口起点为前端的间隔，
不把它当作一次真正的短帧。结论是这两个目标的 **GNM 提交间距变化未表现为同窗呈现停顿**。
owner 条件等待的大数不能据此当成锁缺陷或可消除的 CPU 工作；没有 selected-notify
记录，也未把其它线程重叠的等待当成持锁证据。这两帧内没有 Pipeline/Compile scope，
不支持“PSO 失效导致这两个慢周期”的判断。

真正较长的呈现间隔为：

| Present ID | 前一次成功时刻 → 本次成功时刻（CPU ns） | 间隔 |
| --- | --- | ---: |
| 41135 | 312333761763932 → 312333817517682 | 55.754 ms |
| 41895 | 312359084652630 → 312359134889401 | 50.237 ms |

本次 `VideoOut.Flip` 分别只用 0.526 / 0.573 ms，其中 `Present.DriverCall`
0.191 / 0.205 ms、`WaitSubmitted` 0.045 / 0.051 ms，主要空档在进入这次 Flip 之前。
第一笔两次 Flip 开始相隔约 66.663 ms；第二笔前一成功呈现间隔仅 0.858 ms，
提示存在呈现节奏变化。**不能因此判定是队列空、限帧、未获调度、GPU 完成晚或 PSO**：
当时 GpuComm 已不在 ring 保留交集中，也没有 sched/KGSL sidecar。
不以邻帧条件等待推断根因，不调整睡眠时间、GPU 同步或帧率上限。

### 本地改动

1. 将 PROF 的唯一 FrameMark 从 `sceGnmSubmitDone` 移至 VideoOut 成功呈现新游戏画面后。
   失败、空白、重复重绘不计帧；GNM 保留 `GNM.SubmitDone` scope 与
   `GNM.SubmittedEpochs` counter，供提交阶段分析。
2. PC / Android 会话身份改为 `frame=videoout_present_success`。Foundation 使用已有的
   non-frame activation，使启动阶段即使还没有画面也能记录 scope/phase，不伪造启动帧。
   旧数据仍按其 `frame=gnm_submit_done` 解释，不混比 SDK frame 编号。
3. 普通与 VR flip 队列均增加显式 Post→`VideoOut.FlipDispatch` flow，另记录
   `VideoOut.ReadyFlipDepth`、`VideoOut.FlipEligible`、`VideoOut.PacingWait`。
   下一次真正的长呈现窗口可区分“工作尚未入队”和“已入队等待处理”，并保留节拍等待。
   flow 排队时间不等于 CPU 或 GPU 执行时间。
4. 会话切后台/恢复与 guest 调试暂停重置慢帧基线/待完成窗口，避免把人为暂停算成卡顿。
   此 reset 只影响自动检测；离线比较仍需排除 `VideoOut.Cadence.Reset` 跨界间隔。

未修改渲染输出、限帧、游戏时钟、同步语义或调度策略，未覆盖安装、未触碰正在运行的游戏。
这是采集准确性和归因能力的改良，不是已验收的 FPS 优化。

### 验证

新增独立集成测试 `tests/profiler`，链接生产 `common/profiler.cpp`、Foundation ring 和 SDK；
只有输出路径由 fixture 提供。Mac LLVM Release：**11 checks / 0 failures**。
覆盖首帧前启动记录、提交 burst/无效呈现不产帧、真实呈现产帧、跨线程队列 flow、
暂停重置、长呈现自动捕获、stop 后继续呈现仍可导出、ring 关闭。

两个负对照只改 build 目录中的源副本：去掉成功 guard 为 2 失败；关闭 non-frame activation
为 2 失败。原生产源未被负对照覆盖，测试构建已恢复正常源码。
本机 stop 后保留窗口正常，**没有复现上轮设备内存列表消失的问题，不能宣称已修复它**。

测试 PROF 经 LiteP 公共索引复核，均 data_complete、0 丢块/截断/配对错误：

| Fixture | Frame marks / 完整周期 | SHA-256 |
| --- | --- | --- |
| startup | 0 / 0（6 events，含启动 phase/scope） | `759993686a4b194863db331c1ddb3becaee5849db0c83c3fb2a04850b3fb0bd3` |
| present | 8 / 7 | `82b43e7aca0c2ff9d5f1bc2a74f4e5bab32704716c8ad7690da435216d6121b5` |
| slow-present | 4 / 3 | `224e224e420a019a02ea34a3ad3d5144c9981016b41ba539a3f27bcdc6449538` |

复跑：

```sh
cmake -S tests/profiler -B build/tests-profiler -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests-profiler
ctest --test-dir build/tests-profiler --output-on-failure
```

Android `shadps4_host` 和 host loader 打包通过。F-Droid Debug APK 构建成功：

- APK `c4185b53efe214952c3a00bd6014b8be0e333294756c00a43dc9813766cb9db7`，
  461,582,134 bytes，保存为证据目录的 `shadps4-c4185b53.apk`。
- APK 内 host SHA 与本地生产 host 一致：
  `42c33f737e8b264335426f6119ab7010f7ffed0798be9a3d81999c2c86f7296d`。
- 本包同时含上轮已构建未安装的静态 C++ mainline Turnip `e61a373f…`；
  与现场 `607e9761` / `d7e6746a…` 并非只改采集的一因素 A/B，不能用后续不同场景 FPS
  直接推导本轮收益。完整身份见 `candidate-identity.json`。
- 收尾只读 `device.get_context(6edb0593)` 返回 `device_not_found`，无在线设备；
  未发起安装或游戏控制。已请求保存进度并连接后确认安装，当前等待用户反馈。

Windows、桌面完整应用、XR 场景与新包实机长跑尚未验证。
