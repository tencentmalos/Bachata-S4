# Guest 内存并发：移除 Runtime VM 总锁

状态：2026-09-17 已删除 Runtime/HLE 共用 VM 总锁，普通 APK 屋顶移动及 Stop 验证完成。
最新实现、定向检查与边界见 [总锁删除验证](validation/android-native-host/vm-lock-removal-2026-09-17.md)。
`GuestRuntime::Impl::vm_mutex`、VmGuard 及传给 HLE 的 mutex 参数已删除。
MemoryManager 的私有映射 writer 仍在；这是映射事务排序，不是普通 I/O/audio/HLE 的执行 gate。
范围引用退场和代码停机准备发生在区间表锁之外，因此等待某个 I/O pin 不会拦住无关的 GPU/地址查询。
底层独立 range 并发不代表整个 desktop MemoryManager 的多 writer 事务已并行化。

## Desktop 给出的实际基线

| 路径 | Desktop 当前实现 | Android/FEX 原有额外串行化 | 应采用的边界 |
|---|---|---|---|
| 普通文件读取 | HandleTable 短查询，随后每个 File 自己同步；preadv 用 seek/read/restore，因此同一 File 仍串行 | DispatchStorage 持 VM 总锁跨 I/O，GuestStorage 再持全文件锁 | handle lease；有偏移的普通文件 preadv 无游标锁，普通 read/seek 用每 File 游标锁 |
| AudioOut | port table 短共享查询，随后 port mutex | GuestAudio 校验、pin、PCM 转换/提交使用全 runtime VM gate | 对应 port/队列的所有权；短期 buffer lease；callback 不接触 VM |
| VM 元数据 | MemoryManager 的 mutex/unmap_mutex 保护地址/物理区间表和相关 cache 通知 | 外层 VmGuard 先暂停所有 guest owner，再 drain 图形提交；每个 Update 又 FullFlush | 内存管理器私有的元数据写同步；增加新数据地址不打断执行 |
| GPU 映射 | Rasterizer::MapMemory/UnmapMemory 收到明确 addr/size，更新对应 cache/page tracker | VmGuard 在无关分配时也先 drain 全部图形命令 | 仅受影响资源的引用、提交 tick 和 retire；普通新增地址不 drain |
| guest pthread mutex | 原生 guest 直接执行/调用对应宿主实现，无 FEX runtime VM 外层 gate | 每次更新 owner/depth 还取得 vm_mutex | 同步对象自己的状态锁或统一 guest 原子协议；内存层只负责该对象所在映射的有效性 |

可核对源码：[file_system.cpp](../src/core/libraries/kernel/file_system.cpp)、[audioout.cpp](../src/core/libraries/audio/audioout.cpp)、[memory.cpp](../src/core/memory.cpp)、[vk_rasterizer.cpp](../src/video_core/renderer_vulkan/vk_rasterizer.cpp)。
Desktop 也有元数据/路径/文件锁，不能称作完全无锁；关键是正常音频、文件 I/O、guest 运算不共用一个执行 gate。Android 不应额外引入这种耦合。

“GPU/音频/I/O 可能操作同一块内存”不是全局互斥的理由。正常游戏自行管理这些 buffer；FEX 普通 guest load/store 本来就不取得 `vm_mutex`，这把锁也无法替游戏保证业务数据无竞争。真正需要由模拟器协调的是明确的映射增删改、某个异步请求引用的地址寿命以及可执行代码替换。

## 当前实现

- 文件 descriptor 使用稳定 File 引用；preadv 不持游标锁，read/seek 使用对应 File 的游标同步；save 配额按卷同步。
- `AcquireDataBatch` 一次校验整批范围/方向/可选 mapping identity 后取得所有引用。已迁移 storage、AJM、audio 和多输出 HLE。
- `ReadData/WriteData/AcquireDataSpan` 只短暂同步元数据，memcpy / observer / syscall 在锁外。正在退休的范围内部等待，可取消，无部分 pin。
- `UpdateDataMapping` 支持非 X 数据 Map/Protect/Unmap，关闭该范围新入场并只等重叠 pin，不暂停 FEX，不修改 code generation。代码修改仍走显式 CodePublication。
- Runtime 不向任何 HLE domain 传 VM mutex；SSL / NP / AppContent / heap trace 的对象状态同步已分离。owner 创建/退出在真实 publication 期间有界重试。
- MemoryManager 先选择地址，再通过 PrepareMapping 关闭目标范围的引用入场并等待退场，最后取得区间表锁提交账本。覆盖 map/file map/protect/unmap/pool/direct free；多 alias 的 direct free 一次准备整批范围。Unmap 在清零/回收 backing 前退休映射；Protect 在 backend 成功后更新 VMA。
- 非 X 数据退休不阻止新 Run 取得 execution lease；真实代码 publication 与 poison 仍阻止执行。

下文保留完整设计目标和验收边界；其中 MemoryManager 多 writer 并行、跨 VMA Map 的整笔回滚、额外 executable backing alias 身份仍是未实现部分，不能从底层 range 测试推导已完成。

## 目标接口：普通 HLE 根本拿不到 VM 锁

内存系统应分成三个能力，而不是一个什么都能做的 `VmGuard`。

1. **Data access**：`ReadData/WriteData` 与 `AcquireDataBatch(requests)`。request 描述地址、长度、方向以及可选的映射身份。返回值只有字节视图和释放能力，不暴露任何 mutex、代码 token 或全域停机方法。
2. **Data mapping**：仅内存管理器使用 Map/Protect/Unmap。新增映射直接发布；已有数据范围修改先关闭该范围的新 lease，等待该范围的在途引用退场，再提交映射变更。无关 range 不参与等待。
3. **Code publication**：Linker、guest patch、debugger 专用，显式列出代码地址和实际 backing alias。现有 FEX 在修改代码时可暂时保留 owner 停机，但它不能作为普通数据操作的兜底路径。全 cache clear 只能通过明确的维护操作进入。

