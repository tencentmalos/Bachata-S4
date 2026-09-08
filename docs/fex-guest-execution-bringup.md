# FEX guest 执行接入（backend adapter）

本文记录把 FEXCore 接到 [V0 公共 CPU API](specs/android-fex-v0-api.md) 的实现、
当前实测状态，以及尚未解决的问题。

- 公共 API：[`src/core/guest_cpu/api/context.h`](../src/core/guest_cpu/api/context.h)
- 后端实现：[`src/core/guest_cpu/fex/fex_context.cpp`](../src/core/guest_cpu/fex/fex_context.cpp)
- fixture 源码：[`tests/guest_cpu/fixtures/guest_fixtures.S`](../tests/guest_cpu/fixtures/guest_fixtures.S)
- 测试：[`tests/guest_cpu/guest_execution_tests.cpp`](../tests/guest_cpu/guest_execution_tests.cpp)
- 前置：[Android bionic 构建](fex-android-bionic-build.md)、[host page 适配](fex-host-page-size-adaptation.md)
- 上一轮审核：[2026-09-08](validation/v0/review-2026-09-08.md)

## 1. 当前状态

**guest x86-64 已在 Swan 实机上真实执行并产生正确的架构效果**，但仅限纯寄存器指令；
所有触及 guest 内存的指令仍然失败。

设备实测 30 项中 19 通过、11 失败：

| 类别 | 结果 |
|---|---|
| 整数算术（`add`/`mov`/`xor`/`sub`，含 r8–r11） | **全部通过** |
| 分支（跳转成立） | 通过 |
| 分支（跳转不成立，需写 rax） | 失败 |
| push/pop、显式 store | 失败 |
| SSE2（经 xmm 往返） | 失败 |
| 契约检查（能力、句柄、epoch、拒绝路径） | 9/9 通过 |
| D05 裸 HLT 分类 | 失败，仍报 `Returned` |

整数 fixture 完整通过是关键证据：`add r8, r9` 得到 0x1234、
`mov r10, r8` 后 `sub 5` 得到 0x122F、`xor r11, r11` 归零。
这是翻译链路首次产生正确结果。

## 2. 已定位并修复的根因：`Is64BitMode` 未生效

这是让**所有** guest 指令无效果的原因，值得单独记录，因为它的表现极具误导性。

`FEXCore::Config::ReloadMetaLayer()` 会用已注册的 config layer 重建 meta layer，
**丢弃此前 `Set` 的值**。原实现是先 `Set` 后 `Reload`，于是
`CONFIG_IS64BIT_MODE` 实际为 unset——设备上读回确认为 `(unset)`。

`ContextImpl` 把 unset 当作 32 位：它将 `VirtualMemSize` 收窄到 `1<<32`，
decoder 随后用 32 位模式解码 64 位 guest 字节。结果是每个 fixture 都"到达 return gate"
却没有任何寄存器变化——看起来像执行了，实际全部错译。

`FEXInterpreter` 在 `Set` 之后**根本不调用** `ReloadMetaLayer`，
所以照抄它的写法反而看不出问题。

修法是把 `Set` 移到 `Reload` 之后，并**读回校验**：
静默的 32 位 context 会错译每条指令却仍像在运行，不校验就无法察觉。

## 3. 又发现四个未文档化的嵌入方义务

继 [bionic 构建](fex-android-bionic-build.md) §4 的两个之后，本轮又踩到四个：

| 义务 | 触发点 | 症状 |
|---|---|---|
| **GDT 必须在首次执行前建立** | `Frontend.cpp:1603` 读 `CS.L` 判断 64 位模式 | `segment_arrays` 为空 → 段错误，fault addr `0x4` |
| **CallRet stack 由嵌入方分配** | `Core.cpp:510` 对 `CallRetStackBase` 调 `VirtualDontNeed` | FEXCore 只读不分配 |
| **`Set` 必须在 `ReloadMetaLayer` 之后** | `Config.cpp` 的 meta layer 重建 | 见 §2，静默降级为 32 位 |
| **代码失效必须经过后端** | JIT 缓存已翻译的 block | 只改 guest 内存不够 |

前两项通过原始指针从 `CPUState` 引用，**生命周期必须长于 FEX 线程**，
实现里按线程持有（`ThreadEntry::gdt` / `ThreadEntry::callret`）。

## 4. 未解决：guest 内存访问无效果

### 4.1 现象

纯寄存器指令正确，触及内存的一律失败：

- `push rdi; push rsi; pop rax; pop rcx` → rax/rcx 均为 0，rsp 却正确平衡
- `mov [rsp-16], rdi` → 该槽读回 0
- `movq xmm0, rdi; paddq; movq rax, xmm0` → rax 为 0，xmm0 保持 `0xdeadbeef`（FEX 的调试初值）
- 需要写 rax 的分支路径失败，直接跳过的路径通过

rsp 平衡说明 push/pop 的**寄存器**副作用生效，只有内存读写没有。

### 4.2 已确证：store 根本没有执行

`store_memory` fixture 现在在返回前把两个槽**读回 rax/rcx**，
三种读法互相印证：

