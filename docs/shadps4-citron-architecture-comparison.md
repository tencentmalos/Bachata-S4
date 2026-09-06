# shadPS4 与 Citron 架构相似性、差异及 Android ARM64 CPU 方案

> **复核状态（2026-09-04）**：本文第 2、3 节的代码断言已逐条对照 shadPS4 `e1cf475263c8` 复核，全部成立，详见[附录 A](#附录-a2026-09-04-复核记录)。
> 第 4 节的方案选型需要修正：已存在一个跑通阶段 0/1 的第三方 Android 实现（Bachata S4），且它同时锁定了 Box64 与 FEX，架构选择也与本文 4.2 节不同。见[附录 B](#附录-b已有第三方-android-实现bachata-s4)。

## 1. 范围与结论

分析快照：

- shadPS4：`e1cf475263c8`（2026-08-01）
- Citron：`c5e47458558b`（2026-08-02，本地 `feature/tencentmalos/basic_version`；该分支已于 2026-09 前切换为 `feature/malos/xr_support`，commit 本身仍可达）
- 目标平台：Android ARM64
- CPU 目标：在原生 ARM64 的 shadPS4 进程内，将 PS4 用户态 x86-64 guest 指令动态翻译为 AArch64；**不采用 VM、microVM、chroot/proot，也不启动一个完整的 x86 Linux 用户空间**。

核心结论：

1. shadPS4 与 Citron 在“现代 HLE 游戏机模拟器”的分层上相似，但 CPU ISA、内核接口、GPU ISA 和 Android 完成度差异很大；Citron 不能作为 shadPS4 的直接移植底座。
2. Citron 的 Dynarmic/NCE 处理的是 ARM guest。Dynarmic 没有 x86-64 guest 前端，NCE 又依赖 guest 与 host 同为 ARM64，因此两者均不能解决 PS4 x86-64 到 Android ARM64 的执行问题。
3. 现有 shadPS4 没有可移植 CPU 后端：它在 x86-64 host 上修补少量指令后直接跳入 guest 代码。ARM64 分支当前会进入 `UNREACHABLE`。
4. 对“进程内 x86-64 guest → ARM64 host”而言，**FEXCore 是最合适的首选底座**。官方把它定义为可嵌入的 x86/x86-64 用户态模拟库，目标 host 是 AArch64，并明确说明它可以为 Android AArch64 构建。
5. 但不存在把 FEXCore 链进来就能运行 PS4 游戏的现成 shadPS4 适配层。最大的工程量不是 ELF 解码，而是 HLE ABI 桥、guest 线程/回调、TLS/异常、地址空间及代码缓存失效。

> **2026-09-04 修正第 5 条**：「不存在现成适配层」在本文写作时成立，但现已被第三方实现推翻。Bachata S4（shadPS4 fork）在 SM8650 上提交了 FEXCore 阶段 0 与阶段 1 的实机证据，覆盖 GPR/RFLAGS/XMM/bridge/threads/TLS/invalidation/teardown 全部通过。详见[附录 B](#附录-b已有第三方-android-实现bachata-s4)。它走的不是本文 4.2 节的进程内后端结构，而是容器化运行时；这一取舍需要单独评估。

换句话说：

> 有直接可用的“CPU 翻译核心”，首选 FEXCore；没有直接可用的“shadPS4 Android ARM64 CPU 后端”。

## 2. shadPS4 与 Citron 的相似性

两者都延续了 Yuzu/Citra 系现代 HLE 模拟器常见的分层：

| 层次 | shadPS4 | Citron | 相似性 |
|---|---|---|---|
| 前端与配置 | Qt/SDL、配置、输入、日志 | Qt/Android、配置、输入、日志 | 高层组织方式相似 |
| 可执行文件加载 | PS4 SELF/ELF、模块与 NID 重定位 | Switch NSO/NRO、服务与加载流程 | 目标格式不同，职责相似 |
| 系统软件 | `libSce*` HLE | Horizon 内核与服务 HLE | 都做 HLE，API 模型完全不同 |
| CPU | x86-64 原生执行加补丁 | ARM32/64 Dynarmic，ARM64 NCE | 抽象职责相似，实现不可复用 |
| GPU | GCN/GNM → Vulkan | Maxwell/NVN → Vulkan/OpenGL | 后端概念相似，前端 ISA 不同 |
| Shader | GCN shader → IR/SPIR-V | Maxwell shader → IR/SPIR-V/GLSL | 编译管线思想相似，解码和语义不同 |
| 内存 | 尽量按 PS4 guest VA 映射到 host | 页表、fastmem、设备内存 | 都追求 fastmem，但地址模型不同 |

按本地快照进行的粗粒度统计也说明二者更接近“同类架构”而非“同一代码库”：同相对路径文件只有约 77 个；对这些同路径文件按行数加权的文本相似度约为 26%。这类数字只能用于判断代码亲缘关系，不能等同于可移植工作量。

可合理借鉴的主要是：

- Android 工程、生命周期、输入/触摸、权限、存储与 UI 组织；
- CPU 后端接口化、线程上下文、fastmem、代码失效等设计模式；
- Vulkan 平台接入、缓存、调试和性能统计框架。

不能直接搬用的主要是：

- Dynarmic ARM 前端和 Citron NCE；
- Horizon 内核/服务 HLE；
- Maxwell 命令处理、纹理/格式和 shader 解码；
- 针对 Switch 地址空间和同步模型的实现。

## 3. 实现差异最大的部分

### 3.1 CPU 执行模型

Citron 将 guest ARM 指令交给 Dynarmic，或者在 Android ARM64 上使用 NCE。其进程初始化时会在 `ArmDynarmic64` 与 `ArmNce` 之间选择：

- `/Users/bytedance/workspace/emulations/switch/citron/src/core/hle/kernel/k_process.cpp`
- `/Users/bytedance/workspace/emulations/switch/citron/src/core/arm/dynarmic/arm_dynarmic_64.cpp`
- `/Users/bytedance/workspace/emulations/switch/citron/src/core/arm/nce/arm_nce.cpp`

shadPS4 的模式则是：

1. 把 PS4 ELF 代码映射到进程地址空间；
2. 用 Zydis 扫描少量不能安全原生执行或需特殊处理的 x86 指令；
3. 用 Xbyak 原地生成补丁或跳板；
4. host x86-64 线程直接跳到 guest RIP。

证据包括：

- [`src/core/cpu_patches.cpp`](../src/core/cpu_patches.cpp) 中的 Zydis 解码和 Xbyak patch/trampoline；
- [`CMakeLists.txt`](../CMakeLists.txt) 只在 `ARCHITECTURE == x86_64` 时加入 `cpu_patches.cpp`；
- [`src/core/linker.cpp`](../src/core/linker.cpp) 的 `RunMainEntry` 只有 x86-64 汇编实现，其他架构直接 `UNREACHABLE`。

因此 Android ARM64 不是给现有补丁器增加 AArch64 跳板即可。guest x86-64 主体也无法原生执行，必须新增完整的 DBT/JIT CPU 后端。

### 3.2 HLE 调用边界

这是 CPU 移植最容易低估、也是最关键的差异。

当前 `LIB_FUNCTION` 通过 `HOST_CALL(function)` 取得 host 函数地址，并把这个地址存进 `SymbolsResolver`。链接器随后把 guest import 直接重定位到该地址。在 x86-64 host 上，guest 和 host 都使用兼容的 x86-64 SysV 调用约定，因此 guest 可以直接 `call` 到 shadPS4 的 C++ HLE 函数。

在 Android ARM64 上，这个地址是 AArch64 代码地址。把它写进 x86-64 guest 的 GOT/PLT 后让 guest RIP 跳过去既无法解码，也不满足 ABI。必须改成：

```text
PS4 x86-64 guest call
        |
        v
x86-64 guest callgate / thunk stub
        |
        v
FEXCore 退出到 host 或执行专用 thunk
        |
        v
读取 x86 SysV 参数：RDI/RSI/RDX/RCX/R8/R9、XMM、guest stack
        |
        v
调用原生 AAPCS64 shadPS4 HLE 函数
        |
        v
把返回值写回 RAX/RDX/XMM0，恢复 guest RIP/RSP
```

建议让符号表保存 `HleCallId`、函数签名/编组器及 native target，而不是把 native 函数指针当成 guest 可执行地址。链接器为每个 HLE import 返回 guest callgate 地址。

反方向同样需要桥接：HLE 保存的回调、线程入口、析构器或事件回调如果是 guest 函数指针，native ARM64 代码不能直接调用它，必须通过 `CallGuest(thread, rip, args...)` 回到 DBT。

### 3.3 线程、TLS、异常与内存顺序

当前 guest pthread 入口最终由 host 线程直接切换栈并调用。在 ARM64 后端中，每个 PS4 guest 线程应有独立的 x86 CPU state，并在对应 host 线程上执行 DBT；不能再把 guest start routine 当作 native 函数指针调用。

还需要处理：

- x86 FS/GS、PS4 TLS/TCB 与 Android pthread TLS 的隔离；
- guest fault、非法指令、断点及 host SIGSEGV/SIGILL 到 PS4 异常语义的转换；
- x86 强内存顺序在 ARM 弱内存模型上的保持；
- 原子指令、unaligned access、x87/SSE/AVX 和 PS4 实际使用的扩展；
- guest 修改可执行页后的 JIT block invalidation；
- Android 设备不同页大小、W^X 与 JIT code cache 管理。Android 官方的 [16 KiB page-size 指南](https://developer.android.com/guide/practices/page-sizes) 应作为最低兼容性检查项。

### 3.4 系统和 GPU

Citron 面向 Horizon OS 与 Maxwell GPU，shadPS4 面向 Orbis/FreeBSD 衍生用户态接口与 AMD GCN。即使 CPU 后端能够执行 eboot，`libSce*` HLE、GNM 命令、GCN shader、视频/音频和同步语义仍由 shadPS4 自己负责；FEXCore 不替代这些层。

## 4. Android ARM64 的进程内 x86-64 翻译方案

### 4.1 方案比较

> **复核补注**：下表的选型逻辑经复核仍成立，但需知 Bachata S4 在实践中**同时**锁定 FEXCore 与 Box64，且以 Box64 为当前承载路径。见[附录 B](#附录-b已有第三方-android-实现bachata-s4)。

| 方案 | 进程内作为库 | Android ARM64 | x86-64 → AArch64 | 对 shadPS4 的判断 |
|---|---:|---:|---:|---|
| **FEXCore** | 是 | 官方说明可构建 | 是，主目标 | **首选正式后端**；仍需专用 adapter/HLE bridge |
| Unicorn | 是，C API 清晰 | 官方确认支持 | 是，基于 QEMU JIT | 适合先跑通和差分测试；最终游戏性能必须实测，通常不作为首选高性能后端 |
| Box64 dynarec | 项目主要是完整用户态运行时 | 有 Android/Termux 相关构建 | 是 | 翻译能力强，但没有面向模拟器的稳定、文档化 CPU-library API；抽取会形成长期 fork |
| QEMU TCG | 可通过内部接口改造 | 可移植 | 是 | 技术上可行，但直接嵌入和维护成本高；原型优先用 Unicorn |
| Dynarmic | 是 | 是 | **否，仅 ARM guest** | 不适用 |
| Citron NCE | 否，依赖 ARM guest 原生执行 | 是 | **否** | 不适用 |

FEXCore 的官方说明值得区分为两部分：完整 FEX Linux 用户态环境不支持 Android；但独立的 FEXCore 是通用 x86/x86-64 模拟库，可以为 Android AArch64 构建并供其他模拟器使用。参见 [FEX FAQ](https://wiki.fex-emu.com/index.php/FAQ) 和 [FEXCore README](https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Readme.md)。后者还说明其目标 host 是 ARMv8.1+ AArch64、支持多线程及 x86 强内存模型处理。

FEXCore 的公开接口已经包含 context 创建、guest thread 创建/执行、callback、代码编译与失效，以及 syscall/thunk handler 等能力；这意味着它的边界与 shadPS4 所需 CPU backend 基本对齐。但 FEX 的现成 Linux syscall/ELF loader 不应成为 PS4 执行路径，PS4 加载器和 HLE 仍由 shadPS4 提供。

[Unicorn](https://github.com/unicorn-engine/unicorn) 明确提供 x86-64、多平台、Android 和架构无关 C API，因此是最容易做功能 PoC 的替代项。[Box64](https://github.com/ptitSeb/box64) 的 ARM64 dynarec 已覆盖大量现代 x86 扩展，但官方构建和使用方式以运行 ELF/进程、库包装和 Linux/Termux 环境为中心，而非可嵌入 CPU core；因此这里不把它列为“直接可用库”。

### 4.2 推荐结构

```text
Android ARM64 shadPS4 process

  PS4 SELF/ELF loader ──────── maps guest code/data at PS4 virtual addresses
          |
          v
  CpuBackend interface
          |
          +── X64NativeBackend       (现有桌面 x86-64 路径)
          |
          `── FexArm64Backend        (新增 Android/Linux AArch64 路径)
                    |
                    +── one FEX CPU state per PS4 guest thread
                    +── x86-64 blocks -> AArch64 JIT cache
                    +── HLE callgate/thunk dispatcher
                    +── reverse guest callback entry
                    `── fault, TLS, TSO and code invalidation hooks

  libSce HLE / GNM / Vulkan / audio / input remain native ARM64 C++
```

建议先抽象最小接口，而不是让 linker 直接依赖 FEXCore：

```cpp
class GuestCpuBackend {
public:
    virtual GuestThread* CreateThread(VAddr rip, VAddr rsp, const CpuState* inherit) = 0;
    virtual StopReason Run(GuestThread*) = 0;
    virtual GuestValue CallGuest(GuestThread*, VAddr rip, const GuestCallFrame&) = 0;
    virtual VAddr RegisterHostCall(HleCallDescriptor) = 0;
    virtual void InvalidateCode(VAddr start, u64 size) = 0;
};
```

这样可以保留桌面 x86-64 direct-execution backend，并让 Android ARM64 单独选择 FEXCore。Citron 值得借鉴的正是这种“进程/线程拥有 CPU interface”的组织方式，而不是它的 ARM translator 本身。

### 4.3 FEXCore 接入要点

1. **只嵌入 FEXCore**：不接入 FEX 的 Linux ELF loader、rootfs 或完整 syscall 环境。
2. **沿用 shadPS4 loader**：SELF/ELF、模块、NID、PS4 地址布局仍由 shadPS4 管理。
3. **新增 guest callgate 页**：每个 import 指向一段合法 x86-64 stub；stub 携带 `HleCallId` 并触发 FEX thunk/受控退出。
4. **生成 ABI adapter**：至少覆盖整数/指针、浮点/XMM、结构体、变参、栈参数与多寄存器返回。不能只做六个整数寄存器。
5. **支持 reverse callback**：pthread start routine、模块 init/fini、用户回调和 TLS destructor 都可能是 guest RIP。
6. **每 guest 线程一份 CPU state**：host pthread 负责调度 FEX thread context；阻塞型 HLE 调用时要保持可暂停/可取消语义。
7. **内存采用同址映射优先**：shadPS4 大量代码把 guest pointer 当 host pointer 使用。同址映射若在 Android VA 布局上不可满足，整个 HLE 指针模型都要扩大改造。
8. **把现有 patch 分层**：只为 x86 host 原生执行安全性服务的 Zydis/Xbyak patch 在 FEX 后端应禁用；真正的 PS4 CPU 行为修正需要在 FEX IR、受控 helper 或 guest patch 中重新实现。
9. **严格失效 JIT cache**：`mmap/mprotect/unmap`、模块装载和任何 self-modifying code 都必须通知 FEXCore。
10. **建立解释器/差分测试**：用 Unicorn 或 FEX interpreter 路径验证寄存器、flags、SIMD、异常和内存顺序，再对比 AArch64 JIT。

## 5. 建议的落地阶段

### 阶段 0：可行性门槛

- 用 Android NDK 构建最小 FEXCore 静态库或 `.so`；
- 在普通 Android app native 线程内执行手写 x86-64 函数块；
- 验证 RW→RX JIT cache、guest VA 映射、16 KiB page-size 设备和多线程；
- 验证目标 SoC 满足 FEXCore 的 ARMv8.1+ 要求。

### 阶段 1：shadPS4 CPU 骨架

- 引入 `GuestCpuBackend`；
- 保留 x86-64 native backend；
- 实现 FEX context/thread 生命周期、入口 RIP/RSP 和停止原因；
- 先运行不依赖 GPU 的 OpenOrbis 小程序。

### 阶段 2：HLE ABI 桥

- 改造 `LIB_FUNCTION`/`SymbolsResolver`，区分 guest 地址与 native target；
- 实现整数、指针、浮点、栈参数及返回值；
- 实现 guest→host HLE 和 host→guest callback；
- 优先覆盖进程、内存、线程、时间、文件和日志函数。

### 阶段 3：正确性

- TLS/FS、pthread、同步和原子指令；
- fault/signal/exception 转换；
- x87/SSE/AVX 指令覆盖和 CPUID；
- TSO、代码失效和 self-modifying code。

### 阶段 4：完整 Android 路径

- 接入 Citron 可借鉴的 Android UI/lifecycle/input/storage 结构；
- 完成 Vulkan surface、音频、手柄和暂停恢复；
- 做 block cache、fastmem、HLE thunk 和 TSO 的性能分析。

## 6. 风险排序

| 风险 | 级别 | 原因 |
|---|---|---|
| HLE ABI 与 reverse callback | 极高 | 现有代码依赖 guest/host 同为 x86-64 的直接函数调用 |
| guest VA 同址映射 | 极高 | HLE 广泛直接解引用 guest pointer；Android VA 冲突会放大改造范围 |
| TLS、异常与 pthread | 高 | 当前实现包含原生切栈和直接 guest 函数入口 |
| x86 TSO/原子语义 | 高 | ARM 内存模型更弱，错误通常只在高并发游戏中暴露 |
| 指令覆盖与精确 flags/SIMD | 高 | 能启动不等于能稳定运行商业游戏 |
| JIT/W^X/页大小 | 中高 | 不同 Android 版本与 16 KiB page 设备需要专门验证 |
| FEXCore 上游 API 变化 | 中 | 虽是库接口，仍需锁定版本并维护薄适配层 |

> **复核补注（2026-09-04）**：Bachata S4 的 SM8650 证据（pageSize 4096）已部分缓解「TLS/异常/pthread」与「HLE ABI 与 reverse callback」两项——阶段 1 的 `bridge`/`tls`/`teardown` contract 均通过。但两项风险仍不能降级：其证据是最小 harness 而非商业游戏负载，且「JIT/W^X/页大小」中的 16 KiB page 设备完全未被覆盖。
> 「guest VA 同址映射」一项在容器化路线下不成立（guest 跑在近乎真实的 Linux 用户态中），但在本文的进程内路线下依然是极高风险。

## 7. 最终建议

推荐技术路线为：

> **shadPS4 原生 Android ARM64 + FEXCore 进程内 DBT + shadPS4 自己的 PS4 loader/HLE/GPU。**

这不是虚拟机方案，也不要求 x86 Linux rootfs。FEXCore 只负责 CPU 指令和 guest CPU state；PS4 系统语义继续由 shadPS4 HLE 实现。

第一项 PoC 不应是启动完整游戏，而应是同时证明下面四件事：

1. x86-64 guest block 能在 Android ARM64 app 内稳定 JIT 执行；
2. guest pointer 能按照 shadPS4 预期访问映射内存；
3. guest `call` 能经 callgate 调用 native ARM64 HLE 并正确返回；
4. native HLE 能再次调用一个 guest x86-64 callback。

四项都通过后，FEXCore 路线才算真正解除 CPU 架构阻塞。若只完成第一项，仍不能据此判断 shadPS4 Android 移植可行。

---

## 附录 A：2026-09-04 复核记录

对照 shadPS4 `e1cf475263c8`（即本文快照，亦为当前 HEAD）与 Citron `c5e47458558b` 逐条验证第 2、3 节的可检验断言。

| 断言 | 出处 | 结果 | 复核依据 |
|---|---|---|---|
| ARM64 分支进入 `UNREACHABLE` | 3.1 | 成立 | [`src/core/linker.cpp:62`](../src/core/linker.cpp#L62) `UNREACHABLE_MSG("RunMainEntry unimplemented for current architecture.")` |
| `cpu_patches.cpp` 仅在 x86_64 编译 | 3.1 | 成立 | [`CMakeLists.txt:949-954`](../CMakeLists.txt#L949) `if (ARCHITECTURE STREQUAL "x86_64")` |
| 使用 Zydis 解码 + Xbyak 生成补丁 | 3.1 | 成立 | [`src/core/cpu_patches.cpp:9-11`](../src/core/cpu_patches.cpp#L9) 同时 include `Zydis/Zydis.h` 与 `xbyak/xbyak.h` |
| `LIB_FUNCTION` 经 `HOST_CALL` 存 host 函数地址 | 3.2 | 成立 | [`src/core/libraries/libs.h:18`](../src/core/libraries/libs.h#L18) `reinterpret_cast<u64>(HOST_CALL(function))` → `sym->AddSymbol(sr, func)`；`HOST_CALL` 定义见 [`src/core/tls.h:58`](../src/core/tls.h#L58) |
| Citron 在 `ArmNce` 与 `ArmDynarmic64` 间选择 | 3.1 | 成立 | `citron/src/core/hle/kernel/k_process.cpp:1294`（`ArmNce`）与 `:1300`（`ArmDynarmic64`），且 `ArmNce` 位于 `#ifdef` 内，与本文描述一致 |
| 引用的三个 Citron 文件路径存在 | 3.1 | 成立 | `k_process.cpp`、`arm_dynarmic_64.cpp`、`arm_nce.cpp` 均存在 |
| 同相对路径文件约 77 个 | 2 | 成立 | `comm -12` 对两侧 `src/**/*.{cpp,h}` 求交得 **77**，与原文一致 |

两处需要留意，但都不影响结论：

1. Citron 本地分支已从 `feature/tencentmalos/basic_version` 切到 `feature/malos/xr_support`，正文已注明；引用的 commit `c5e47458558b`（"Migrate submodules to tencentmalos mirrors"，2026-08-02）本身仍可达，所有行号引用有效。
2. 第 2 节「按行数加权文本相似度约 26%」这一项本次未重算——原文已声明该数字只用于判断代码亲缘关系、不等同于移植工作量，且不承载任何结论，故不重复验证。

### VR/PSVR 现状补记

本文未涉及 VR，但同期核查了一次，结论对 Android 方向有影响，记录在此：shadPS4 **没有 VR 支持**，只有让 VR 游戏不崩溃的 HLE 桩。

- [`src/core/libraries/hmd/`](../src/core/libraries/hmd/) 与 [`src/core/libraries/vr_tracker/`](../src/core/libraries/vr_tracker/) 共约 1959 行、229 个导出函数，其中 198 个（约 86%）为纯桩。
- [`hmd.cpp:26`](../src/core/libraries/hmd/hmd.cpp#L26) 写死 `"PSVR headsets are not supported yet"`；[`hmd.cpp:121`](../src/core/libraries/hmd/hmd.cpp#L121) 的 `sceHmdGetDeviceInformation` 恒返回 `ORBIS_HMD_DEVICE_STATUS_NOT_DETECTED`。
- 全仓 `openxr|OpenVR` 零命中：无头显运行时对接、无位姿追踪、无双眼渲染路径、无 reprojection、无畸变网格。
- `param.sfo` 的 `require_ps_vr` bit（[`elf_info.h:53`](../src/common/elf_info.h#L53)）只在 [`emulator.cpp:509`](../src/emulator.cpp#L509) 打日志，无任何逻辑消费。
- VR 相关提交共 7 次，集中在 2025-02 至 2025-08，标题均含 "stubs"，之后无实质推进。

这些桩的价值在于逆向出了准确的 ABI 边界和错误码语义（`hmd.cpp` 有五处注释复刻了真机固件的错误码 bug），渲染与追踪需从零实现。与 Android 方向的交集见附录 B 末尾。

## 附录 B：已有第三方 Android 实现（Bachata S4）

2026-09-04 调研发现 shadPS4 已有一个活跃的第三方 Android 移植，已作为子模块检出至 [`references/Bachata-S4`](../references/Bachata-S4)。

### 候选甄别

叫「shadPS4 Android」的仓库很多，但只有一个有真实代码：

| 仓库 | 判断 |
|---|---|
| **The412Banner/Bachata-S4** | shadPS4 真实 fork，领先上游 241 commit，完整 `android/` + `runtime/`。**已检出** |
| Tersonous/shandroidPS4 | 141 stars 但为空壳：`main` 领先上游 0 commit；Android 改动仅在 `SibroPS4` 分支且只有 4 个文件（全是文档）。停更于 2025-03 |
| JICA98/Bachata-S4 | 仅 release 元数据与兼容性报告，无源码（上游作者的发布仓库） |
| kardeiro/shadPS4-Android | 自述 UI-only 原型，199 KB |
| dev-Ali2008/onRps4-Android | 仅 README 与截图，无代码 |

注意上游作者是 JICA98，但其同名仓库不放代码；`The412Banner` 的 fork 是目前唯一能拿到完整实现的入口。

### 对本文选型的影响

**一、FEXCore 阶段 0/1 已有实机证据。** `runtime/evidence/sm8650/` 下两份 JSON 对应本文第 5 节的阶段 0 与阶段 1，FEX 固定在 `f2b679f6`：

| 证据 | marker | 通过项 |
|---|---|---|
| `fex-phase0.json` | `FEXCORE_SMOKE_OK` | `gpr` `stack` `fp` `threads` `tls` `callback` `invalidation` |
| `fex-phase1.json` | `FEXCORE_GUEST_ENGINE_OK` | `gpr` `rflags` `xmm` `bridge` `threads` `tls` `invalidation` `teardown` |

设备为 SM8650 / arm64-v8a / SDK 36 / **pageSize 4096**。这批 contract 恰好覆盖本文 4.3 节的多个接入要点（每线程 CPU state、reverse callback、JIT cache 失效、TLS 隔离）。`runtime/tests/` 下有十余个 `fex-*.test.mjs` 及 `bloodborne-fex-validation-cache-source.test.mjs`，可作复现参照。

需要注意证据的边界：pageSize 为 4096，**本文 3.3 节列为最低兼容性检查项的 16 KiB page-size 设备并未被这两份证据覆盖**，该风险项仍然开放。

**二、Box64 与 FEX 是并行路线，不是二选一。** `runtime/locks/components.lock.json` 同时锁定两者（Box64 `50c8b90b`、FEX `f2b679f6`）。本文 4.1 节把 FEXCore 列为首选、把 Box64 判为「没有可嵌入 CPU-library API」，从选型逻辑看仍然成立，但实践中该项目是两条都做——`runtime/settings/android-setting-metadata.json` 里有完整的 `BOX64_*` 环境变量配置段（`BOX64_ARGS`、`BOX64_INPROCESSGPU`、`BOX64_NOSANDBOX` 等），说明 Box64 是当前实际承载运行的路径，FEX 更接近在验证中的第二后端。

**三、架构选择与本文 4.2 节不同。** 它没有做进程内 `GuestCpuBackend` 抽象，而是 **Winlator 式容器化运行时**：Box64 + glibc + Mesa/Turnip + Vortek Vulkan 转发 + 内嵌 X11 server（见 `NOTICE.android-runtime.md`）。取舍是：

- **绕开了**本文风险表中列为「极高」的 guest VA 同址映射问题——因为 guest 跑在近乎真实的 Linux 用户态里；
- **代价是**要背整个 Linux 用户态运行时的分发、许可与体积；`NOTICE.android-runtime.md` 里 LGPL 组件（Winlator、Vortek）的源码提供义务是实际约束。

这与本文第 1 节「不启动完整 x86 Linux 用户空间」的前提直接冲突。所以它不是本文路线的实现，而是一条**被本文范围排除掉的替代路线**，且这条路线已经能跑 Bloodborne。这一点值得在决策时重新权衡：本文当初排除容器化方案的理由是否仍然充分。

**四、与 VR 方向的交集。** 这套容器化运行时是纯 2D 输出路径（X11 + Vortek Vulkan 转发）。若日后要往 VR 走，多出的这层间接会成为额外阻碍——VR 需要的低延迟与 per-eye 同步在转发层上更难保证。结合附录 A 的 VR 现状（渲染与追踪需从零做），Android + VR 双方向叠加的成本远高于任一单项。