`AcquireDataBatch` 必须一次校验并取得全部 span，不能逐个 pin 后让调用者自己取得 VM mutex 来补原子性。AJM 的输出 mapping identity 检查也放进同一个原子 admission 中。遇到 range 正在退役时，内部先释放所有临时引用再等/重试；不得持部分 pin 等待一个又在等这些 pin 的 writer，也不得把短暂 Busy 变成 guest EFAULT。

普通 `Write` 也已改为取得目标范围的短 lease，离开元数据同步区后 memcpy/notify，避免删除外层 `vm_mutex` 后留下同类串行化。查询仍使用短元数据同步，未引入不可变区间快照或自制 RCU。bulk copy、I/O syscall、decode、callback 不在其中执行；范围等待通过 condition_variable 释放元数据锁。mmap/mprotect 的映射提交 syscall 仍在内存管理器的元数据临界区，这是后续若进一步拆分提交需要处理的边界。

## 数据映射事务如何实现

- MemoryManager 作为虚拟/物理区间事务的 owner，元数据锁保持私有；HLE domain 不保存它的引用。地址选择、物理分配和 ledger commit 在这个组件内部完成。
- 为要替换/删除的范围建立 `MappingEdit`，标记 retiring 并禁止该范围的新 lease。只等待该范围的引用；MapFreshData 不进入 retiring/drain。
- 对 GPU cache 发 addr/size 的失效/retire，等待确实引用该范围的资源或提交 tick；不要调用通用 `graphics->WaitIdle()`。音频 callback 消费的是 host 自有 PCM，无须参加 guest VM drain；AJM 持有的 host 解码结果也不参加，只在写回时取得输出 lease。
- syscall 成功后发布新的 mapping identity；失败保留原映射/账本。MemoryManager 与 GuestAddressSpace 目前有两份 ledger，迁移必须同时整理 prepare/commit，不能先改 VMA 再让底层返回错误留下分叉。当前生产映射失败仍可能终止 generation，并未在这轮提供完整事务回滚。
- backing alias 记录真实 backing identity + offset/length，不能依赖全域 `shared_backing_seen` 就清空所有译码。新增 alias 不写 backing；修改 executable bytes 才需要对实际 code alias 退休译码。fd 数值会复用，不能作为长期 backing identity。
- Session Stop 是独立的终止协议：先取消并 join 相关 owner/worker，再销毁映射。不要把正常运行的每次操作伪装成一次缩小版 Stop。

## 迁移范围与防止回退

本轮已按连续修改包完成：内存 batch/range lease → 迁移全部 HLE 调用者 → 删除公共 VM gate → 并发反例和真机 loading 验证，中间可以提交，但不能把替换一两个调用点算成收口。

已迁移的普通 HLE 调用者：runtime 的 clock/TLS/短输出、thread attributes、mutex/rwlock/kernel semaphore、storage、audio、AJM、AvPlayer、Pad、NP/HTTP2/RTC/AppContent 等 marshalling。还有两类伪装成 VM 的对象同步：SSL dummy id 的静态计数器已改原子；NP lambda 的 observed 状态已自身原子化。把 `vm_mutex` 机械删除会暴露这些隐藏职责，因此要逐一改到所属对象，不能换成另一把 HLE 总锁。

完成条件：

- `Impl::vm_mutex` 删除；HLE 构造函数、DispatchStorage 等不再接收通用 mutex 参数；没有用新名字重新引入同等共享 gate。
- blocking I/O、audio backpressure、AJM decode、guest callback 中任意一个被确定性挂起时，其他模块以及无关数据 Map/Protect/Unmap 仍可推进；相同 range 的销毁等待可解释且可取消。
- 多 buffer 的最后一个参数无效、同 VA remap、close/slot reuse、取消与写回交错均不发生部分错误写回或 use-after-unmap。
- data mapping 不改变 code generation、不清 FEX cache、不暂停 owner；真实代码修改仍验证旧译码不可达。
- 用真实启动和跨帧 loading 区间验证 guest / I/O / audio / GPU 进度，标明采样扰动。不能以只跑文件 microbench、stub/FEX lease 测试或 GPU overlay 仍刷新代替游戏 loading 完成。

## Loading 的独立验收

删除总锁前的 PID 4164 历史记录中，最长 PreparedGuestFlips 间隔为 56.921–82.788 秒（25.867 秒）；期间没有 VmGuard 全局停机、图形 drain、FullCodeRetirement，甚至没有 data map。故该停顿不能归因于这些 VM 事务。这条历史结果不能替代对普通 HLE VM gate 的处理；该 gate 已在本轮删除。

FIOS 原库继续在 guest 执行；读请求完成只代表该请求的实际字节到达，不等于 archive inflate、FMOD bank 构建或 Studio command 完成。下一轮要把 FIOS job/worker/request completion 与主线程 flush/condition 等待关联，并采集 worker on-CPU 栈。禁止把 flushCommands 提前返回或使用空 custom patch 来“修复”加载。VM 总锁移除和后台任务链定位应共同服务于这个验收，不能预设一个原因覆盖所有停顿。


删除总锁前另一次120秒历史记录重复了上述现象：56.988–82.190秒无新guest flip（25.202秒），无VM事务重叠，期间后台完成284MB读取，syscall累计132ms。总锁删除后的 PID21722 记录仍有 29.685 秒连续 guest flip 间隔，期间没有 VM 事务重叠；完整身份和指标见最新验证报告。这说明总锁删除已经落地，loading 的任务等待链仍需独立定位。
