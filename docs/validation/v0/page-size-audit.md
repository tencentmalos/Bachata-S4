# FEXCore 页大小审计（16 KiB host）

所属：[V0 总 spec](../../specs/android-fex-v0.md) §5。这是 spec 要求的 `page-size-audit.md`。

- 审计对象：`references/FEX`，只读，未修改
- 审计范围：**FEXCore-only 构建实际链接**的代码（`FEXCore/`、`CodeEmitter/`、`FEXHeaderUtils/`）
- 明确排除并单独标注：`Source/Tools/LinuxEmulation/`、`Thunks/`、`FEXInterpreter` 等不链接的部分
- 审计日期：2026-09-07

## 0. 结论摘要

在链接范围内找到 96 处 `FEX_PAGE_*` 使用（分布于 20 个文件）、1 处字面 `4096`、2 处 `0x1000`、
3 处 `0x0FFF` 掩码、10 处 `>>12 / <<12`。分类计数：

| 类别 | 数量 | 含义 |
|---|---|---|
| HOST_VM | ~41 | 传给 mmap/mprotect/munmap/madvise/mremap，**必须**等于真实 host page |
| INTERNAL_INDEX | ~38 | 内部 code-cache / lookup 索引粒度，**可以**保持 4 KiB |
| GUEST_ABI | ~17 | x86-64 guest ABI 粒度，**必须**保持 4096 |

**关键事实：FEXCore 链接范围内没有任何 `getpagesize()` / `sysconf(_SC_PAGESIZE)` 调用。**
唯一获取真实 host 页大小并注入 allocator 的代码在 `Source/Tools/FEXInterpreter/` 与 `ELFCodeLoader.h`，
而这些在 FEXCore-only 构建里**不存在**。也就是说：嵌入式 FEXCore 目前根本不知道 host 页大小是多少。

这直接推翻了一个可能的乐观假设——“把 `FEX_PAGE_SIZE` 改成运行时变量就行”。
`TypeDefines.h:9` 的单个常量同时承担上面三种互相冲突的职责，不能整体改值（见 §3）。

## 1. 根常量

| 位置 | 内容 |
|---|---|
| `FEXCore/include/FEXCore/Utils/TypeDefines.h:9` | `constexpr size_t FEX_PAGE_SIZE = 4096` |
| `TypeDefines.h:10` | `FEX_PAGE_SHIFT = 12` |
| `TypeDefines.h:11` | `FEX_PAGE_MASK = ~(FEX_PAGE_SIZE - 1)` |

该处注释自称是为“在 16k/64k 页的构建系统上绕过问题”，但实际把三种语义合并成了一个值。

## 2. 四个阻断项（16 KiB host 上启动即失败或静默失去保护）

### PS-01　`GetHostVABits()` 启动即 abort —— 最高优先级

`FEXCore/Source/Utils/Allocator.cpp:105-135`。探测 host VA 位宽的方式是：对
Bits ∈ {57,52,48,47,42,39,36} 依次尝试 `mmap((1<<Bits) - 4096, 4096, MAP_FIXED_NOREPLACE)`。

每个候选地址都是 4 KiB 对齐但**不是 16 KiB 对齐**。在 16 KiB 页内核上七次尝试全部 `EINVAL`，
循环耗尽后直接命中 `FEX_UNREACHABLE`。

它由 `OSAllocator_64Bit::DetermineVASize()` 和 `Setup48BitAllocatorIfExists()` 在最早期调用，
因此这是**硬启动中止**，不是降级。修法明确：探测偏移改为一个 host page。

注意这也正是验收矩阵 M04 说的“EINVAL 不当作 VA 位宽上限”——这里的 EINVAL 恰恰来自对齐而非位宽。

### PS-02　JIT code buffer guard page 静默消失

- `SharedCodeBufferManager.cpp:25-27`：`AlignDown(Ptr + Size - 1, 4096)` 后 `mprotect(…, 4096, None)`
- `SharedCodeBufferManager.h:71`：`UsableSize() = AllocatedSize - 4096`
- `ThreadPoolAllocator.h:423-425`：`PooledAllocatorVirtualWithGuard::Alloc` 同一模式（支撑 `CPUBackendAllocator`）
- `JIT.cpp:887-891`：`UsableBufferRange = Size - 4096`，`JITGuardPage = TempCodeBuffer + UsableBufferRange`

