# FEX 16 KiB host page 适配（PS-02 / PS-03）

本文记录在自有 fork `tencentmalos/FEX` 上解除两个 16 KiB host page 阻断项的实际改动、判断依据和实测证据。

- 分支：`feature/malos/host-page-size`
- upstream base：`50e6eee95`（`FEX-Emu/FEX`）
- fork 起点：`dcfbe49cf`
- 对应审计项：[页大小审计](validation/v0/page-size-audit.md) PS-02、PS-03
- 验证探针：[tests/host_page_size/host_page_size_probe.cpp](../tests/host_page_size/host_page_size_probe.cpp)

## 1. 为什么这两项必须改

它们都在**嵌入方必经的路径**上，无法从 shadPS4 侧绕过：

- **PS-02**：JIT code buffer 的分配无条件执行，只要 FEXCore 编译一个 block 就会走到。
- **PS-03**：`ContextImpl::DestroyThread` 是销毁 guest 线程的唯一入口；`InterruptFaultPage`
  又是 dispatcher 中断机制本体，V0 验收 T02（"无 HLE 死循环可暂停"）建立在它之上。

另外两项 PS-01 / PS-04 **不在**当前路径上（只经 `Setup48BitAllocatorIfExists`，其唯一调用者
`FEXInterpreter.cpp:155` 不参与 FEXCore-only 构建），本次不改。

## 2. 关键区分：三种页大小不是一回事

这是本次改动最容易出错的地方，也是**绝不能全局把 4096 替换成 16384** 的原因。

| 概念 | 常量 | 取值 | 含义 |
|---|---|---|---|
| guest ABI 页 | `FEX_PAGE_SIZE` | 恒 4096 | PS4/x86 guest 可见的 mmap 对齐与权限粒度。改它就是改 guest ABI |
| host 内核页 | `HostPageSize()` | 运行时，4096 或 16384 | host 内核对 `mmap`/`mprotect` 边界的要求 |
| 编译期上限 | `FEX_MAX_HOST_PAGE_SIZE` | 16384 | 需要编译期常量的场合（`alignas`、结构体内数组尺寸） |

`FEX_PAGE_SIZE` 保持 4096 不动。新增的两个量只用于交给 host 内核的参数。

## 3. 故障机制（实测，非推断）

### 3.1 PS-02：code buffer guard page

原代码（`SharedCodeBufferManager.cpp:25`）：

```cpp
uintptr_t LastPageAddr = AlignDown(reinterpret_cast<uintptr_t>(Ptr) + Size - 1, FEXCore::Utils::FEX_PAGE_SIZE);
```

`Size` 是 16 MiB，`Ptr` 来自 `mmap`（host page 对齐）。按 4096 向下对齐得到的是
`Ptr + Size - 4096`，这个地址在 16 KiB host 上**不是 host page 对齐**，
`mprotect` 直接以 `EINVAL` 失败。

原代码对失败只记一条 `EFmt` 日志然后继续。后果不是"少了个保护"，而是
**JIT 写溢出从可捕获的 SIGSEGV 变成静默破坏 buffer 之后的堆内存**——
故障点会出现在离根因很远的地方。

同时 `UsableSize()` 仍按 `AllocatedSize - 4096` 计算，即使 `mprotect` 成功保护了
16 KiB，前 12 KiB 也会被继续当作可用代码空间发出去。

### 3.2 PS-03：InterruptFaultPage

`InternalThreadState` 原本是 `alignas(FEX_PAGE_SIZE)`，且继承 `FEXAllocOperators`
（自定义 `operator new`）。C++ 对带 `alignas` 的类型会调用**对齐版** `operator new`，
传入的 align 就是 `alignof`，即 4096。

实测结果（构建主机，真实 16384 页）：

```
aligned new(size=12288, align=4096)
FaultPage addr % 16384 = 8192        ← 不是 host page 对齐
mprotect(unaligned, 4096, PROT_NONE) -> -1 errno=22 (Invalid argument)
```

所以 `Core.cpp:447` 和 `SignalDelegator.cpp` 的几处 `mprotect` 在 16 KiB host 上全部失败。
`Core.cpp:447` 原本**完全丢弃返回值**，失败后 `delete Thread` 会把仍处于 `PROT_NONE`
的内存还给分配器，之后任何复用都会在远离根因处崩溃。

## 4. 改法与约束交叉

`InterruptFaultPage` 有一个容易忽略但决定性的约束：**JIT 把它的偏移编码成指令立即数**。

`Arm64JITCore::EmitSuspendInterruptCheck`（`JIT.cpp:769`）与 `Dispatcher.cpp:279` 都用
`offsetof(...)` 作为 `if constexpr` 的编译期常量。因此它：

- 必须留在 `InternalThreadState` 内部，**不能**改成独立 `mmap` 的指针
  （那需要多一次间接加载，属于改 JIT 代码生成）
- 与 `BaseFrameState` 的距离必须满足 `<= 65520`

由此得到本次改动的**适用上限**。实测各对齐下的最坏距离：

| `alignas` | 最坏距离 | `<= 65520`？ |
|---|---|---|
| 4096 | 4096 | 是 |
| **16384** | **16384** | **是** |
| 65536 | 65536 | **否** |

所以 16 KiB 可以支持，**64 KiB 不行**——65536 恰好越界。这不是保守估计，是精确的边界。
64 KiB host 需要把 JIT 的这条 store 改成寄存器偏移形式，属于独立改动。
`SetHostPageSize` 因此在运行时明确拒绝 64 KiB，而不是编译期悄悄错掉。

改动后的实际布局（NDK 交叉编译，实机确认）：

