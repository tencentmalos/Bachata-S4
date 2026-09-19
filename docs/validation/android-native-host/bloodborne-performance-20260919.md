# 血源 Android：Litep 性能定位与离线轮询

本轮先按用户要求采 Litep。结论是当前场景的帧产出更像卡在 guest CPU 任务与同步链：每帧约 344 ms，而 GPU guest 工作约 80 ms。负责 GNM 提交的 Guest-19 大部分时间在等待条件变量。网络线程还存在已确认的空转：请求 33 ms 的空 epoll 几微秒就返回。不能把条件变量等待解释成 mutex 实现本身消耗了 302 ms，也不能把网络空转直接当作主帧停顿的唯一原因。

## 采样身份与范围

- AYN Thor `9c2841a4`，Adreno 740，Android 13；Bloodborne `CUSA03023`，猎人位于室内病房，截图见证据目录。
- PID `12119` / generation `1` / UUID `4033c9e667c119a051f861da9ea6b4ec`。APK `cbbbe44b89e182058eb67a0c475cbde4697fc2856fc26c6d1e379023171301cd`。
- Turnip Mesa 26.0.0-devel `5ac41be677`；实际库 SHA `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`。
- Internal Scale 0.5、High / 1×1、FDM OFF。本轮没有自动移动或战斗输入，不是固定脚本的全游戏基准。
- 有界 20 秒 PROF，CPU 时间覆盖 19.76534073 秒，原始文件 58,385,397 字节，SHA `9cb9a2b56c91799adf0a05d70ede15cf68fc93e6333d881de8733a3593910450`。
- 7,275,376 事件，704 个完整 chunk / 0 skipped，57 个完整 SDK 帧。采集首尾有未配对 scope，`truncated=true`；统计仅使用已配对区间，保留诊断，不宣称全量无损。
- GPU 有 1,063 对 zone、0 orphan times，但 CPU/GPU 时钟未校准。GPU elapsed 可用，精确跨时钟关键路径不可据此认定。

原始 PROF 和 2.7 GB SQLite 留在 `build/validation/bloodborne-perf-20260919/`。可复核的小型摘要、脚本、日志和截图存放于 [evidence/bloodborne-perf-20260919](evidence/bloodborne-perf-20260919/manifest.json)。帧明细接口拒绝该窗口的边界歧义，未强行伪造成功；采用完整 trace 的索引配对区间和明确时间范围的 stage reducer。

## 实际耗时

| 指标 | 测量 | 含义 |
|---|---:|---|
| VideoOut 新 guest 帧 | 2.910 FPS，均值 343.628 ms，P95 424.467 ms | 实際呈现计数，不使用 overlay 重绘次数当 FPS |
| GPU.GuestFrame | 55 次，均值 79.643 ms，P95 81.473 ms | GPU 也很重，但不足以单独解释 344 ms 的产出间隔 |
| GPU.HostPrepare | 0.359 ms / 次 | 含嵌套 PostProcess，不重复相加 |
| GPU.Present | 0.605 ms / 次 | GPU 命令区间，不等于 CPU 的 vkQueuePresent 调用 |
| Guest-19 条件变量等待 | 57 次，共 17.216 s，均值 302.036 ms | 等生产者／通知者；地址、caller 和 notifier 尚未记录 |
| Guest-19 GNM.SubmissionGate | 754 次，共 10.426 ms | 全窗口总量；不能沿用 TMNT 的长门控瓶颈结论 |
| GpuComm PM4.Resume | 共 2.644 s | Host 命令生成 elapsed，与别的线程重叠，不代表独占 CPU |
| Submit worker vkQueueSubmit | 1,398 次，共 4.825 s，中位数 0.070 ms，最大 83.237 ms | 异步 worker 确有 GPU 等待型长调用；尚未证明其阻塞主帧 |
| Submit worker queue lock | 共 24.756 ms | 包含一个 12.325 ms 离群值 |
| Presenter queue lock | 共 0.213 ms | 不是本次 300 ms 等待的直接解释 |

代表 SDK 帧 4582，333.535 ms：Guest-19 从 +0.430 到 +298.864 ms 等条件变量；异步提交的长调用发生在 +2.669 到 +82.948 ms。两者为同一 CPU 时钟，但没有条件变量对象身份，不能据时间重叠就断言唤醒关系。

Guest-1 的完整 HLE 区间覆盖共 5.236 s，约占窗口 26.5%。其余约 14.529 s 包括未标定的 guest 执行、FEX 转换与调度时间，不能全部命名为 on-CPU。该线程 mutex Lock/Unlock 各 144,498 次，共 1.359 s；条件变量 174 次共 2.410 s。后续应围绕生产任务、条件变量地址、通知者和准确 guest caller 补充 Litep 标定。

## 网络空转：确认到参数和调用顺序

Guest-45 / TID 12279 在窗口内执行以下循环，各入口均出现 543,537 次：

`sceNetEpollWait → gettimeofday → scePthreadMutexLock → scePthreadMutexUnlock`

约 27,500 轮/秒，即 110,000 次 HLE 跨越/秒。四项完整 HLE elapsed 分别为 1.906、1.024、1.486、1.222 s。这些并不是额外相加到每帧的时间。

原 PID 保存的日志尾部有 46 条一致记录：`extnetwork2_udp, maxevents=1, timeout=33000`，单位为微秒。不是 timeout=0 的正常主动轮询。`guest_network.h` 原实现转入桌面 `sceNetEpollWait`；后者只在存在 socket 时执行 native wait，空集合直接返回。因此这条 33 ms 请求实际平均仅约 3.507 µs。

直接返回成功已经是原来的行为，会继续触发游戏自己的时间查询和互斥锁。不能在 HLE 里凭空跳过这些后续 guest 调用。用户不需要联网，本轮采用仅离线的本地 epoll 对象：不创建 host socket/epoll、不调用桌面逐次日志；零 timeout 立即返回，正值按微秒等待，负值等待取消。Abort 唤醒现有等待，Destroy 和 Session Stop 也能唤醒；共享对象维持在途生命周期，等待期间不持有 domain/VM 锁或 guest 输出 pin，不伪造网络就绪。

## 最初样本的观察限制

没有调度 trace，因此不能报告精确 on-CPU 占比或已经证明的完整关键路径。捕获有 tracing 开销，未做探针开关 A/B/A。最初启动的 simpleperf 在停止请求前已自行结束，本报告不采用其数据；后续只用 Litep。

原采样完成后已恢复 GPU timing OFF、ring ON。分析期间设备进程先换为 PID 21111，随后退出至桌面；这些状态不混入 PID 12119 的统计。安装修复前服务已不存在，本轮无需强停。性能改善以修复后同场景数据为准，不从调用次数直接推算 FPS 收益。

## 后续验证

本页保留最初 PID12119 的证据边界。后续已补齐同次 KGSL/sched、显式 selected-notify、SDK async flow 和完整帧贡献分析，见 [后续报告](bloodborne-cpu-gpu-sidecar-20260919.md)。新现场不支持 epoll 持全局锁的判断；提交线程主要等 guest 生产者通知，生产者又包含实际执行、调度延迟及对 worker 的等待。不能将旧样本“没有调度数据”的限制套用于后续同次 sidecar，也不能把不同场景 FPS 直接作 A/B。
