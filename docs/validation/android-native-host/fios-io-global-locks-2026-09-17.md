# FIOS I/O 串行化修复与 VM 总锁复核

2026-09-17，`codex/android-fex-round2` / `4da582b7` + 保留的既有 dirty tree。本轮未 commit/push，不修改 FEX/Foundation 子仓，不做完整回归。

## 结论

已移除普通文件 I/O 的两层跨请求串行化；新增普通数据映射也不再需要暂停所有 guest owner、drain 图形提交并全量清除 FEX 译码。**Runtime 的 vm_mutex 仍存在，完整移除尚未完成。** 已对照 desktop 整理[删除总锁的接口与生命周期设计](../../guest-memory-concurrency.md)，不能用另一个同等作用的全局锁替代。

真机正常 APK 能到 TMNT 屋顶，但 loading 长时间无新帧仍存在，未宣称加载/FPS问题修复。诊断捕获中最长25.867秒停帧期间没有全局 VM 停机、图形 drain 或译码清空，同时后台读取287MB，仅读取 syscall累计135ms。应继续跟 FIOS 读后处理/解压、FMOD任务完成与通知链；不能把主线程同步等待简单当成 host 读盘慢，也不能提前返回 flushCommands。

## 实现

### 存储层

`GuestStorage` 将原 files 全局锁拆成短 handle 表操作 + 稳定 `shared_ptr<File>` 租约。普通 preadv 直接走 native preadv，同一描述符也不需要 cursor lock；read/seek 等修改文件偏移的操作使用该 File 的 cursor mutex。stdio 使用各自同步。关闭 guest handle 后，已入场的 I/O 保持原 native fd，不会碰到复用后的 fd。挂载卸载能看见仍存活的关闭后租约。

关闭现在返回“guest handle 已退休”，native close 在最后一个 File 租约销毁时执行；不能把异步退休后的 close errno 回传给早已返回的 Close。原生 fd 仅由 File 所有，不进行 close 重试。

save 扩容/写入/truncate 使用每存档卷独立 quota lock，目录用量计算与目录变更遵守同一 quota；不能把不同存档卷/只读游戏文件串起来。namespace 仍有元数据 shared_mutex，readonly Open/Stat 可共享，mount/unmount/create 等写操作独占；这是尚存的目录元数据同步，不声称整个 filesystem 无锁。

`DispatchStorage` 仅在完整参数及 pin 集合的 admission 中持短 VM gate，随后在 syscall/文件游标等待前释放。pin 退场不重新取得 VM gate。posix errno 写回延后到内部 pin 全部销毁之后，防止“VM drainer 持 gate 等 pin、I/O 错误出口持 pin 等 gate”。新的批量内存 admission 接口落地后，剩余短 VM 参数也应删除。

### 数据映射

新增 backend-free `GuestAddressSpace::MapFreshData` / `ProtectData`。

- Map 仅非可执行、未映射地址；拒绝覆盖、无效范围/权限/backing。允许其他 owner 的 execution lease 和无关 I/O pin 存活。
- 新 stack guard 可只修改自己的非 executable 范围；重叠 pin、改入/改出 executable、跨 hole 等请求被拒绝。
- 不调用 CodeInvalidationSink、不修改 code_generation；ledger 分配先于 OS mmap，mmap失败不留下未登记映射。
- 新 owner stack/TLS、mutex/rwlock/semaphore/threadattr、AvPlayer scratch、guest heap trace、entry params，以及 NoFixed / Fixed+NoOverwrite 的非 executable guest map 接入。物理分配不操作 guest VA，只用 MemoryManager 自身账本同步。
- VmGuard 的 Data 路径当前仍持 runtime 元数据 gate；代码发布、旧地址 unmap/remap、通用 mprotect 仍保留旧 transaction。这是过渡实现，不是完全移除 VM 总锁。