| 读法 | 结果 |
|---|---|
| guest 自己读回（rax/rcx） | 0 |
| host 经 `PinnedSpan` 读 | 0 |
| host 直接解引用指针 | 0 |

三者一致为 0，排除了两种可能：
不是"写了但 API 读不到"（直接读也为 0），
也不是"store 被优化掉"（guest 自己的 load 同样读不到，
若只是死代码消除，load 会读到原值而不是恰好为 0——何况该槽在运行前已被显式清零，
所以 0 表示这块内存从未被写过）。

结论：**内存访问指令没有产生任何效果**，而同一条指令流里的寄存器指令正常。

### 4.3 下一步

1. **让 JIT 反汇编真正输出**。后端已支持 `GUEST_CPU_DISASSEMBLE=blocks`
   （必须程序化 `Config::Set`，`FEX_DISASSEMBLE` 环境变量对嵌入方无效，
   因为我们不加载 FEX 的环境配置层）。但还需要：
   - FEXCore 以 `-DENABLE_VIXL_DISASSEMBLER=ON` 构建。实测该配置下
     `External/vixl/src/aarch64/disasm-aarch64.cc` 缺 `#include <map>`，
     libc++ 下编译失败；这是 VIXL 上游问题，需在自有 fork 修或提交 patch。
   - 输出走 `LogMan::Msg::IFmt`（INFO 级），确认日志 handler 不会过滤掉。
2. **检查 `VirtualMemSize` 与 reservation 的关系**。修好 `Is64BitMode` 后它是 `1<<36`，
   guest reservation 在 `0x7f8000000` 附近（约 34 GB），在范围内但接近上半区。
3. **确认 TSO/原子模式配置**。`CONFIG_TSOENABLED` 等未设置；
   若 FEX 因此走了需要额外支持的路径，内存操作可能被静默丢弃。
   考虑到 `Is64BitMode` 正是同类问题（未设置 → 静默降级），这条值得优先查。

### 4.3 已排除的路径

- fixture 编码错误：现由汇编器生成并反汇编回验，整数 fixture 已通过
- 陈旧代码缓存：单 fixture 连续三次 Run 结果完全一致
- 页边界放置：把 fixture 放在页内偏移 0x40 处行为不变
- 高地址寻址：见 §5，该假说本身不成立
- `CONFIG_DUMPIR`：`OpDispatcher::SetDumpIR` 全仓无调用者，无输出

## 5. 更正：64 GiB 不是 FEX 的能力上限

上一版称 FEXCore 无法寻址 `1<<36` 以上，依据是 `LookupCache` 对 guest RIP 做掩码。
**这个推理不成立**，[审核 R8](validation/v0/review-2026-09-08.md) 指出得对：
掩码只用于算索引，之后 `LookupCache.h:207` 与 `Dispatcher.cpp:211-218`
都会比较**完整地址**，不匹配就落到 L3 或重新编译。索引碰撞的代价是一次 miss，
不会执行错误的 block。

代码中的 `kGuestAddressPolicyLimit` 保留为 **V0 放置策略**（让地址确定、
避免在排查执行问题的同时引入别名路径），已改名并注明它不是 FEX 能力上限。
要移除它需要同一 fixture 在低/高 VA 的对照实验，不能只删常量。

## 6. fixture 由汇编器生成

[审核 R2](validation/v0/review-2026-09-08.md) 发现手写编码出错：
`49 01 C8` 是 `add r8, rcx` 而非 `add r8, r9`（后者应为 `4D 01 C8`），
所以 C01 即使后端完全正确也不可能通过，而这个失败被误读成后端缺陷。

现在 fixture 全部来自 [`guest_fixtures.S`](../tests/guest_cpu/fixtures/guest_fixtures.S)，
由 [`generate-guest-fixtures`](../scripts/android/generate-guest-fixtures) 汇编、抽取，
**再反汇编回来**写进生成的头文件。注释因此不可能与字节脱节，
汇编失败是构建错误而不是错误的测试结果。

测试在失败时打印 fixture 的字节数与反汇编，便于区分"后端错了"和"fixture 错了"，
无需自行推导编码。

## 7. 已实现的 API 表面

| 方法 | 状态 |
|---|---|
| `CreateContext` / `QueryBackendCapabilities` | 可用 |
| `CreateThread` / `DestroyThread` | 可用，含映射与权限校验 |
| `Run` | 可用；纯寄存器指令正确，内存指令见 §4 |
| `Step` | **有意拒绝**（`Unsupported`），未接通 JIT block 长度限制（T07） |
| `ReadRegisters` / `WriteRegisters` | 可用，含 stopped/epoch/owner 校验 |
| `InvalidateCode` | 可用，但目前整表清除而非按范围 |

`Run` 的锁协议已按[审核 R5](validation/v0/review-2026-09-08.md) 修正：
认领 running 与解析句柄在同一次持锁内完成，执行期间放锁（guest block 时长无上界），
结束后重新持锁发布结果。原实现由一个在返回时释放锁的 helper 交出裸指针再解引用。

**仍未完成的是 R5 的另一半**：context 级的 quiesce 门闩，
使 `Run` 与 `InvalidateCode` 不能交错。当前 token 不足以作为跨线程改代码的安全凭据。
