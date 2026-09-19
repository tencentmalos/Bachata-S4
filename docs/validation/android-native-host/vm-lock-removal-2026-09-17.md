# Runtime VM 总锁删除与真机验证

2026-09-17，`codex/android-fex-round2`，`4da582b7` 加此前未提交工作和本轮修改。没有 commit/push；没有修改 FEX 子仓、Foundation 或 guest patch。此前 dirty 内容保留。

## 已落地的边界

`GuestRuntime::Impl::vm_mutex`、`VmGuard`、共享 `vm_token`、`data_mapping_depth` 已删除。Audio / AJM / AvPlayer / mutex / rwlock / thread attributes / kernel semaphore 构造函数和 DispatchStorage 不再接收 VM mutex，普通 HLE 无法取得一把跨模块执行锁。

- `GuestAddressSpace::AcquireDataBatch` 在一次元数据临界区里校验并登记整批范围、权限、可选 mapping identity。最后一个输出无效时不会留下前面几个 pin；AJM、preadv/pwritev、存档多输出、音频多端口、线程属性、DiscMap/AppContent 使用批量接口。
- `ReadData/WriteData/AcquireDataSpan` 将范围有效性和寿命绑定。Read/Write 的 memcpy、observer、I/O、PCM 转换均在 address-space 元数据锁外；普通读写不持 VM execution gate。WriteData 保留 write-only 语义，通用 writable span 保持 RW 要求。
- `UpdateDataMapping` 用 retiring range 关闭冲突入场，只等待重叠的 HLE pin；支持非 X 数据 Map/Protect/Unmap、分割与同 VA 替换。无关范围的 API 操作和 execution lease 可以推进，不调用 invalidation sink、不改变 code generation。
- 取消必须在 syscall commit 前再检查。第一轮 Android AJM 反例抓到了“cancel 与最后一个 pin 释放同时发生，condition_variable_any 因谓词为真仍返回成功”的竞态，已修复；旧失败日志保留。等待上限 2 秒，stop_token 取消后释放 retiring 标记。
- 分配/释放线程的 backend handle 改为遇到真实代码 publication 时有界等待重试，不再借 VM 锁同步。TLS 初始化持自身目标 pin；heap trace 用 once_flag；NP 一次性日志用自身 atomic；AppContent 状态用自身 mutex；desktop SSL dummy id 改成 atomic。
- 代码修改仅经显式 `CodePublication`：token 属于当前物理线程的作用域，嵌套复用外层 token。它参与 MemoryManager 映射 writer 的排序，没有向 HLE domain 暴露锁。实际 X 映射或 Linker / guest patch 才停机；VideoOut 内嵌 GPU shader 字节走普通数据写。通用映射不再先 `graphics->WaitIdle()`，仍调用 desktop 的按 addr/size GPU cache 通知。
- MemoryManager 的 Unmap 先退休实际范围，再清零/归还 flexible backing；Protect 先成功执行 backend，再提交该 VMA 的权限账本，避免 pin 存活时回收内存。flexible_usage 准入检查移回映射 writer 内。

## 最后审计补齐：范围退休不能拖住区间查询

删除外层 Runtime 锁后，MemoryManager 曾仍持区间表 `mutex` 进入 backend，再等待某个 I/O pin。真实 MemoryManager 反例重现：`Protect` 确定性阻塞时，无关地址的 `QueryProtection` 也超过 300 ms 无法返回。测试内关闭提前准备的旧顺序得到 **10 checks / 1 expected failure**，不是把原始失败算为 PASS。

已增加 `GuestMemoryBackend::PrepareMapping` 和私有范围退休作用域，按此顺序执行：

1. 私有 mapping writer 选择/验证地址，整批检查是否有 X 范围。
2. 不持区间表锁，关闭目标范围的新引用并等待已有引用退场；若涉及代码，先取得明确的 CodePublication。
3. 取得区间表锁，执行分段 Map/Protect/Unmap，提交 VMA/物理账本；准备作用域一直保留到整笔操作结束。

已覆盖 MemoryManager 的 MapMemory、MapFile、Protect、UnmapMemory、PoolCommit、PoolDecommit、Direct Free；后者把所有 VA alias 一次准备，避免先退休数据再遇到代码而等待自己。一个物理线程拥有准备结果，可完成该范围内多个分段提交；外线程不能借用，取消与异常都重新开放引用入场。

