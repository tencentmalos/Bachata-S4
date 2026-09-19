# 血源诊所长等：任务队列自旋与 TSC 预算

2026-09-20；承接[上一轮等待链](buffer-upload-litep-20260920.md)。代码基线 `5ed369e2` / `feature/malos/hle_vr`，本轮没有修改生产锁、时钟或游戏任务语义。

## 已定位的问题

当前最明确的 CPU 优先项是 **CSEzWork 取任务使用的 guest `DLAdaptiveMutex`**。其固定自旋预算为 200,000 TSC ticks；当前 FEX / Orbis 时间接口统一使用 AYN 的 19.2 MHz 原始计数器，因此一次竞争可以先自旋约 **10.417 ms** 才进入 semaphore。这个时间已经被场景 trace 独立复核，不能再笼统归因为角色/物理任务计算过重。

同窗 15,973 次队列 acquire 累计 11,539.659 ms，其中 **9,706.646 ms on-CPU**。这是跨 worker 累计值，不能当成一帧的墙钟时间。实际任务回调累计只有 1,669.902 ms elapsed / 1,044.599 ms on-CPU。队列锁内部的 semaphore HLE 累计 1,168.537 ms elapsed / 31.953 ms CPU，其余大部分 CPU 不在 HLE 内。

最突出的 105.365 ms task-group 等待中，最后通知的 worker 有 **93.760 ms 在取队列锁，任务回调仅 0.184 ms**。等待主要发生在通知之前，并非 condition 收到通知后抢不回 mutex。

## 新采集身份与边界

设备重新连接时运行的是 TMNT，先通过正常 UI Stop，再启动 Bloodborne。确认人物与诊所已经可见后才采 PROF / KGSL；没有使用 loading 性能。自动启动输入已停止，采集窗口无自动输入。短移动输入未形成足够清楚的画面对照，**本轮只确认场景可见，不标记 GAMEPLAY_REVIEWED**。

| 项目 | 本轮有效样本 |
|---|---|
| 游戏 / 配置 | CUSA03023/01.00；Internal Scale 0.5；High 1×1；FDM OFF |
| 设备 / 驱动 | AYN Thor / Adreno 740；Turnip `5ac41be677`，SHA `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09` |
| 运行 | PID18248 / generation2 / context33 / UUID `f0f5906dae902c604909c09cb7a0beb9` |
| APK / host / JNI | 沿用 `c14cf608` / `0d272251` / `20ccbae6`，未重新构建 APK |
| PROF | 31,865,079 bytes；SHA `4ee111141ebf5ddd7255be205c508f13a798d9881e375727d372496b0bd6293b` |
| 解析 | 3,658,893 events；360 chunks / 0 skipped；60 完整、连续 source 帧 |
| 有效窗口 | `[619589751589720, 619601973757741)`，12.222168021 s |
| KGSL | 24 s、384 MiB 总预算、独立 tracefs instance；压缩 trace SHA `360c0abe487f1309bbcaf321415fb8a7deed25cef5305dbe633f178bcf84df00` |
| 时钟 / 调度 | 不确定度1125 ns；完整覆盖 PROF；overrun0 / dropped0 / scheduling_complete |
| guest 探针 | 12 个显式 EH 根、242 probes / 472 sites；profile SHA `f9f61fc96e255b7c02ec3aff8e7e01323436cdded13f8a6a66f836dcb2634e8a` |

PROF 容器有效但 CPU scope 存在边界不完整，保留 truncated 诊断。KGSL 保留1,681条 zero-timestamp batch 观察及一条边界未配对 wait end。探针采集前后 overflow / abandoned / code invalidation 增量均0；ignored ends增量87,999包含录制外回调与没有对应 begin 的后继，不据此宣称所有探针事件无损。分析只使用完整配对，跨边界样本单独排除。

[证据清单](evidence/bloodborne-job-spin-20260920/manifest.json)保存源文件SHA和主要结果；大 PROF / SQLite / ftrace留在 `build/validation/bloodborne-longwait-20260920/`，不提交游戏二进制或构建产物。

## 三层等待关系

GNM source 帧22349长261.384 ms。Guest-19 的 condition elapsed234.886 ms；其中 enqueue→Guest-1 selected-notify234.819 ms，notify→resume0.0443 ms、resume→reacquire0.0227 ms。通知前 Guest-1 的 CPU83.476 ms、sleep134.604 ms、runnable16.739 ms；不能把全部234 ms说成计算。

继续沿 task-group 的显式 selected-notify 找最后完成的 worker，再在同一 lane、同一等待窗口内切分调用：

| Guest-1 任务组等待 | 最后通知 TID | 队列 acquire elapsed / CPU | 任务 callback elapsed / CPU |
|---|---:|---:|---:|
| 105.365 ms | 25812 | 93.760 / 66.028 ms，27次 | 0.184 / 0.157 ms，27次 |
| 92.264 ms | 25813 | 70.107 / 45.796 ms，23次 | 0.148 / 0.121 ms，23次 |
| 85.950 ms | 25815 | 79.282 / 74.188 ms，30次 | 0.131 / 0.131 ms，30次 |
| 77.458 ms | 25815 | 63.992 / 57.863 ms | 0.122 ms elapsed |

