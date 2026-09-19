# Buffer 上传与脏页锁拆分

日期：2026-09-20。分支：`feature/malos/hle_vr`。承接 [Azahar/Citron 对照](buffer-cache-concurrency-comparison-20260920.md)；旧工作先在 `f8d264e5` 提交、推送，再创建新分支实施本轮改造。跨仓交付见 [交付记录](git-delivery-20260920.md)。

## 本轮结果

`StreamBuffer::Map` 的 staging 分配、容量背压和 GPU 退役等待，已移出全部 `RegionManager` 脏页锁。CPU 缺页入口不再遍历 renderer 正在修改的 `buffer_ranges`。原有 CPU/GPU 数据所有权、必要的上传复制和 GPU 完成约束继续保留。

Android 定向检查 **58 项通过、0 失败**；host 与 APK 构建成功。新 APK 在 AYN Thor / Turnip 上进入血源诊所，触屏短时移动有可见响应，最终正常 Stop。10 秒 native 样本的成功上屏率约 **5.695 FPS**，仍然很低；没有匹配场景的 A/B/A，本轮不声称整游戏提速，也没有完成完整游戏回归。

## 上传事务

`MemoryTracker::SnapshotForUpload` 将原有任意 `on_upload` 回调拆成明确的两个阶段：

1. renderer 先准备稳定区域描述和局部 scratch；按地址顺序锁住相关区域，统计当前 CPU 脏页，再释放全部锁。
2. `prepare` 在锁外预留 staging 和 copy 元数据。继续使用原有 512 MiB staging ring，容量不足时可在此等待；超出 ring 容量仍走既有临时 buffer 路径，没有新增无预算的备用池。
3. 重新取得相关锁，重查脏页。若等待期间新增的脏页超过容量，释放锁后按整个查询的页对齐大小预留，因而最多两次准备；不能清掉 dirty 后才发现空间不足。
4. 锁内只做页面保护、必要的 RAM→staging 快照和脏状态交接。所有复制成功后，才发布 GPU dirty。复制失败会恢复原 CPU dirty 位及 write watcher，随后释放锁；预留失败不修改 dirty。
5. 锁外 flush/commit 实际使用的 staging 前缀、登记退役，以及记录原有的一次 `copyBuffer` 和屏障。没有添加额外 memcpy、blit 或渲染 pass。

等待期间若重入回调已经同步了该范围，外层重查可以得到零字节，不再上传过时列表。事务 scratch 属于当前调用，不能共享一个 renderer 成员列表。`StreamBuffer::Commit(used_size)` 只发布实际复制的前缀，避免把扩容预留的空洞也提交。

```mermaid
flowchart LR
    A[统计脏页并解锁] --> B[锁外预留 staging / 等待退役]
    B --> C[加锁重查容量]
    C -->|不够：解锁| B
    C --> D[保护页面 / 快照复制 / 交接 dirty]
    D --> E[解锁]
    E --> F[flush / 命令记录 / 退役登记]
```

CPU `InvalidateMemory` 直接查询稳定原子发布的 tracker 区域。不存在、未监视的页面无需访问 renderer 的 interval tree；真实 GPU dirty 回读仍沿既有请求路径执行。此修复没有把历史崩溃归因于那次容器竞争。

## 验证

生产头文件定向测试使用真实 `MemoryTracker`、`RegionManager`、位图和 Android futex；PageManager 的 OS 页保护调用被 watcher 计数桩替代。另一个 futex 套件包含真实 mprotect/SIGSEGV 场景。

| 检查 | 结果和覆盖 |
|---|---|
| Futex | 14/0；竞争、唤醒与真实 fault 路径 |
| MemoryTracker | 44/0；独立分区、跨分区/目录/地址边界、同区域写者在 staging 等待期间推进 |
| 容量与异常 | 新脏页触发有界重查；预留失败不清 dirty；跨区域复制失败精确回滚；重入后零上传 |
| Precise 回读 | GPU read watcher 在快照后发布；flush 回调可重入 tracker；CPU/GPU 位和 watcher 数恢复 |
| 构建 | NDK 29、API 33 host 成功；`:app:assemblePlaystoreDebug` 成功 |
| 工具交付 | 本轮交付前 Litep 86/0，guest tools 30/0；不重复旧版本的全量测试结论 |

命令入口为 `scripts/android/test-buffer-tracking-locks`、`scripts/android/build-host-android` 与 Android Gradle 任务。原始输出无损压缩、SHA 和源码身份见 [manifest](evidence/buffer-upload-unlock-20260920/manifest.json)。APK 构建后仅调整两处 C++ 换行缩进。

设备为 AYN Thor `9c2841a4`，血源 `CUSA03023 / 01.00`；Turnip Mesa `5ac41be677`，Internal Scale 0.5、High/1×1、FDM OFF，GPU timing OFF。APK SHA `c14cf608e903afc6cb5b9515605e9bba3c89d00faa7e978ebdcc384d52211e83`；host `0d272251ec5c1f5886c855faf434db60c04e44195cd3bb5211da454c49b33000`；JNI 未变，完整身份见 [identity](evidence/buffer-upload-unlock-20260920/identity.json)。

本次 PID **6163**、generation **1**、run UUID `cb7f937ed48da57e5332fde966b3484e`。实际走过启动、加载和诊所画面；触屏左摇杆 700 ms 后人物位置/朝向变化，截图 [输入前](evidence/buffer-upload-unlock-20260920/runtime-3.png)、[输入后](evidence/buffer-upload-unlock-20260920/runtime-move.png)。这不是完整战斗、存档或长期稳定性验证。

native `cpu-cycles:u / 199 Hz / FP / 10 s` 采样共 **8115 样本、0 reported lost**，前后 PID/generation/UUID 一致。host `Common::SpinLock::lock` 聚合 self 为 0.05%，不代表所有锁成本或具体锁持有时长；大量热点仍是匿名 guest JIT 地址，不能据此命名 guest 函数或断言 GPU 无瓶颈。原始 `.data` 留在本地，仅提交 SHA 与文本报告。

首次很短的 Stop 点击后对话框消失，但状态仍 Running，因此没有把它算成功；重新打开对话框并使用 180 ms 点击后得到 **Stopped / user_stop**。最终 TracerPid=0，没有遗留自动输入、采样或调试器。

## 保留边界

- 实际快照复制仍持有相关区域锁，多区域上传也仍有相关范围的串行化；并非整个 Buffer Cache 无锁。CPU/GPU dirty 的数据一致性关系必须保留。
- staging ring 耗尽仍可能让 renderer 等待，只是不再借脏页锁阻塞 guest 写者。复用仍要求 GPU 完成及对应 host submit 返回，没有放宽异步退役约束。
- `CopySparseMemory` 有自身内存映射同步；texture cache mutex、GPU 回读和其他 scheduler 路径不在本轮全部消除范围内。
- 普通设备验证未主动制造真实 Vulkan staging 耗尽、分配失败或完整 Precise 游戏回读；对应锁协议由定向测试覆盖，不能冒充这些 GPU 场景的实测。一般性的 StreamBuffer/scheduler 重入和缓存对象生命周期仍需另行审计。
- 后续优先把匿名 guest worker 热点与 exact-build guest 逻辑对应，并补可选的锁对象/持有者/等待原因关联；不能只凭 FPS 或时间重叠继续删除同步。