新测试同时发现 `AcquireExecutionLease` 还因任意 data_edit 而拒绝新 Run，这是残留的执行门控；已删除这一条件。非 X 数据退休期间，现有执行、新 Run/回调恢复都不被全局拦住。代码 publication / poison 的拒绝条件保持，host contract 仍通过。

最终 Android 定向 **832 checks / 0 failures**：range167、真实 MemoryManager40、AJM130、native mutex495；旧顺序另有确定性 expected failure。macOS range ThreadSanitizer **172 / 0**，host API contract **46/47 + 1 RWX SKIP**。早期 ThreadSanitizer 测试的“新 Run 不应被拒绝”失败输出保留。详见 [最终定向清单](vm-lock-removal-20260917/preparation-results.json)。

## 保留的内部同步及范围限制

这不是“整个 VM lock-free”。GuestAddressSpace 仍有私有元数据 mutex；MemoryManager 保留 desktop 的私有 mapping writer 和区间表同步。**MemoryManager 的一个已进入事务若等待其重叠 pin，另一个 MemoryManager writer 仍可能排队**；底层 range API 的独立映射并发通过，不等于整个 desktop MemoryManager 已改成并行事务。普通 HLE、I/O、音频消费并不取得这把 writer 锁；范围退场时区间查询锁已释放，真实 MemoryManager 反例40/0确认无关查询可推进。当前 mmap/mprotect 提交仍在元数据临界区，多 writer 并行需要进一步独立地址预留/事务账本，不能简单在持有 VMA iterator 时 unlock。

代码 publication 的全 owner drain / FEX retirement 仍保留，不以非 X 数据操作兜底触发。新数据映射不写旧 backing；direct alias 的代码内容修改不在此批实现额外 alias 跟踪。MemoryManager 跨多个 VMA 的 Map 失败依旧是 generation-fatal，未提供整笔 desktop VMA/物理账本回滚；不能将底层单次 mmap 失败的原状保持测试扩大成全事务回滚保证。

## 首阶段定向验证（最后补齐前）

首阶段原始结果、精确 APK/DSO 身份见 [归档](vm-lock-removal-20260917/artifacts.json)，[测试清单](vm-lock-removal-20260917/final-results.json)，[本轮源文件 SHA](vm-lock-removal-20260917/this-turn-manifest.json)。

| 环境 / 检查 | 结果 |
|---|---|
| Android run-as：data mapping / batch / cancel / identity | 149 / 0 |
| Android run-as：file I/O | 363 / 0 |
| Android run-as：Audio / AJM | 123 / 0、130 / 0；真实 MP3 解码 32256 bytes |
| Android run-as：AvPlayer | 1205 / 0；包含损坏媒体和取消负例 |
| Android run-as：kernel semaphore / rwlock / condition / native mutex | 55 / 0、60 / 0、67 / 0、495 / 0 |
| macOS ThreadSanitizer：data / native mutex / condition | 154 / 0、495 / 0、67 / 0 |
| macOS API contract | 46 / 47 通过、1 个 RWX 平台 SKIP，0 FAIL |
| macOS thread attributes / sysmodule | 43 / 0、35 / 0 |
| 生产 host / APK | HOST_LINK_PASS；Gradle assemblePlaystoreDebug 成功 |

Android standalone 测试是 run-as 身份，不冒充普通 APK 验收。新的 mutex 反例直接退休某个 mutex 所在 page，并验证另一对象的 trylock 能推进，已不再用测试外部的 VM 总锁模拟阻塞。macOS fixture 必须按真实 HostPageSize 对齐；早期测试里的 4 KiB 假设及容量不足已纠正，失败输出保留在 build 下。

## 首阶段普通 APK 与 loading（精确旧产物）

首阶段安装 APK `20768a09…`，host `85c71bc0…`，JNI `5ec510b1…`；逐字节核对 APK 内 host 与 native 构建产物 SHA 一致。Host/JNI RelWithDebInfo、FEX Release；包 variant playstoreDebug。设备 AYN Thor / API33 / 4 KiB / Adreno740 系统 Qualcomm。

