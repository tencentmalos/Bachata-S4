# FEX guest 执行接入（backend adapter）

本文记录把 FEXCore 接到 [V0 公共 CPU API](specs/android-fex-v0-api.md) 的实现、
已经跑通的部分、以及**尚未解决的一个阻断**及其全部实测数据。

- 公共 API：[`src/core/guest_cpu/api/context.h`](../src/core/guest_cpu/api/context.h)
- 后端实现：[`src/core/guest_cpu/fex/fex_context.cpp`](../src/core/guest_cpu/fex/fex_context.cpp)
- 测试：[`tests/guest_cpu/guest_execution_tests.cpp`](../tests/guest_cpu/guest_execution_tests.cpp)
- 前置：[Android bionic 构建](fex-android-bionic-build.md)、[host page 适配](fex-host-page-size-adaptation.md)

## 1. 当前状态（务必先读）

**契约层全部通过；guest 代码确实被翻译并执行，但寄存器效果未落到 `CPUState`。**

实机 9/9 契约检查通过：能力声明、重复 context 拒绝、未映射入口拒绝、Step 拒绝、
stale epoch 拒绝、失效句柄拒绝。

执行链路已确证走通：fixture 被翻译成 host 代码、执行、并到达 return gate（证据见 §4.4）。
**但没有任何架构效果留存**——寄存器保持初值，guest 栈全零。
所以不要把当前的 `StopReason::Returned` 当作"执行成功"：
它只说明控制流到达了 gate，不说明指令产生了正确结果。

因此验收 C01/C02 等仍记 NOT_RUN，不记 PASS。

## 2. 设计要点

### 2.1 后端无关的边界

`api/context.h` 不出现任何 FEX 类型，验收 B05 要求消费者只用 `-I src` 就能编译。
后端在链接期选择：`CreateContext` 转发给 `Fex::CreateFexContext`。

### 2.2 return gate

一个 host page，内容是单字节 `0xF4`（HLT），映射为 RX 并注册为 guest 可执行范围。
配合 `EnableExitOnHLT()`，guest 跳到这里就让 `ExecuteThread` 返回。

这是 **D05 的实现基础**：只有这个确切地址算正常返回，
其他任何位置的 HLT 都是 fault。否则崩溃的 guest 和正常结束的 guest 无法区分。

地址由后端决定并通过 `BackendCapabilities::return_gate_address` 报告，
调用方必须读取而不是假设固定位置。

### 2.3 权限与发布

测试按 RW→RX 发布代码，从不使用 RWX。写入通过 `PinnedSpan`，
并在改权限**之前**释放 pin——`Protect` 在仍有 writer 持 pin 时会拒绝，
这正是保证代码事务完整的互锁。

### 2.4 所有权

`CreateThread` 的调用线程成为 owner，只有它能 `Run`/`Step`/销毁。
其他线程得到 `WrongThread` 而不是数据竞争。句柄是 id + generation，
销毁后的句柄可与复用槽位区分。

## 3. 又发现四个未文档化的嵌入方义务

继 [bionic 构建](fex-android-bionic-build.md) §4 的两个之后，本轮又踩到四个。
FEX 文档同样没有说明，任何嵌入实现都会遇到。

| 义务 | 触发点 | 症状 |
|---|---|---|
| **GDT 必须在首次执行前建立** | `Frontend.cpp:1603` 读 `CS.L` 判断 64 位模式 | `segment_arrays` 为空 → 段错误，fault addr `0x4`（位域偏移） |
| **CallRet stack 由嵌入方分配** | `Core.cpp:510` 对 `CallRetStackBase` 调 `VirtualDontNeed` | FEXCore 只读不分配；为空则 JIT 首次 call/ret 出错 |
| **guest 地址必须低于 2^36** | `LookupCache.h:196,323` 用 `VirtualMemSize-1` 掩码 RIP | 超限则 block 查找别名到错误槽位，**不报错** |
| **代码失效必须经过后端** | JIT 缓存已翻译的 block | 只改 guest 内存不够，重复地址会执行旧代码 |

前两项都通过原始指针从 `CPUState` 引用，**生命周期必须长于 FEX 线程**，
所以实现里按线程持有（`ThreadEntry::gdt` / `ThreadEntry::callret`）。

