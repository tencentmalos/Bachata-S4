# 血源 CPU 热点：Buffer Cache 区域锁、修复和剩余边界

采集开始于 2026-09-19，复核和源码对照完成于 2026-09-20。接续 [Litep/KGSL 长帧分析](bloodborne-cpu-gpu-sidecar-20260919.md)。对 Azahar、Citron 的架构比较和下一阶段方案见 [缓存并发对照](buffer-cache-concurrency-comparison-20260920.md)。

## 主要结论

已定位一个真实的主机 CPU 开销：多个 guest 线程在图形写保护缺页路径竞争 `RegionManager::lock`，Android 的 fallback 是纯自旋。基线中 host `__aarch64_swp1_acq` 的 self 占 CPU-cycle 样本 **37.60%**，另有 `Common::SpinLock::lock` self **0.67%**。完整 native 调用链可见：

```text
Guest-1 / Guest-20…24
  FEX InterruptFaultHandler
  GuestGraphics access-fault callback
  SignalDispatch::DispatchAccessViolation
  Vulkan::Rasterizer::InvalidateMemory
  BufferCache::InvalidateMemory
  MemoryTracker::InvalidateRegion
  RegionManager::lock → SpinLock → __aarch64_swp1_acq
```

37.60% 是该 host 原子函数的聚合采样份额，不是直接测量某一把锁的等待时间，也不能把每个原子样本都精确归到同一个对象。它与上述完整链和源码共同证明严重的 fault-path 自旋竞争；不是“guest 算术计算占了这么多时间”。

本轮 Android 已改为 **256 KiB 区域 + 原生 futex 等待**。最终静止场景样本中相同原子热点降至 **0.03%**，移动后的慢窗口为 **0.02%**。消除了已定位的大量空转，但**血源低帧率未解决**，也没有同场景 A/B/A 的 FPS 提升证明。

## 为什么看起来像全局锁

- 原区域大小 4 MiB，一把锁覆盖 1024 个 4 KiB 页面。不同页面的写者存在不必要的互斥。
- `ForEachUploadRange(is_written=true)` 同时保留查询覆盖的全部区域锁，直到 `on_upload` 结束并发布 GPU dirty。这个回调经过 `UploadCopies → StreamBuffer::Map → WaitPendingOperations → Scheduler::Wait`，容量复用时可能等待 GPU。
- 因此单个 GPU 可写 buffer 上传会影响整个被覆盖区域内的其他 guest CPU 写者。worker 无法完成时，等待 worker 的主线程/提交线程也受到影响。
- 非 Android 的 GNU adaptive mutex 条件在 Bionic 上不成立，旧代码走 `Common::SpinLock`。因此等待不仅拖长 wall time，还消耗 CPU、增加其他任务的 runnable 延迟。

这不是已证明的“整个进程一把锁”。当前证据未记录具体锁地址、持有者和持锁跨 GPU wait 的实际时长；后者目前是源码证明的可达路径。

## 本轮实现

| 文件 | 变化 |
|---|---|
| `src/common/futex_mutex.h` | Linux 原生 futex 三态 mutex，竞争时睡眠；处理 EAGAIN/EINTR/伪唤醒并保存 errno。无库级 waiter table、动态分配或额外全局锁。 |
| `src/video_core/buffer_cache/region_manager.h` | Android 选用 FutexMutex；池对象只在发布前 Initialize，避免覆盖已有锁对象的构造生命周期。 |
| `src/video_core/buffer_cache/region_definitions.h` | Android 区域 4 MiB→256 KiB，64 页共用一把锁；桌面区域尺寸和锁选择不变。 |
| `src/video_core/buffer_cache/memory_tracker.h` | 两级原子发布目录，保持顶级索引约 2 MiB；renderer 创建、fault 侧 acquire 查询。脏位查询也受区域锁保护。 |

`PageManager` 的锁粒度从同一常量派生，因此也变细；它仍是 SpinLock，未在本轮全面重构。其覆盖 1 TiB 的锁数组会由约 256 KiB 增至约 4 MiB，不能宣称此改动没有内存成本。MemoryTracker 对象实测 2,097,256 字节，避免把顶级指针数组直接扩至约 32 MiB；还需按需叶目录和已创建 manager 的空间。