新增 VM tags：MetadataGateWait、QuiesceAllOwners、DrainGraphicsSubmissions、FullCodeRetirement、MapFreshData、ProtectData、AllocatePhysical。它们只覆盖 VM guard/映射操作，不覆盖每个普通 HLE 的短 vm_mutex 获取，因此不能以 VM tag 很短证明所有锁获取成本为零。这里的 graphics WaitIdle 实际等待 Liverpool 命令处理/queue submission 返回，**不是 vkDeviceWaitIdle/所有 GPU 运算完成**。

## 定向验证

| 检查 | 结果 | 覆盖/限制 |
|---|---|---|
| 最终 Android File I/O | 363/0 | 同 fd 独立 pread、不同卷写入、quota超额、close+在途租约、pin drain、errno失败出口、并行 readonly open |
| 最终 Android Data mapping | 129/0 | 活跃执行/无关pin期间分配、权限split、边界/覆盖/executable拒绝、code_generation/sink不变 |
| macOS Data mapping | 133/0 | 另含 mmap ENOMEM、mprotect EACCES，失败后ledger/权限/code状态不变 |
| LLVM TSan Data mapping | 133/0 | 并发 Write/新增Map，无报告；非真实 FEX/设备调度证明 |
| 现有 host API contract | 46/47 +1 SKIP | 原 token/publication/refusal矩阵；macOS不允许RWX的一例仍SKIP |
| 普通 APK | 多轮屋顶+正常Stop | 真实FEX/Qualcomm；不等于完整游戏、十分钟或loading性能验收 |

Android 原生测试通过 `run-as com.shadps4.android` 执行，UID相同、SELinux域为runas_app，区别于普通APK。真实FEX链路由独立的普通APK运行验证。初次独立 native runner缺少GPU Reshape动态库、MCP错误service参数、macOS默认sysroot失效等失败保留在build日志；没有把失败当通过。

## FIOS 与 host 直读的实测

诊断 property `debug.shadps4.profile_io=1` 在 Session Prepare 时启用逐次 read counters；默认关闭，无额外 lseek/逐read计时。Open保存guest path，ReadDone保存fd/offset/bytes/result/errno及storage preparation/syscall ns。7个counter与完成bookmark在同一物理线程顺序写出，离线解析按完成事件配对；路径在读完所有线程块之后解析，避免跨线程chunk顺序漏关联。

APK6364b2a / PID27747 / gen1 的120秒记录：

- 46,379,075 events，0 skipped chunks，0 unmatched scope，54个边界未闭合scope保留；文件/容器完整不等于无边界截断。
- 39,129次read，704,967,456字节；syscall累计353.912ms，p50 7.552μs、p95 17.448μs、max0.424948ms。
- Storage准备累计26.450ms，p50 0.625μs、max0.058333ms。这不含FEX/guest FIOS调度/解压，也不能把跨线程累计耗时当单一critical path。

按时间排序后每39条选一条成功的/app0请求，保留1004条/17,486,277字节，实际文件为安装内容`data.arc`。停止游戏后同UID复放相同文件、偏移、长度和预触页buffer；一次raw预热并建立结果/hash，之后5轮轮换mode顺序。散列计算不计入read计时，每个mode每个请求均校验真实字节与返回值，错误0。

| 路径 | 1004条读请求总耗时中位数 | 五轮范围 |
|---|---:|---:|
| host native preadv | 2.247ms | 2.172–2.380ms |
| GuestStorage::Positioned | 2.317ms | 2.234–2.492ms |
| checked DispatchStorage | 2.673ms | 2.601–2.782ms |

这是暖缓存、同文件的 native存储/ABI回放，**未执行guest FIOS或FEX**，不能由此宣称整个FIOS只有这些开销，也没有清系统page cache或做冷存储吞吐测试。新bench：`tests/host_runtime/fios_io_bench.cpp`，精确TSV和运行输出归档于`fios-io-20260917/`。

## VM 与停帧时间线