第三项是本轮最隐蔽的一个，详见 §4.2。它促成 API 增加
`BackendCapabilities::max_guest_address` 与 `AddressSpaceConfig::max_address`。

第四项促成了 API 增加 `CpuContext::InvalidateCode`：
`GuestAddressSpace::InvalidateCode` 只记录 generation，
真正持有翻译结果的是后端，失效必须传达到那里。它要求 `QuiescenceToken`——
在有线程正在执行时丢弃 block 就是 use-after-free。

## 4. 未解决的阻断：guest 指令的效果不落到 CPUState

### 4.1 现象

每次 `Run` **确实编译 2 个 block**，dispatcher 正常进出，RIP 被正确更新到 return gate，
但所有 guest 指令的架构效果都不存在：寄存器保持初值，guest 栈全零。

### 4.2 已定位的两个真实缺陷（其一已修）

**缺陷 A：guest 地址超出 FEXCore 的可寻址范围（已修）**

`ContextImpl` 把 `Config.VirtualMemSize` 固定为 `1<<36`（64 GB），64 位 guest 下不可配置。
`LookupCache` 在**插入和查找时都**用 `(VirtualMemSize - 1)` 掩码 guest RIP
（`LookupCache.h:196,323`）。

设备实测：内核给的 reservation 落在 `0x70bc410000`（约 451 GB），
return gate 落在 `0x735e41d000`——**两者都超出 64 GB**。掩码后：

```
guest rip 0x70bc410000 → 掩码后 0xbc410000
gate      0x735e41d000 → 掩码后 0x35e41d000
```

不同地址会折叠到同一槽位，block 查找与插入互相错位，且**不报任何错误**。

修法：`AddressSpaceConfig::max_address` 与 `BackendCapabilities::max_guest_address`。
reservation 与 return gate 都用 mmap hint 落到限内，**并在之后校验实际地址**——
hint 只是建议，校验才是约束。放不下时明确失败，不返回后端无法寻址的内存。

实测内核会尊重低位 hint（`0x100000000`–`0x800000000` 全部命中）。
两个 hint 必须错开：reservation 在低半区，gate 因此 hint 到接近上限处。

修复后地址正确（code `0x7f8010000`，gate `0xfc0000000`），但**指令效果仍然缺失**。

### 4.3 逐项排除记录

| 检查 | 结果 | 排除了什么 |
|---|---|---|
| 入口 RIP 与其处字节 | `0x7f8010000`，`49 01 c8 …` 正确 | 入口错误 / 代码没写进去 |
| 初始寄存器 | `r8=0x1000 r9=0x234` 正确 | 种子未生效 |
| GDT / cs_idx | 非空，`0x30` | 段状态缺失（本轮已修） |
| CallRet stack | 已分配，含 guard page | 该结构缺失（本轮已修） |
| L1/L2 lookup 指针 | 非零 | LookupCache 未初始化 |
| 可执行范围查询 | 无 miss | 范围未注册 |
| FEX 自身日志 | 无输出 | FEXCore 内部报错 |
| **每次 Run 编译块数** | **2** | **翻译未发生**（早期误判，已更正） |
| `CurrentFrame == &BaseFrameState` | true | frame 身份错乱 |
| 退出后 `BaseFrameState.rip` | 已更新为 gate | JIT 完全不回写状态 |
| guest 栈内容 | 全零 | — 确证指令效果未留存 |
| 编译覆盖的 guest page | fixture 与 gate 各一 | fixture 未被翻译（见 §4.4） |

**关键更正**：早期记录"首次 Run 编译 0 个 block"是测量错误——
计数打印在 `Run` **之前**，读到的是上一次的累计值。修正后每次都是 2，
所以翻译确实发生了，方向应从"没翻译"改为"翻译了但效果不落地"。

### 4.4 已确证：fixture 确实被翻译并执行

两条独立证据推翻了"guest 没跑"的假设：

**证据一：两个 block 都被编译。** 通过覆写 `SyscallHandler::MarkGuestExecutableRange`
（FEXCore 每编译一个 guest page 的代码就调用一次）观察到：

```
compiled code covering 0x7f8010000 +0x1000   ← fixture 本体
compiled code covering 0xfc0000000 +0x1000   ← return gate
```

