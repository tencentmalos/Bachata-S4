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

## 3. 又发现三个未文档化的嵌入方义务

继 [bionic 构建](fex-android-bionic-build.md) §4 的两个之后，本轮又踩到三个。
FEX 文档同样没有说明，任何嵌入实现都会遇到。

| 义务 | 触发点 | 症状 |
|---|---|---|
| **GDT 必须在首次执行前建立** | `Frontend.cpp:1603` 读 `CS.L` 判断 64 位模式 | `segment_arrays` 为空 → 段错误，fault addr `0x4`（位域偏移） |
| **CallRet stack 由嵌入方分配** | `Core.cpp:510` 对 `CallRetStackBase` 调 `VirtualDontNeed` | FEXCore 只读不分配；为空则 JIT 首次 call/ret 出错 |
| **代码失效必须经过后端** | JIT 缓存已翻译的 block | 只改 guest 内存不够，重复地址会执行旧代码 |

前两项都通过原始指针从 `CPUState` 引用，**生命周期必须长于 FEX 线程**，
所以实现里按线程持有（`ThreadEntry::gdt` / `ThreadEntry::callret`）。

第三项促成了 API 增加 `CpuContext::InvalidateCode`：
`GuestAddressSpace::InvalidateCode` 只记录 generation，
真正持有翻译结果的是后端，失效必须传达到那里。它要求 `QuiescenceToken`——
在有线程正在执行时丢弃 block 就是 use-after-free。

## 4. 未解决的阻断：dispatcher 进入后不翻译

### 4.1 现象

首次 `Run` 编译 **0 个 block**，第二次编译 2 个。guest 栈完全没有被写过。

### 4.2 实测数据

用 `GUEST_CPU_DEBUG=1` 采集（该环境变量会装上 FEX 自己的日志 handler）：

```
pre-run  rip=0x70bc410000 rsp=0x70bc423ff0 r8=0x1000 r9=0x234
         L1=0x12d2c4000 L2=0x124ac4000 callret_sp=0xffcff000
         gdt=0xb40000720c404d50 cs_idx=0x30
compiles so far: 0                      ← 首次 Run 之前
post-run rip=0x735e41d000 (gate=0x735e41d000) r8=0x1000 rax=0x0
```

逐项排除的结论：

| 检查 | 结果 | 排除了什么 |
|---|---|---|
| 入口处 guest 内存字节 | `49 01 c8 4d 89 c2 …` 正确 | 代码没写进去 |
| 初始寄存器 | `r8=0x1000 r9=0x234` 正确 | 种子没生效 |
| GDT / cs_idx | 非空，`0x30` | 段状态缺失（已修） |
| L1/L2 lookup 指针 | 非零 | LookupCache 未初始化 |
| 可执行范围查询 | 无 miss 日志 | 范围未注册 |
| FEX 自身日志 | **无任何输出** | FEXCore 认为没有错误 |
| guest 栈内容 | 全零 | **确证：没有任何指令执行** |
| `PreCompile` 计数 | 首次 0，之后 2 | **确证：首次未进入翻译** |

`callret_sp=0xffcff000` 看似异常，但它是 JIT 自己 spill 的寄存器值——
说明 dispatcher 确实运行了，只是没有翻译任何 block。

### 4.3 已排除的假设

- **不是**代码未写入或权限不对（字节和权限都已验证）
- **不是**寄存器种子丢失（pre-run 已确认）
- **不是**缺 GDT（已修，指针非空）
- **不是**缺 CallRet stack（已加，含 guard page）
- **不是**陈旧代码缓存（调整测试顺序后首个 fixture 仍失败）
- **不是**可执行范围未注册（无 miss 日志）
- **不是** FEXCore 内部报错（日志 handler 已装，无输出）

### 4.4 下一步排查方向

按可能性排序：

1. **dispatcher 入口状态**。`ExecuteDispatch` 从 `AbsoluteLoopTopAddress` 开始，
   可能还需要 `CpuStateFrame` 中某个未初始化字段。对比
   `Dispatcher::InitThreadPointers`（`Dispatcher.cpp:2611`）设置的全部指针，
   逐个确认在我们的路径上都有效。
2. **`ExitOnHLT` 的退出路径**。`Dispatcher.cpp:424` 显示该模式下退出走的是
   **`GuestSignal_SIGSEGV` handler**，即一个 fault 路径。
   需要确认首次 Run 是否在第一条指令之前就走了这条路。
3. 用 LLDB 在 `CompileBlock` 下断点，直接观察首次 `Run` 是否到达，
   以及 `AbsoluteLoopTopAddress` 处的实际控制流。
   参考 [host LLDB → guest 工作流](fex-lldb-host-guest-workflow.md)。

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