```
sizeof=32768  alignof=16384  sizeof%alignof=0
BaseFrameState -> InterruptFaultPage distance = 16192   (上限 65520)
sizeof(InterruptFaultPage) = 16384
```

`sizeof % alignof == 0` 这条是必要的：本机实测 `aligned_alloc` 在 size 不是 alignment
整数倍时返回 `EINVAL`，而 `alignas` 恰好保证了这个性质。

## 5. 逐项改动

| 文件 | 改动 |
|---|---|
| `FEXCore/include/FEXCore/Utils/TypeDefines.h` | 新增 `HostPageSize()` / `SetHostPageSize()` / `FEX_MAX_HOST_PAGE_SIZE`；`FEX_PAGE_SIZE` 不变 |
| `FEXCore/Source/Utils/TypeDefines.cpp` | 新增。host 页大小的存储与校验，默认 4096 |
| `FEXCore/Source/CMakeLists.txt` | 把上面这个 TU 加入 `FEXCORE_BASE_SRCS` |
| `SharedCodeBufferManager.h` | guard 尺寸存为成员 `GuardSize`，`UsableSize()` 据此扣减 |
| `SharedCodeBufferManager.cpp` | guard 按 host page 对齐定尺；`mprotect` 失败从日志升级为 `ERROR_AND_DIE_FMT` |
| `InternalThreadState.h` | 结构体与 fault page 对齐/定尺到 `FEX_MAX_HOST_PAGE_SIZE`；新增尺寸 `static_assert` |
| `Core.cpp` | `DestroyThread` 检查 `VirtualProtect` 返回值，失败即致命 |

### 为什么把失败改成致命错误

两处原本都是"失败也继续"。改成致命是有意的：

- code buffer guard 失效 → JIT 溢出静默破坏堆
- fault page 权限未恢复 → 释放后复用时崩在无关位置

这两种都属于内存破坏类问题，**当场失败远比带着损坏状态继续跑容易诊断**。

## 6. host 页大小的注入路径

FEXCore 链接范围内**没有任何** `sysconf(_SC_PAGESIZE)` 调用——唯一查询 host 页大小的代码在
`Source/Tools/FEXInterpreter/`，不参与 FEXCore-only 构建。已有的
`Allocator::SetupHooks(PageSize)` 只把值转发给 `InitializeAllocator`（rpmalloc 配置），
并没有存成 FEXCore 可查询的量。

因此嵌入方必须在创建 Context 之前显式调用：

```cpp
FEXCore::Utils::SetHostPageSize(sysconf(_SC_PAGESIZE));
```

未调用时保持 4096，即历史行为，不会突然改变 4 KiB host 上的语义。

## 7. 验证证据与其边界

探针 [host_page_size_probe.cpp](../tests/host_page_size/host_page_size_probe.cpp) 共 19 项检查，
其中包含**对照检查**：显式验证"修复前的算法在此处确实会 `EINVAL` 失败"，
以防日后退回旧写法而测试仍然通过。

| 环境 | 页大小 | 结果 | 证明了什么 |
|---|---|---|---|
| 构建主机 macOS ARM64 | **16384**（真实） | 19/19 PASS | 16 KiB 上对齐与 `mprotect` 正确；旧算法确实失败 |
| 实机 `PB3210PGL6170004G`，Android API 36 | 4096 | 19/19 PASS | bionic/aarch64 下可编译可运行，**没有破坏 4 KiB 行为** |

另外单独链接 `TypeDefines.cpp` + `LogManager.cpp` 实测 `SetHostPageSize` 契约：
默认 4096；0 / 1024 / 6144 / 65536 四类非法输入全部拒绝**且不改变已有状态**；16384 接受。

**这些证据不包含的内容**，避免被过度解读：

- 未在 16 KiB 页的 Android 实机上验证（手头设备是 4 KiB 内核页）
- 未运行 FEXCore 的 `InitCore()`，未执行任何 guest 代码
- 未构建完整 FEXCore（macOS 缺 Linux syscall 常量与 `malloc.h`，属宿主差异）
- 探针复现了布局与调用，不等于链接了 FEXCore 本体

## 8. 已知未改动项

| 位置 | 情况 | 为什么不改 |
|---|---|---|
| `SignalDelegator.cpp:1024` | AltStack guard 只保护 4096，且 `ss_sp` 从 +8 起可能落在被保护页内 | 位于 `Source/Tools/LinuxEmulation/`，不在 FEXCore-only 构建内；shadPS4 自行处理信号 |
| `SignalDelegator.cpp:422,603,662` | 三处 `mprotect` 不检查返回值 | 同上。尺寸用 `sizeof(InterruptFaultPage)`，会自动跟随新尺寸 |
| `Allocator.cpp` `GetHostVABits()`（PS-01） | 4 KiB 假设 | 不在当前调用路径上，见 §1 |
| `64BitAllocator.cpp`（PS-04） | 32 处 4 KiB 假设 | 同上。将来启用 64-bit host allocator 时须按三种页语义分别赋值 |
| 64 KiB host 支持 | 明确不支持 | 越过 JIT 立即数预算，见 §4。运行时拒绝而非静默错误 |

## 9. 关于上游贡献规则

`FEX-Emu/FEX` 的 `AGENTS.md` 与 `CLAUDE.md` 各含一行：

> AI must not be used to generate code for contributions to this project.

本 fork 的 `dcfbe49cf` 提交删除了这两处声明。**删除声明不等于上游规则不存在**。
本次改动在授权下于自有 fork 进行，且明确不回流上游。若将来需要向上游提交，
必须由人类工程师重写，不能以本文件的改动直接提 PR。
