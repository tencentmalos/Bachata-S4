# KH3 / Odin3：修复包游玩与慢帧续验（2026-10-24）

## 身份与范围

用户保存后授权安装，Odin3 `6edb0593` 已覆盖为 APK `ac5b5a96`（完整 SHA 见
[音频修复记录](kh3-audio-loss-20261023.md)），包含 AJM 编号复用、此前后台恢复修复、
mutex 生命周期优化及内存回收分段。基础主仓 HEAD `22bd5a9d7` 加本地修改；
Turnip 仍为 `68dafb89a26` / driver SHA `d7e6746a…`。

KH3 CUSA15072 update 01.10，PID 24451 / generation 1，run UUID
`b820015f8830eda4a7447bdf16e05d36`。用户开始体验后没有发送手柄输入、重启、暂停、
替换 APK 或修改游戏配置。09:53 的应用截图显示黄昏镇街道、角色和正常 HUD。
结束采集时仍是同一运行会话。音频多声道 PCM 与设备消费正常，长期效果待继续体验。

本地证据：`build/validation/kh3-slowframes-20261024/`。原始 PROF、sidecar、SQLite
索引、分析 JSON、日志与截图保留，不入仓。以下帧均为 GNM 提交周期，**不是上屏 FPS**。
CPU scope 是 elapsed；跨线程和嵌套项不可相加。没有 GPU query 或 sched/KGSL sidecar。

## 样本与完整性

| 文件 | 结果 | 限制 |
|---|---|---|
| ring，默认最近 120 标记 | 119 个完整周期，最大 19.06 ms | 约 2 s，不作为整段游玩的代表 |
| ring-all，显式 dump 0 | 排除启动线程后 1410 个完整周期，平均 18.99 ms，P95 27.93 ms，最大 85.85 ms；>50 ms 1 次 | 完整周期总时长约 26.78 s；首尾跨度 28.67 s，中间 skipped_frame_index 的约 1.89 s 排除 |
| ring-game，黄昏镇 | 连续 1057 个完整周期 / 26.50 s；平均 25.07、P50 24.98、P95 35.70、P99 40.27 ms；>33.33 ms 96 次，>50 ms 2 次，>100 ms 0 次，最大 57.04 ms | 各线程留存不同；不与昨日不同路线作收益 A/B |
| continuous，8 s 上限 | 留存 411 个完整周期，最长 38.49 ms；用于查看新内存埋点 | 507 chunks、54 个 dropped chunk markers；不作为无损频率或依赖链证据 |

前三份 ring 均 0 skipped chunks / 0 pairing errors；窗口尾开放 span 与缺失 flow 端点
仍明确保留。ring-game 的 GpuComm 仅覆盖末尾约 0.130 s，recorder 约 0.958 s，
故无法从早期 57 ms 帧缺少这些线程的 scope 推断它们没有工作。
SDK header main_thread_id=24615 是启动线程；游戏标记来自 24616 / Guest-31。
所有分布排除启动线程帧 1/2、owner 切换、缺标记及最后未闭合周期。

## 管线构建停顿仍存在

ring-all 的 SDK frame 17270 为 **85.8468 ms**，邻帧中位数 **27.0184 ms**：

- recorder 的 Pipeline.WaitReady 完整持续约 83.15 ms，其中帧内 77.23 ms。
- Pipeline.CreateGraphics 完整约 88.86 ms，其中帧内 77.14 ms；新日志对应 key
  `0x53dc2e18266aa87a`、FS `0xb371688e132badc1`、VS `0x0172f7a5f59db076`，
  driver feedback 88.84 ms，时间落在 vkCreateGraphicsPipelines 内。
- submit worker 在帧内等待 recorder 84.47 ms，present 线程等待提交 70.02 ms；
  帧 owner 等 Equeue 66.82 ms。它们是同一段阻塞在不同线程上的表现，不相加。