这些区间存在于上游 condition 等待内，不再相加到 CPU 帧时长。最后通知者 lane 中的任务不一定全部属于该 task group；当前探针没有逐任务对象/队列对象地址。因此这是明确依赖窗口内的工作分解，尚不是每个任务的所有权图。

```mermaid
flowchart LR
    A[Guest-19 等待帧准备] -->|selected-notify 生产者| B[Guest-1 角色阶段]
    B -->|task-group completion| C[CSEzWork workers]
    C --> D[队列 acquire：固定 tick 自旋]
    D --> E[持锁摘链并释放节点]
    E --> F[解锁后执行任务 callback]
    F --> G[完成计数与通知]
    G --> H[查询所属 allocator 并回收 task]
```

## 锁的静态结构与计时闭环

从原始 SCE RELA 和机器码恢复，最终队列锁虚表是 `+0x53a1990`，其 Lock槽+0x18指向`+0x2037cb0`、Unlock槽+0x28指向`+0x2037e30`。构造阶段先写基类表`+0x53b1a90`，随后替换最终表，两者这两个槽指向相同实现；不能只根据析构的基类表推断运行期虚表。

队列对象+0x30内嵌锁：

| mutex 内偏移 | 字段 | 证据 |
|---|---|---|
| +0x00 | 虚表 | 构造/析构及两个 RELA 表 |
| +0x08 | int32 自旋 tick 预算 | Lock `+0x2037d03` 有符号扩展；队列初始化 `+0x203281f` 写200000 |
| +0x0c | 未知占位 | 不推断语义 |
| +0x10 | semaphore handle | WaitSema / SignalSema 参数 |
| +0x18 | 64位 owner/waiter状态 | 低40位owner，高24位waiter；CAS访问 |

Lock先调用PthreadSelf并读取RDTSC。竞争时反复读状态、CAS，即使期望的新值与旧值相同也执行原子操作；这条循环没有PAUSE。截止后登记waiter，再调用`WaitSema(1, nullptr)`。Unlock在无waiter时清owner；有waiter时递减计数、写低40位全1的交接哨兵，再SignalSema(1)。不能用“返回成功”或只清owner替代这个协议。

551次完整 acquire→WaitSema入口的间隔：

| 指标 | 实测 |
|---|---:|
| 最短 | 10.420980 ms |
| 中位 | 10.427751 ms |
| P90 | 10.501915 ms |
| 平均 / 最大 | 10.538901 / 21.622020 ms |
| 落在10～11 ms | 538 / 551 |
| 少于10 ms | 0 |

独立CNTVCT/monotonic双锚点推得19,199,999.966 Hz，`200000 / frequency = 10.416666685 ms`。上述区间还包含PthreadSelf、探针和调度，所以不是纯自旋CPU；但最短值和分布共同确认了固定tick预算带来的约10.4 ms入睡前延迟。未直接读取运行期mutex字段，不声称所有实例都保持该常量。

当前 `fex_context.cpp` 在Reload后设置`SMALLTSCSCALE=0`；RDTSC/RDTSCP与Orbis ReadTsc/GetTscFrequency保持同一原始域。此前[时钟一致性修复](ring-clock-compiled-guest-2026-09-14.md)解决了FEX单独64倍缩放与HLE混用的错误。**这次发现的是低频域遇到硬编码tick预算的兼容问题，不能直接撤销那次一致性修复。**

## 持锁释放节点与 allocator 竞争

完整 executor 的阶段分布（跨worker累计，不可相加当单帧）：

| call site | 内容 | 次数 | elapsed / CPU / runnable / sleep |
|---|---|---:|---|
| +0x20336f3 | 取队列锁 | 15973 | 11539.659 / 9706.646 / 930.332 / 902.681 ms |
| +0x2033732 | 持队列锁释放链表节点 | 12499 | 1762.716 / 143.041 / 1516.908 / 102.766 ms |
| +0x2033752 | 解锁后的任务callback | 12499 | 1669.902 / 1044.599 / 269.032 / 356.270 ms |
| +0x2033827 | 查找task所属allocator | 12503 | 1409.297 / 533.029 / 553.997 / 322.270 ms |
| +0x2033841 | task最终释放 | 12499 | 484.667 / 191.448 / 239.354 / 53.864 ms |

`+0x2033732`确实在队列解锁调用`+0x203373f`之前。其大部分 elapsed 是 runnable：线程持有guest队列锁时没获得CPU，其他worker仍可自旋。持锁区和调度延迟证据明确；但缺少每个队列实例地址，不能把任意同时自旋者与任意持锁者都连成已证明的同锁边。

