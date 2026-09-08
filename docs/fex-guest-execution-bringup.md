# FEX guest 执行接入（backend adapter）

本文记录把 FEXCore 接到 [V0 公共 CPU API](specs/android-fex-v0-api.md) 的实现、
已经跑通的部分、以及**尚未解决的一个阻断**及其全部实测数据。

- 公共 API：[`src/core/guest_cpu/api/context.h`](../src/core/guest_cpu/api/context.h)
- 后端实现：[`src/core/guest_cpu/fex/fex_context.cpp`](../src/core/guest_cpu/fex/fex_context.cpp)
- 测试：[`tests/guest_cpu/guest_execution_tests.cpp`](../tests/guest_cpu/guest_execution_tests.cpp)
- 前置：[Android bionic 构建](fex-android-bionic-build.md)、[host page 适配](fex-host-page-size-adaptation.md)

## 1. 当前状态（务必先读）

**契约层全部通过，guest 指令尚未真正执行。**

实机 9/9 契约检查通过：能力声明、重复 context 拒绝、未映射入口拒绝、Step 拒绝、
stale epoch 拒绝、失效句柄拒绝。

但**没有任何 guest 指令产生效果**。这一点有决定性证据（见 §4），
不要把当前的 `StopReason::Returned` 当作"执行成功"——它只说明 dispatcher 退出到了 return gate。

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
| guest 栈内容 | 全零 | — 确证指令无效果 |

**关键更正**：早期记录"首次 Run 编译 0 个 block"是测量错误——
计数打印在 `Run` **之前**，读到的是上一次的累计值。修正后每次都是 2，
所以翻译确实发生了，方向应从"没翻译"改为"翻译了但效果不落地"。

### 4.4 下一步排查方向

RIP 能正确更新说明 JIT 确实回写了部分状态，但 GPR 没有——
这把范围收窄到**静态寄存器分配（SRA）的 spill**：

1. **确认退出路径是否真的 spill 了 GPR**。`ExitOnHLT` 的返回在
   `GuestSignal_SIGSEGV`（`Dispatcher.cpp:416-427`），它前面有 `SpillStaticRegs`。
   但真实 HLT 走的是 `GuestSignal_SIGILL`（`:396`），那条路径 spill 之后执行
   **host `hlt(0)`**，会崩溃而不是返回。既然我们干净返回了，
   说明退出并非来自 HLT 指令本身——需要查清实际退出点。
2. **确认 block 是否真的在执行 fixture**。编译了 2 个 block，
   但可能是 gate 与某个 stub，而非 fixture 本体。
   用 `-DENABLE_VIXL_DISASSEMBLER` 或 IR dump 观察实际生成的代码。
3. LLDB 在 `CompileBlock` 与 `SpillStaticRegs` 处下断点直接观察，
   见 [host LLDB → guest 工作流](fex-lldb-host-guest-workflow.md)。


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