**证据二：关闭 `EnableExitOnHLT` 后行为改变。** 用 `GUEST_CPU_NO_EXIT_ON_HLT=1`
关掉它，程序不再干净返回，而是 SIGSEGV 崩在 `[anon:FEXMem_Misc]`（dispatcher 自身代码）。
这正是 `Dispatcher.cpp:430-431` 在非 ExitOnHLT 模式下**故意**执行的空指针加载。

也就是说：**guest 确实执行到了 gate 的 HLT**，返回门机制按设计工作。
问题只在于寄存器效果没有留存。

`SpillStaticRegOptions` 的默认掩码是 `~0U`（全部寄存器），
所以"退出路径没 spill GPR"这条也不成立。

### 4.5 剩余的唯一疑点

fixture 被翻译、被执行、到达 gate，`SpillStaticRegs` 写的就是 `State.gregs`，
掩码也覆盖全部 GPR——但 `State.gregs` 里没有结果。

这已超出静态阅读能回答的范围，需要在设备上用 LLDB 实际观察：
在 `SpillStaticRegs` 生成的 store 指令处下断点，看它究竟写到了哪个 `STATE` 基址。
怀疑点是 `STATE`（x28）指向的 frame 与我们读的 `CurrentFrame` 不是同一个对象，
尽管 `CurrentFrame == &BaseFrameState` 已验证为 true。

见 [host LLDB → guest 工作流](fex-lldb-host-guest-workflow.md)；
注意该文档强调 `STATE=x28` 是特定 JIT 布局的结论，必须对照实际部署版本确认。

### 4.6 已排除、不要重复的路径

- `CONFIG_DUMPIR` / IR dump：`OpDispatcher::SetDumpIR` 全仓无调用者，无输出（已实测）
- "spill 写到别处"：`SpillStaticRegs` 就是写 `State.gregs`（`Arm64Emitter.cpp:719-728`）
- "spill 掩码排除了 GPR"：默认 `GPRSpillMask = ~0U`
- "首次 Run 没编译"：测量错误，实际每次编译 2 个 block
- "fixture 没被翻译"：已确证两个 block 都编译（§4.4）
- "guest 没到达 gate"：已确证（§4.4 证据二）
- 地址超限：已修复并验证（§4.2），是真实缺陷但不是本症状的唯一原因

### 4.7 复现方法

```bash
scripts/android/build-fexcore-android
adb push build/fexcore-android/guest_execution_tests /data/local/tmp/gx
adb shell chmod 755 /data/local/tmp/gx

# 正常运行：契约 9/9 通过，执行类 12 项失败
adb shell /data/local/tmp/gx

# 观察 FEX 内部日志与编译覆盖范围
adb shell "GUEST_CPU_DEBUG=1 /data/local/tmp/gx"

# 关闭 ExitOnHLT：应当崩在 dispatcher 的故意 fault，证明执行到达 gate
adb shell "GUEST_CPU_NO_EXIT_ON_HLT=1 /data/local/tmp/gx"
```

## 5. 已实现的 API 表面

| 方法 | 状态 |
|---|---|
| `CreateContext` / `QueryBackendCapabilities` | 可用 |
| `CreateThread` / `DestroyThread` | 可用，含映射与权限校验 |
| `Run` | 能进出 dispatcher，但 guest 指令无效果（§4） |
| `Step` | **有意拒绝**（`Unsupported`）。需要 JIT block 长度限制，未接通；返回整个 block 冒充单步比拒绝更糟（T07） |
| `ReadRegisters` / `WriteRegisters` | 可用，含 stopped/epoch/owner 校验 |
| `InvalidateCode` | 可用，但目前是整表清除而非按范围 |

`InvalidateCode` 的粒度说明：FEXCore 面向嵌入方的入口是
`ClearCodeCache`，它整体丢弃线程的翻译结果。比请求的范围粗，
但**永不陈旧**——多失效只是多一次重编译，少失效会让线程执行已不存在的代码。

## 6. 能力声明的取舍

`QueryFexCapabilities` 声明 `BaseInteger | Sse2`，**不声明 AVX**。
C04 是条件 MUST：没有实际执行与状态恢复测试就声明特征等于虚报。
C03 验证请求 AVX 时被明确拒绝。

`step_scope` 报 `None`，与 `Step` 返回 `Unsupported` 一致——
不声明做不到的事。