PID21722 / generation1 / UUID `cd67d4b909d60220fe607cf2acce478e` 实际完成启动、PLAY 后 loading，到 Leonardo 屋顶 MOVE；随后实际左右摇杆触摸引起角色移动、镜头跟随，进入 ATTACK 教程。最终独立 movement run 在运行中实时审核，`GAMEPLAY_REVIEWED`，见 [审核](vm-lock-removal-20260917/movement-review.json) / [manifest](vm-lock-removal-20260917/movement-manifest.json)。启动 warmup 和第一段 movement 未在截止前提交审核，仍是 TIMEOUT_UNVERIFIED；未事后改写为 PASS。截图在 `build/vm-lock-removal-20260917/`。

120 秒 PROF：**44,392,172 events，0 skipped chunks，0 unmatched，56 个截止边界 open spans**。不将未闭合 span 计作等待时间。没有 GPU timestamp records，这次不提供 GPU 成本归因。原始 PROF 359,757,408 bytes，SHA `a55975df…`，在 [trace summary](vm-lock-removal-20260917/trace-summary.json) 关联完整身份。

- 首次 guest present：启动后 20.527 秒。
- 504 次 `VM.DataMapping`：累计 9.124 ms，最长 0.07823 ms。
- 2 次 `VM.QuiesceAllOwners`：累计 0.053855 ms，最后结束在启动后 0.238 秒。
- 1227 次 FullCodeRetirement 全部在初始化中，最后结束在 0.723 秒；运行期间没有泛用 VM 全停机或全图形 drain。
- 最长连续 guest flip 间隔 **64.246–93.931 秒，29.685 秒，flip563→564**：无 VM 事务重叠；后台完成 12,779 次读、289,378,427 bytes，native read syscall 累计 137.607 ms。这里累计时间是多线程 elapsed 的和，不是关键路径或 CPU 时间。
- 屋顶截图约 13–14 FPS，未做等价场景 A/B，不宣称游戏加速。启动 profile / per-read observer 是显式开启的测量，不能视为默认无探针的性能基线。

**总锁删除没有解决这段 loading 停帧。** 仍需追 FIOS worker 的 CPU/解包/任务完成与 FMOD command 消费、主线程轮询的对应关系；不能将 bytes 到 host 等同 FMOD flush 已完成，也不能用空 guest patch 跳过同步。

最终实际 UI Stop 到 `Stopped/user_stop`，Session none，PID 仍存活，TracerPid0。profile_startup_seconds / profile_io / profile_sync 均归零，ringbuffer ON，capture 已 ready，自动输入退出。未做十分钟、三次同进程重启、Swan、16 KiB 或完整回归。

## 最终代码的普通 APK 验证

追加准备阶段/新 Run 准入修复后重新构建并安装：APK **fbe32126…** / host **49c9d7c6…** / JNI **cef0fdec…**，APK 内 host 与构建产物逐字节 SHA 相同。完整身份见 [最终产物](vm-lock-removal-20260917/artifacts-preparation-final.json)。Host/JNI RelWithDebInfo，FEX Release，API33 / 4 KiB / Qualcomm system Adreno740。

PID10690 / generation1 / UUID `8d98a6ab657f727f82cac3fcee8b5070`，未开 startup 文件采集、profile_io 或 profile_sync，ringbuffer ON。启动→PLAY→loading→屋顶恢复，frame11 在屋顶左侧 MOVE；实际 stick-right 输入后 frame14 移到右侧栏杆、镜头跟随、教程切为 ATTACK，guest flip905→1304。运行中提交审核并由 runner 退出0 / **GAMEPLAY_REVIEWED**，见 [manifest](vm-lock-removal-20260917/preparation-gameplay-manifest.json)、[review](vm-lock-removal-20260917/preparation-gameplay-review.json)。不是事后改写首次超时结果。

当前截图约12–13FPS，loading 仍出现数十秒无新帧，后续自行恢复；没有新等价场景 A/B 或性能提升结论。该轮不复用上一 APK 的精确 PROF 时间做新代码归因。实际 UI Stop 成功到 Stopped/user_stop、Session none、进程存活/TracerPid0；ring ON，capture none，自动输入 OFF。