- 五条匹配的 SDK async flow 中，两个 GpuComm 提交排队 84.05 / 80.56 ms，
  present 提交排队 69.98 ms，支持录制等待向提交队列传播。

缓存正常加载：启动读入 driver cache **61,829,062 B**，预载 **2517** 条配方，
whole preload 596.4 ms，启动前构建批次 44.1 ms。后续遇到 73 条新 graphics pipeline，
创建平均 21.07 ms、最大 88.86 ms；38 次 bind 等构建，平均 24.64 ms、最大 83.14 ms。
这些数据证明既有缓存被使用，新管线仍会产生停顿；不能把 reported cache hit 当作
未发生编译的证明，固定 Turnip 的反馈问题见昨日记录。本轮没有改驱动。

CPU 回读正确性保护仍生效：frame 6175 的缺失内容进入 readback 后，本会话关闭 skip；
没有为降低等待而放开该保护。

## 内存回收

新增分段已实际出现在本次 PROF 中。ring-all 留存的 415 次 PoolDecommit，最大
10.48 ms；对应所有样本的 GPU invalidation 最大 0.970 ms、texture decommit 最大
0.932 ms、texture mutex 获取最大 0.052 ms，未复现昨日 79 ms invalidation 候选。

有丢块的 continuous 中，保留了 1877 次 PoolDecommit，最大 5.186 ms；
GpuInvalidate 最大 2.756 ms、TextureDecommit 2.747 ms、DecommitLock 0.328 ms。
这些仅是留存记录，不代表没有被丢失的长等待，也不是前后优化收益。
本次没有取得 450 ms 对象集中回收的同场景负对照，不能宣称已解决。

## 自动慢帧漏报与修复

09:51:51 左右启用 factor=2 / min_ms=33 / pre=5 / post=2 / count=8 / mib=8，
后续 ring 已记录到 85.85 ms 周期，但监控 detected 始终 0。源码原因明确：
`observe_slow_frame` 按 FrameEpoch 的最初 main_thread_id 过滤，启动线程 24615
之后不再发帧，真正的游戏线程 24616 被全部忽略。

Foundation 的 SDK 修为跟随当前实际标记线程；单一帧源顺序迁移时重新预热，取消未闭合
post 窗口，pre 范围裁到新来源首帧。保持原始 header、wire、已有线程上的帧语义及
try_lock 非阻塞策略。仍不支持多个线程并行交错提交独立帧源。
Foundation 本地提交 `bbe7709b`（`codex/profiler-slow-frame-owner`），尚未推送；
主仓其他音频/会话修改仍未提交。当前设备包不含这项后来补的采集修复。

验证在 Mac 上完成：17 项 slow-frame 单测与 SDK lifecycle CTest 2/2，通过 C++ / Python
smoke 导出逐字节校验；新增两项线程切换测试在原实现均失败。Release encoding perf
8/8，OpenSpec active / archived strict validate 与 diff --check 通过。首次新测试在
producer 退出后才等快照，导致其 TLS 数据消失；已调整为在线程退出前等待快照，不记
首次结果通过。未做这项采集修复的 Android APK/设备验证，以保留用户当前游戏。

无效的自动触发已停止，8 s streaming 已完成，驻留 ring 恢复开启（profiler generation 3，
游戏 session generation 仍为 1），fine/GPU detail 均未开启。没有遗留录制或调试器。

## 原始文件 SHA

- ring：`78ea638c6d09224d125d6985e6b57d1dfc77e532876e427f45b4d00599164898`
- ring-all：`fdd3b9cee2e6d477d380042e3b2ba82a38affac2184fb63a4d65ac15543a0bad`
- ring-game：`c01b27fe15bc0e7b0e11d0c7dfd24435ffda141e9651e6adae83ee31d828b991`
- continuous：`428ae6174ddf92afcc5a8cdef0b9b5e9a0a1f4c05e3cb4cfac573f00bab60186`