FutexMutex 是非递归锁，只沿用“fault 不会打断一个已经持有同一把锁的线程”的既有约束。renderer 的 guest 数据访问使用不受 GPU watch 保护的 backing alias。不能将该类描述为任意 signal handler 都可安全使用的通用锁。

**仍保留的边界：** GPU 可写上传仍跨 `on_upload` 保留区域锁；多个相关区域仍可能一起阻塞。`Scheduler::Wait` 必要时 Flush，并可能执行已退役回调，进一步说明这个回调边界应该拆除。检查的新脏位查询锁没有在所查 FindBuffer/资源删除回调中找到同锁重入，但没有完成全部 renderer 的重入证明。

## 有意义的针对性验证

命令：

```sh
scripts/android/test-buffer-tracking-locks \
  --ndk /Users/bytedance/Library/Android/sdk/ndk/29.0.14206865 \
  --serial "$DEVICE_SERIAL" \
  --out build/validation/bloodborne-cpu-jobs-20260919/formatted-tests
```

- Futex：**14 checks / 0 failures**，160,000 次受保护递增；覆盖多线程竞争、try-lock、errno、EINTR、独立锁进展和真正 `mprotect → SIGSEGV → 竞争锁 → 恢复`。
- 持有者睡眠、6 个竞争者的示例 CPU 时间：格式整理前 spin 244.420 ms / futex 0.213 ms；复测 740.204 / 0.318 ms。没有把容易波动的时间比值设为测试通过门槛，也不用于计算游戏提速。
- 生产 MemoryTracker/RegionManager：**18 checks / 0 failures**。只替换 PageManager 的 OS protection 回调和 settings 生命周期。证明同一个旧 4 MiB 内相距 256 KiB 的写者可独立推进，同区域写者正确等待；覆盖跨分区 CPU/GPU 状态、4 MiB 目录边界和 1 TiB 上界附近。
- 用旧 HEAD 的 tracker/region 头文件运行同一用例：**18 checks / 2 个预期失败**，分别是旧尺寸和独立写者被阻塞，构成实际反例。
- Android host 与 APK 构建通过；本轮共 **32 项针对性检查通过**。测试不等于完整 Vulkan、所有 readback 模式或整游戏并发正确性证明。

## 真机采样与限定

设备 AYN Thor / Adreno 740，Turnip `Mesa 26.0.0-devel (git-5ac41be677)`；Internal Scale 0.5，guest shading High/1×1，FDM OFF。游戏 `CUSA03023`、`APP_VER 01.00`，初始病房场景。

采样使用 app 身份的 simpleperf，`cpu-cycles:u`、199 Hz、10 秒、frame-pointer callgraph。原始数据、脚本和截图位于 `build/validation/bloodborne-cpu-jobs-20260919/`。没有并行运行其他 device 基准的采样才用于下面的结论。

| 样本 | PID / generation | 样本数 / 丢失 | FP 回溯错误比例 | host 字节交换 self |
|---|---|---|---|---|
| 原实现 `native-cpu-baseline.data` | 20429 / 2 | 8174 / 0 | 40.14% | 37.60% |
| 仅 futex 的干净复测 `futex-only-clean.data` | 11317 / 1 | 8226 / 0 | 18.733% | 作为中间参考，未宣称完整修复 |
| 分区+futex `sharded-final.data` | 13889 / 1 | 8518 / 0 | 19.453% | 0.03% |
| 同进程移动后 `sharded-post-input.data` | 13889 / 1 | 4619 / 0 | 21.087% | 0.02% |

FP 错误和匿名 JIT 区域均保留，零 lost samples 不代表所有调用栈完整。第一次 `futex-only.data` 与独立并发反例短暂重叠，因此不作为验收样本；之后已重采 `futex-only-clean.data`。

实际 presents：原实现另一独立 15 秒窗口约 **4.648 FPS**；futex-only 干净窗口约 **5.598 FPS**；分区最终静止窗口约 **5.525 FPS**。相机/场景没有严格匹配，**不能据此声明提速比例**。