APK2723fdfa / host1dfa820e / PID4164 / gen1 / UUID16f9f837b195ffa8bd3ba71c3f505218：120秒45,838,065events，0skipped/0unmatched，56个边界scope保留。

| 事件 | 次数 | 累计 | 最后发生 |
|---|---:|---:|---:|
| MapFreshData |459|4.665ms|88.501s|
| ProtectData |31|0.437ms|22.434s|
| QuiesceAllOwners |10|0.581ms|24.925s|
| DrainGraphicsSubmissions |4|0.012ms|24.925s|
| FullCodeRetirement |1240|92.218ms|24.940s|

FullCodeRetirement 大部分来自启动装载器/token内部多次更新，不能把1240次说成1240次运行中全owner停机。

真实 PreparedGuestFlips 576→577 间隔56.921–82.788s =25.867s，PresentedGuestFrames同样停止25.870s，不是仅counter采样断档：后一个计数只增加1。该区间无VM transaction/data map事件，其他时段的VM边界不能解释这25秒。在这个区间仍有12,590次read /287,053,433字节，syscall累计134.529ms、storage准备8.091ms。

真实首帧17.720s（前一IO诊断17.918s）不是受控性能A/B，不宣布0.2s收益。用户观察的超长loading仍在；单靠VM事务已有计量不能解释它，普通HLE总锁争用/跨界成本/guest后台CPU工作还须继续分开计量。

## 最终交付与限制

最终APK84be94ba / host66816766，将AvPlayer callback scratch等剩余新数据分配也接上；最终APK源码/DSO/zip条目SHA见`final-artifacts.json`，源码文件SHA见`source-manifest.json`。PID7161/gen1/UUID3d7be6026ec3adffde264107e14cfaad实际屋顶MOVE，约12FPS；warmup TIMEOUT_UNVERIFIED保留，没有以屋顶截图提升为输入验收。真实Stop为Stopped/user_stop。

最后在关闭逐read诊断/自动startup capture后启动普通会话，恢复ring recording；无自动抓帧、无debugger、无持续按键。最终正常会话和最终120秒profile汇总追加于下方。

没有修改FMOD/FIOS语义或加游戏no-op patch；没有提前完成I/O/flush、变更音频/网络实现、换驱动或做完整回归。详细的所有权分解、全部调用者迁移范围、剩余隐藏锁职责和验收门槛见[设计文档](../../guest-memory-concurrency.md)。

### 最终 profile 与清理复核

最终APK的120秒文件`825f08ee…`：46,095,825 events /0 skipped/0 unmatched/52边界scope；首帧17.454s。463次MapFreshData累计4.528ms，31次ProtectData0.423ms；5次Quiesce累计0.150ms，2次图形drain累计0.005780ms，最后发生于25.059s。1236次旧FullCodeRetirement主要在loader/token阶段，最后25.071s，累计93.159ms。

最长PreparedGuestFlips656→657停顿为56.988–82.190s（25.202s），无VM tags重叠；期间12,373次read/284,482,749字节，syscall累计132.018ms。第二份独立诊断重复了“长停帧期间后台继续读，且无VM停机”的结论，仍未证明主线程具体在等待哪个任务的哪次完成。

最终普通会话PID7161/gen2仍在屋顶MOVE，截图约12FPS；与gen1区分，run_uuid沿用进程值，不能单靠UUID拼合不同代际。profile_io/startup_seconds均0，ring recording=1，file capture已结束，TracerPid=0，自动按键已经退出。该normal warmup同样TIMEOUT_UNVERIFIED，不提升为GAMEPLAY_REVIEWED。主仓与子仓先前未提交改动均保留。

归档入口：[artifact manifest](fios-io-20260917/artifacts.json)、[三份profile摘要](fios-io-20260917/analysis-summaries.json)、[最终普通会话](fios-io-20260917/normal-state.txt)、[正常屋顶截图](fios-io-20260917/normal-rooftop.png)。大PROF文件留在manifest记载的build路径，未塞入git。