`+0x207cb10`先走TLS分配器范围快查，否则在DLReadWriteLock下查范围表。回收每个细碎task都可能再查全局allocator。后续应优先验证把摘下节点的释放移出队列临界区，并保留allocator生命周期，或复用节点；不能盲目用无锁容器替换。

## 实际任务分支与 native 旁证

本次启动累计：EnqueueCharacterVirtual108与`+0x18c0030`入口均161,742次，TargetBankUpdate919次；CharacterComponentUpdate入口和其enqueue均0。说明此前猜测的component-update分支不是这个观察窗口的主要命中分支。`+0x18c0030`再走虚表+0x150；原始RELA只给出`+0x18ca2a0`和`+0x18e6b20`候选，尚无运行期对象/vtable身份，保留候选，不假装已恢复实际主体。

关闭探针后的10秒、199Hz simpleperf共有8,610 samples；Guest51～56都集中在同一组JIT地址。当前没有把这些host JIT PC映射回精确guest PC，故只作为独立的共同热点旁证，不把具体PC直接命名成该锁。

## GPU仍有独立成本

同窗785条Vulkan Post→WorkerSubmit均匹配：排队P50 0.04073 ms、P99 85.062 ms、最大93.114 ms。KGSL完整submitted→retired798批，最大96.994 ms；最长wait97.121 ms、仅0.018 ms on-CPU。flow93861在worker TID24876等待时到达，等待结束约0.329 ms后开始处理，排队90.417 ms。

因此GPU/驱动尾部等待仍会挡住提交worker，不能声称CPU问题修复后就能达到目标帧率。KGSL批次时长包含排队/fence/完成上报，不等于shader busy；这条队列不与前面的角色等待相加。

## 工具修复与可复用产物

本轮发现guest batch自带Python decoder不认识新SDK的`Post / PostDyn / SpanBeginFlow / SpanBeginFlowDyn`。同一PROF旧解码只读到6,552事件和一个帧标记，导致“没有完整guest帧”；C++ Litep reader本来能完整解析。修复已按SDK `e00321e`真实wire布局补齐，保留flow ID、动态名称和参数，并对未知协议明确拒绝。

同时修复两处分析缺口：录制边界实际由decoder产出Instant，原先只匹配Bookmark；帧只接受同lane、连续SDK帧号和正时长，跳号不制造假长帧。新增worker lane的inclusive/union观察及明确标注来源的后续探针根，避免只在发FrameMark的线程找guest工作。worker权重不是关键路径，也不自动部署下一轮探针。

工具源提交`7f9da023`（dev_tools / codex/litep-profiler）。35项针对性检查通过；本地`reverse_study 0.3.0-local.20260920.asyncjobs1`已通过package manager安装。实际installed MCP调用恢复60完整帧、3个实测后续根；安装内3个解码/分析源文件与工作树SHA相同。Codex的6个既有产品核验为当前版本，只更新reverse_study入口并保留所有自定义args/env；Doubao全部11 wrappers已重新生成并检查，10项initialize/tools-list通过，独立webaccess因自身启动失败未通过（见归档），不宣称11/11。该失败不影响实际guest batch验收。12根探针生成器从原始EH核对函数范围，机器码逐字节校验；两个跳转表尾明确排除。重新生成242探针得到同一SHA。新增9个重建函数名和2个结构体，符号索引revision2共16函数/2类型，exact-SHA校验通过，C11结构体size/offset断言编译通过；没有编译或部署guest替换。

开启/关闭诊断的背景试验得到OFF5.223 / ON5.143 / OFF5.147 FPS；它是已插入探针但disabled与enabled的对比，且没有同时写PROF，**不能证明本次录制无扰动，也不是未插桩基线或修复后提速**。

## 下一步实施顺序

1. 首先做时钟/自旋预算的受控验证。可先对这套exact-build mutex做仅缩短入睡前自旋预算的对照，保留CAS、waiter登记和semaphore交接；再设计RDTSC/RDTSCP、CPUID、ReadTsc、GetTscFrequency及ProcessTimeCounter共同的高频guest时钟域。禁止只开FEX缩放或直接跳过等待。
2. 移出队列锁内的节点释放前，补齐队列实例/allocator生命周期和并发释放契约；这比继续调epoll或condition更贴近当前证据。
3. 用同视角、未插桩基线和实际场景内采集复测：成功present间隔、队列acquire CPU、10.4ms入睡前平台、worker runnable、GPU排队分别比较。保留Stop/销毁唤醒、双时钟一致性及竞争回归。

设备最终正常UI Stop，`Stopped / user_stop`、TracerPid0；next-session探针属性已清空、profile_sync0、GPU timing OFF、capture inactive、ring ON；独立tracefs已删除，全局tracing状态不变。只关闭本轮Reverse Study workspace，无自动输入或调试器遗留。本轮是进一步定位和工具修复，没有声称游戏性能已修复或完成全游戏回归。