最终一次约 0.7 秒的触屏摇杆输入后，角色姿态/朝向发生变化，随后 10 秒慢窗口仅 **1.887 FPS**，但原自旋热点仍只有 0.02%。稍后 Stop 弹窗截图 overlay 又约 6 FPS，显示明显阶段波动。新的慢窗口分布到 mutex、内存访问校验、FEX/HLE 桥接和匿名代码，尚缺同窗口完整 sched/GPU 配对，不能仅据这份 on-CPU 抽样宣布新的唯一瓶颈。

静止最终样本里的匿名 PC `0x55bac04138/3c` 合计约 20.85%，集中于多个 worker；尚未映射为 exact guest 函数，不能称为“物理计算”或“guest 自旋”。

## 继续定位 guest 等待链的静态成果

已保存 [7 个重建符号](../../../guest/games/CUSA03023/01.00/symbols/index.json)（仓库实际路径 `guest/games/CUSA03023/01.00/symbols/index.json`），并经 `guest_debug.symbols_prepare` exact-source dry-run：7 symbols / 0 types。它们是 correlated/candidate，不是假冒厂商符号。

- `+0x20336c0`：从 CSEzWork 队列取任务；队列锁在 `task vtable+0x10` 执行前已释放；任务结束后 group 计数减一，最后任务通知。与旧运行 Guest-56 的 `+0x203377c` 返回点相关。
- `+0x20353b0`：带 Havok thread-memory 初始化的 worker loop；线程初始化方式不证明所有任务都属于物理。
- `+0x19177f0` / `+0x1915760`：角色更新和行为/worker 阶段协调候选；关联已有长等待返回点。
- `+0x1a659d0`：WorldAi member task；`+0x1a65290` / `+0x1a65430`：虚表 D0/E8 槽任务。依据原始 SCE RELA 恢复 execute 槽，不能把文件中的零填充当成最终 vtable。

SELF SHA `6764938b23539d29c936bca9880fc4a774e7b0099ce31c7e8c4b0f8bd0befb80`；解密原始 ELF SHA `cc2826ae36b4df515d4b2f4ff9cb18a24057165d95cc51c3413ff98adf704209`。只在分析副本补 ET_EXEC/节表/EH 起点符号，LOAD 字节不变；不把该副本写回游戏。

Reverse Study 未完整自动识别部分 coordinator，因此没有生成并部署新的自动探针，也没有替换 guest 函数。两个本轮分析 workspace 已关闭。

旧帧 2884 的某次 main-owner 条件等待从帧内 +273.679 ms 延伸到 +320.319 ms，帧本身在 +286.169 ms 结束；本帧贡献约 12.490 ms，不能把完整 46.639 ms 都计入这一帧。该片段 Guest-56 的约 10.503 ms sleeping 也没有证实位于 semaphore HLE，仍保持未知。

## 工具结论与交付状态

Litep 的“on-CPU、HLE scope 之外”可以包含 host 图形 fault handler 自旋，不能直接归为 guest 游戏算法；这次有界 native 抽样补上了这一盲点。未在本轮新增 Litep 的 native-PC→guest-PC 映射能力；之前 KGSL sidecar、async flows、帧贡献工具成果仍以此前报告为准。

最终安装的分区+futex APK：

| 产物 | SHA-256 |
|---|---|
| APK | `12990b6f20356357974e8c1c277683c8b149815bd060fcfe021fea0daecf9fdb` |
| host | `9a39859b36cd0fe19a1dcfb65428cd1188deec2ff25799ca8439450f7bb598c8` |
| JNI | `20ccbae630a233a492eb01c1e74a4ef1afe6b3ed6ab3cb5a91e1dad20a0530bf` |

PID13889 / gen1 / UUID `1054174aff943978237aae03aebefe52` 最后通过 UI Stop 正常结束，`Stopped/user_stop`；未 force-stop。停止前 ring ON、GPU timing OFF、file capture idle、profile_sync=0、auto-tag disabled_at_startup、TracerPid0；没有遗留自动输入或采样。格式整理不改变上述已测实现逻辑，整理后重新运行 32 项检查通过。

本轮没有修改 Foundation/FEX 子模块、Azahar/Citron；保留其他已有工作树修改。**尚未 commit/push，没有完整游戏回归、长期稳定性或整体性能修复声明。** 小型可复核证据保存在 [证据目录](evidence/bloodborne-buffer-locks-20260919/)，大型 perf/APK/ELF 仅留本地并由 manifest 记录哈希。
