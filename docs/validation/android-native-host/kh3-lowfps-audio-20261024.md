# KH3 / Odin3：长会话低帧率与爆音现场（2026-10-24）

## 身份与采集

用户反馈低帧率、爆音，随后报告降至约 6 FPS，要求再抓一轮。两轮均为同一
Odin3 `6edb0593` / PID 30164 / generation 1 / run UUID
`dbe6e7f341f82d05e6c9a09da171acca`，KH3 CUSA15072 01.10。
APK SHA `35a79908b99ab653eaa959c448699e17b9e01751538cc7cdb7adb01fe1f79828`；
host `732d6391d35fad94cdbdbf4b3fc227621d65766f41d656fcd11c7d3b80d2633e`；
Turnip `68dafb89a26`，driver `d7e6746a…`。未安装、重启、注入输入或修改游戏配置。

本地证据在 `build/validation/kh3-lowfps-audio-20261024/`，第二轮在 `round2/`。
使用驻留 ring dump 0、10 秒 guest PCM、10 秒 simpleperf CPU-clock 199 Hz / FP。
没有开启细粒度/GPU scope 或长期 streaming；PCM 和 simpleperf 已结束。
12:44:00.992 用户设备切后台，最后状态为同 PID 的 Paused/background，未崩溃。
第二轮 ring 和 PCM 均采自此前的 Resumed 状态；后来的桌面截图不作游戏场景证据。

## 主要发现

### 1. 同步地址池确实耗尽，新的对象丢失快路径

日志 12:14:18.392 明确报告：

```text
GuestSyncObjects block 0x1c01ba8000 is outside the fast-path arena window
[0x1d00000000, 0x1d10000000): window exhausted
```

`GuestSyncArena` 每个对象占 256 B，地址直到会话结束都不复用；互斥锁和条件变量共用
256 MiB 窗口，可容纳 1,048,576 个累计对象。窗口用完后按 16 KiB 单独 MapMemory，
仍能正确工作，但 guest mutex.c 的 Object 只认可启动时发布的窗口，后续对象全部
退回 HLE。此警告仅打印第一次；不能从只有一行警告推断只多分配了一块。

第二轮最后 0.85 秒内，两个音频相关线程 Guest-51 / Guest-52 合计有
58,121 次 pthread_mutex_lock/unlock HLE（约 6.84 万次/秒）。
这是一个已证实的长会话性能退化路径，不是 PSO 缓存失效。

### 2. 原生 CPU 热点是地址查询，AJM 受到显著影响

第一轮 simpleperf 4,983 样本、0 lost；第二轮 4,418 样本、0 lost。
Guest-51 上 ValidateRangeLocked 的 self 占全进程样本分别为 17.88% / 22.27%；
第二轮该线程共 1,323 样本，占全进程约 29.95%，其中地址校验约占该线程 74%。
保留下来的调用链为 GuestAjm::Build/Parse → ValidateRange/Read → ValidateRangeLocked。
FP 调用链有 393/1323 的错误链，故不把整棵调用树视为完整；叶子采样仍直接命中该函数。
Guest-52 的同函数另占全进程 2.99%，Waker self 占 13.58%。

源码 FindContainingMappingLocked 只有 64 个直接映射的索引提示，冲突后线性扫描
整个 mappings。同步窗口溢出增加小映射；AJM 又在每个 job 多次检查范围。需要修复
查询的规模退化，同时保持当前权限、区间边界、pin 与映射退休语义。
`/proc/maps` 会合并邻接 VMA，不能拿其行数当 GuestAddressSpace ledger 条数。

### 3. 爆音现场存在持续供音不足

48 kHz、每块 256 帧，10 秒 capture：

| 端口 | 第一轮送入音频时长 | 第二轮送入音频时长 | 第二轮超过 10 ms 的块间隔 |
|---|---:|---:|---:|
| 0x20000001 / Guest-36 / 8ch | 10.000 s | 9.995 s | 0 |
| 0x20000004 / Guest-52 / 8ch | 8.331 s | 6.763 s | 238 |
| 0x20000005 / 同生产者 / PadSpk | 8.331 s | 6.763 s | 238 |

第二轮 4,410 块、0 dropped，端口 4 缺口约 32.4%，最大供给间隔 26.73 ms。
同轮最后 0.85 秒 Audio.QueueWait 仅 2.15 ms / 80 次，生产者并非主要被输出队列反压。
设备 reconnects/stalls/error 均为 0；第一轮 xrun 为 0，starved_frames 持续增长。
说明游戏混音/解码侧来不及供应，足以产生断续声；没有证据把当前现象归为设备断连，
也不是此前 AJM 实例编号用尽的同一个已证实错误。原始 PCM 未做主观回放验收。

### 4. 低帧率窗口主要是游戏提交不足

第二轮尾部连续 16 个 GNM 提交周期为 109.28–167.70 ms，与用户 6–8 FPS 反馈相符。
这是提交周期，不冒充显示时间戳。SDK 161689 为 145.864 ms，owner Guest-31 的
pthread_cond_wait exclusive 共 134.289 ms（约 92%）；最大一次 105.832 ms。
GNM.SubmissionGate 仅 0.00224 ms。相邻三帧也慢，median 126.799 ms。

覆盖上述帧的最后 0.85 秒没有 Pipeline.* scope。当前 driver cache 已加载 62,848,874 B，
预载 2604 条、后续已保存 14 次；新管线偶发等待确实存在，但不能解释这一段持续低帧率。
第一轮 sysfs GPU busy 约 25.5%，未见当前热限频（ThermalStatus 0；旧缓存温度不作当前值）。
GPU faultcount 一直为 15，没有新增 GPU 故障。未采匹配 sched/KGSL/GPU query，
不能把所有 cond_wait 直接归给音频或断言每帧的唯一因果链。

## 修复方向与边界

优先处理同步 arena 超过首个窗口后仍可使用快路径，以及地址区间查询的线性扫描退化。
地址不得直接复用：现有旧句柄必须始终指向已退休对象，不能误锁新对象。
扩大一个固定上限只会推迟复现；扩展窗口需在 guest/host 两端安全发布并隔离 mutex/rwlock。
需以小容量强制越界的 FEX 回归、多映射查询/权限/退休测试验证，再做同场景 A/B。
现场采集时尚未修改生产实现。随后已完成[按需扩容与查询索引修复](kh3-adaptive-sync-20261024.md)，
Odin3 独立回归通过，APK 已授权安装；世界地图短测的帧周期和音频供给正常，
长会话跨首窗后的 FPS/声音效果仍待验证。

## 原始数据完整性

- 第一轮 PROF：67,524,686 B，SHA
  `dfa2c03a3a53f46deb44bb59c87a64658567a1f820f1a083ff119d815386f85e`。
- 第二轮 PROF：67,717,100 B，SHA
  `f29449658457741b81d8c82b1a03fd54ae0bcd72eae7918e1bd00b5610f6d8e8`；
  Litep index `83bc14979ca448af8165f761a8b79c5e`，3,088,096 events、608 完整周期。
- 两轮均 0 skipped chunks、0 scope pairing errors、未截断；保留捕获边界未配对 scope
  和缺失 flow 端点，线程留存时间不同。CPU scope 是 elapsed，不能跨线程或嵌套求和。
- 第二轮 Frame 161689 的完整分析、native SQL 结果、音频 cadence JSON、simpleperf
  原始文件/报告、最终日志均留在本地证据目录；不提交原始游戏音频。