在 16 KiB host 上 `AlignDown(…, 4096)` 落在页中间，`mprotect` 失败。
**失败只被记为一条 `EFmt` 日志，执行继续。** 于是 JIT 溢出不再触发 SIGSEGV，而是静默破坏堆。
同时 `UsableSize()` 少预留 12 KiB。

这条正对应 spec §5.1“禁止以长期 RWX 及取消所有保护作为验收实现”的反面风险：
不是主动取消保护，而是保护**以为设上了其实没有**。修法：guard 必须是一个 host page，`UsableSize` 同步扣减。

### PS-03　`InterruptFaultPage` —— 与中断机制直接相关

`FEXCore/include/FEXCore/Debug/InternalThreadState.h:125`：`alignas(4096) uint8_t InterruptFaultPage[4096]`，
位于 `alignas(4096) struct InternalThreadState`（:90）内部，而该结构经 `FEXAllocOperators::operator new`
→ `Allocator::malloc` 分配，也就是**堆对象**，对齐由 jemalloc 给出。

它是 JIT 的中断机制本体：生成代码在 `Dispatcher.cpp:279-280` 与 `JIT.cpp:771` 无条件执行
`strb wzr, [STATE, #off]` 写入该页；信号路径在 `PROT_NONE` ↔ `RW` 之间翻转它
（`Core.cpp:447` 与前端若干 `mprotect`）。

16 KiB host 上两种结果都坏：`mprotect` 因地址未 16 KiB 对齐而 `EINVAL`；
或者恰好对齐时保护整整 16 KiB，**连带破坏相邻 12 KiB 的 jemalloc 堆**——其中包括同一段生成代码要读的
`BaseFrameState`。此外 `:130` 还有 `<= 65520` 的偏移断言限制它能挪多远。

这一项对 V0 特别关键：验收 T02 要求“无 HLE、无 yield 的死循环可被暂停”，而 FEX 的
dispatcher 中断检查正建立在这个 fault page 上。修法：改为独立映射且按 host page 对齐/定尺，不能留在堆对象里。

### PS-04　`64BitAllocator.cpp` 整体（32 处）

它用 4 KiB 页位图重新实现了 `mmap`/`munmap`：

- `:141` `static_assert(sizeof(LiveVMARegion) == 4096)` —— 直接改常量会编译失败
- `:599-604` `make_alloc_unique` 在 `AlignUp(sizeof(T), PAGE) != PAGE` 时 `ERROR_AND_DIE_FMT`
- `:242-260` `Mmap()` 接受 4 KiB 对齐的 addr/offset，而底层 `::mmap` 会拒绝
- `:99` `alignas(4096) FlexBitSet UsedPages`，注释明说是为 madvise 零页池对齐
- `:193` `UPPER_BOUND -= 4096`（x86 末页），`:532` 跳过 `<= 4096*2` 的区域

该文件在 `NOT MINGW` 条件下编译，因此 **Android FEXCore-only 构建会包含它**。

### PS-05（次级）　CodeCache 的 `mremap` 与磁盘 4 KiB 填充契约

`CodeCache.cpp:917-944` 的 `mremap(MREMAP_FIXED|MREMAP_DONTUNMAP)` 要求 src/dst/len 三者页对齐，
而三者都只来自 `AlignUp(…, 4096)`（:317, :655, :683）。`:358-375` 的 `ftruncate`/`lseek`
把磁盘缓存也按 4 KiB 填充。这不只是常量问题，是**磁盘格式**问题：已有 cache 文件在 16 KiB host 上不可映射。

V0 不依赖持久 code cache，可以先禁用该路径规避；但不能声称已解决。

## 3. 为什么不能全局把 4096 改成 16384

spec §5 明令禁止全局替换，审计给出了具体理由：

1. **生成的机器码钉死了 12**。`Dispatcher.cpp:195,204` 发射的 ARM64 指令是
   `lsr TMP2, TMP4, #12` 和 `and TMP2, TMP4, #0x0FFF`。改常量不会改这两条指令，
   会与 `LookupCache.h:196-197` 的索引计算立刻不一致。
2. **guest ABI 钉死了 4096**。x86-64 vsyscall 页架构上就是 4 KiB
   （`Frontend.cpp:1358-1371`、`VSyscall.inc:29` 的 `uint8_t VSyscallData[0x1000]`），
   guest auxv 的 `AT_PAGESIZE = 4096`。
3. **`static_assert` 会直接编译失败**（`64BitAllocator.cpp:141`）。
4. **启发式会被静默放大**。`Frontend.cpp:1196` 的 `MAX_FORWARD_BRANCH_DIST = FEX_PAGE_SIZE * 4`
   本意是 16 KiB 的多块编译上限，改常量会悄悄变成 64 KiB。

结论：必须**先拆分三个常量**，再分别赋值。这与 spec §5“区分三种量”的要求一致。

## 4. 边界情形：SMC 写保护粒度

`Core.cpp:904-905`、`Core.cpp:1000-1001`、`CodeCache.cpp:790-791` 的
`AddBlockExecutableRange(…, CodePage, FEX_PAGE_SIZE)` + `MarkGuestExecutableRange(…, FEX_PAGE_SIZE)`
是 guest 页粒度的（GUEST_ABI），但最终会去 `mprotect` **guest 映射**（HOST_VM）。
16 KiB host 无法对 4 KiB 子范围单独设权限。

这正是 spec §5.1 允许 V0 用 `Unsupported` 挡掉的情形，也是验收 M03 要验的行为，
以及 V0 选择 **ExplicitPublication** 而非 TransparentSMC 的直接技术理由（见 §6）。

## 5. 不链接的部分（对照记录）

以下都**不在** FEXCore-only 构建中，列出以免误判：

- `Source/Tools/FEXInterpreter/FEXInterpreter.cpp:152,169`、`ELFCodeLoader.h:472`
  —— 唯一取得真实 host 页大小并传给 `Allocator::SetupHooks(PageSize)` 的地方
- `Source/Tools/LinuxEmulation/LinuxSyscalls/ThreadManager.cpp:36,170,186-195`
  —— `CALLRET_STACK_ALLOC_SIZE = CALLRET_STACK_SIZE + 2*FEX_PAGE_SIZE` 及两侧 4 KiB guard
- `SignalDelegator.cpp:1012-1027` —— altstack `mmap` 与首页 `mprotect`
- `LinuxAllocator.cpp`、`VDSO_Emulation.cpp`、`ELFCodeLoader.h` 的 `AT_PAGESIZE = 4096`

需要注意一个混合情况：`InternalThreadState::CALLRET_STACK_SIZE = 0x400000`（`InternalThreadState.h:112`）
**在链接范围内**，被 `Core.cpp:505,1089` 和 `JIT.cpp:823` 通过 `VirtualDontNeed`
（→ `madvise(MADV_DONTNEED)`）使用；但 ±1 页的 guard 运算和实际分配都在不链接的前端。
也就是说 V0 必须自己提供这段栈的分配与 guard，不能指望前端。

另有 `atomic_segmented_bitmap_allocator.h` 与 `atomic_bitset.h`：审计确认当前无非 unittest 的引用者，
属于死代码，不计入阻断项。

## 6. 对 V0 的直接影响

| 影响项 | 结论 |
|---|---|
| FEX 能否在 16 KiB host 直接启动 | **否**。PS-01 是硬中止，必须先修 |
| 能否只改 `FEX_PAGE_SIZE` 一个值 | **否**，见 §3 |
| SMC 模式选择 | 因 §4 的子页保护不可行，V0 采用 **ExplicitPublication**，并在初始化时拒绝要求 TransparentSMC 的 caller |
| host 页大小来源 | 必须由**嵌入方**（本仓库）在初始化时发现并注入；FEXCore 自己不查询 |
| 修改是否属于必须改 FEX 源码 | 是。PS-01~PS-04 无法在 FEX 之外绕过 |

最后一行触发 spec §3 与任务书的贡献规则条款：FEX 自身的 `AGENTS.md`/`CLAUDE.md` 禁止
AI 生成的代码贡献。相关处理见[技术决定记录](decisions.md) DEC-03。
