# FEXCore 源码检出、规模统计与 Dynarmic 对比

日期：2026-09-07。目标延续 [Android 16 / 16 KiB 原生方案](fex-android16-native-guest-host-plan.md)：原生 ARM64 shadPS4，FEXCore 执行 PS4 guest，后续覆盖非 VR 游戏与 Beat Saber。

**结论：FEXCore 的 CPU 核心与 Dynarmic 是同一数量级，接入更难主要来自内存、线程、异常与 guest ABI 的边界，而不是源码大了一个数量级。可以借鉴 Dynarmic 的 API 组织，为 FEXCore 建一个薄适配层；不值得把 FEX 的 x86 前端搬进 Dynarmic。**

本次完成源码检出、固定提交的可复现统计，以及源码接口核查。没有编译 Android 产物，也没有测量运行时性能、APK 大小或编译后库大小。

## 1. 本仓现在有了什么

新增 Git 子模块：

- 路径：[references/FEX](https://github.com/FEX-Emu/FEX/tree/50e6eee95ae95d3257672727a9302a30b4a60a9a)
- 来源：官方 [FEX-Emu/FEX](https://github.com/FEX-Emu/FEX)
- 父仓 gitlink 固定提交：`50e6eee95ae95d3257672727a9302a30b4a60a9a`，2026-09-06。
- FEXCore 位于该仓的 `FEXCore/`，不是单独发行的另一个 Git 仓库；保留完整上游目录与历史，便于查调用链和比较版本。

已初始化源码依赖子模块，包括 fmt、xxHash、unordered_dense、rpmalloc、jemalloc、range-v3、cpp-optparse、VIXL、Zydis/zycore、Vulkan/DRM headers、Catch2、Tracy。三个 `fex-*-tests-bins` 是预编译测试资源，没有下载；不影响本次源码分析，后续运行相关测试再初始化。

FEX 源码保持未修改，以父仓 gitlink 固定；尚未接入 shadPS4 的 CMake 链接。

比较对象使用用户项目实际依赖，不另找一个不对应当前工程的 Dynarmic 版本。现已把这两个版本分别加入本仓 `references/dynarmic-citron` 与 `references/dynarmic-azahar`，计量脚本默认使用它们，原始 JSON 中仓外路径保留为首次测量记录：

| 项目 | 固定提交 | 本地位置 |
|---|---|---|
| FEX 当前检出 | `50e6eee95ae95d3257672727a9302a30b4a60a9a` | [references/FEX](https://github.com/FEX-Emu/FEX/tree/50e6eee95ae95d3257672727a9302a30b4a60a9a) |
| 之前 shadPS4 ARM64 参考锁定的 FEX | `f2b679f6028ce1c38875233aecfcf5d3f8ebecec` | 同一个 FEX Git 仓库的历史对象 |
| citron 使用的 Dynarmic | `a593d9262388e3216985b982e400f19ef9ce9749` | [citron/externals/dynarmic](https://github.com/tencentmalos/dynarmic/tree/a593d9262388e3216985b982e400f19ef9ce9749) |
| azahar 使用的 Dynarmic | `96a803e921ba19bde0fbb315264971fd268fb2ed` | [azahar/externals/dynarmic](https://github.com/tencentmalos/dynarmic/tree/96a803e921ba19bde0fbb315264971fd268fb2ed) |

## 2. 统计口径与核心结果

使用 [统计脚本](../scripts/analysis/compare_cpu_core_size.py)，结果保存在 [原始 JSON](data/fex-dynarmic-source-size-2026-09-07.json)。

口径：从 `git archive <精确提交>` 读取已跟踪 C/C++/头文件/汇编，以及 `.inc/.inl`。物理行包括注释和空行；“非空行”仅排除空白行，**不是去除注释后的 SLOC**。排除未跟踪构建产物、嵌套 gitlink、CMake/Python/JSON/DSL 和文档；已跟踪的生成代码仍计入。分组可重叠，不把全部行相加。

### 2.1 CPU 核心

| 范围 | 源文件数 | 物理行 | 非空行 |
|---|---:|---:|---:|
| FEXCore `Source/ + include/` | 184 | 73,038 | **61,652** |
| FEX 自带 `CodeEmitter/` | 11 | 20,097 | 18,251 |
| FEX `FEXHeaderUtils/` | 6 | 686 | 581 |
| FEXCore + 上述两项 | 201 | 93,821 | **80,484** |
| citron Dynarmic `src/dynarmic/` | 385 | 90,034 | **73,809** |
| azahar Dynarmic `src/dynarmic/` | 385 | 89,987 | **73,763** |

直观看，FEXCore 约 6.17 万非空行，加自身 emitter 和公共头工具约 8.05 万；两个 Dynarmic 版本均约 7.38 万。**无法从这个结果得出 FEXCore 比 Dynarmic 庞大很多。**

但上表还不是完全对称：Dynarmic 核心包含多个 host backend，却把 ARM64 指令编码库 oaknut 放在 externals；FEX 把自己的编码库放在项目内。进一步作目录范围比较：

| ARM64 host 相关源码范围，仍保留双方已有 guest 前端 | 非空行 |
|---|---:|
| FEXCore + CodeEmitter + FEXHeaderUtils | **80,484** |
| citron Dynarmic 去除 x64/riscv64 backend 目录 | 50,585 |
| citron oaknut | 18,296 |
| citron mcl | 3,221 |
| 后三项合计 | **72,102** |

这说明连编码器一起考虑，两者仍在约 7–8 万行的量级。这个“ARM64 相关范围”只是目录筛选，不是 CMake 构建闭包：Dynarmic 仍含 A32+A64 前端，mcl/oaknut 也未按实际引用剔除；FEX 的其他公共/数值依赖未加进来。**8.05/7.21 ≈ 1.12 不能当作维护成本、编译时间或性能倍率。**

### 2.2 FEX 整仓大在哪里

| FEX 分组 | 源文件数 | 非空行 | 对 PS4 Core 嵌入的意义 |
|---|---:|---:|---|
| CPU core + emitter/header utils | 201 | 80,484 | 主体复用范围 |
| LinuxEmulation | 152 | 28,131 | Linux guest syscall/signal/进程环境；不是 PS4 必带层 |
| ThunkLibs | 56 | 30,490 | Linux 库桥与相关设施；不等于 PS4 HLE 桥 |
| Source/Common | 27 | 3,800 | 部分配置/host feature 等可能复用，按依赖选择 |
| CommonTools | 8 | 2,011 | 不应因参考 build script 链了整库就全部照搬 |
| Windows | 70 | 7,792 | 非运行依赖；宿主契约可参考 [Windows 前端补充](fex-windows-frontend-reference.md) |
| `unittests/ + FEXCore/unittests/` | 2,598 | 133,976 | 包含大量逐指令汇编案例 |
| 第一方源码合计，含测试、排除 External | 3,143 | **294,411** | 不是需要移植的总代码量 |
| Dynarmic 第一方源码合计，含测试 | 421 | **108,841** | 其中测试 35,032 行，组织方式不同 |

FEX 第一方含测试约 29.44 万行，是 Dynarmic 的约 2.7 倍，但直接用整仓数判断 CPU 接入难度会严重失真。测试行数也不是语义覆盖率，汇编测试和参数化 C++ 测试不能按行数比较覆盖强弱。

第三方代码单列，不做整仓总量排名。citron Dynarmic 父 Git 中 vendored external 源码有 269,836 非空行，而 azahar 的许多依赖通过 gitlink 引入；FEX 也大量使用 gitlink。统计“目录里有多少行”会因依赖打包方式而改变，与 CPU 内核复杂度无关。

## 3. 核心内部规模与阅读路线

| 职责 | FEX 非空行 | Dynarmic/citron 非空行 | 解释 |
|---|---:|---:|---|
| guest 前端/指令语义 | 21,833 | A32 16,826；A64 10,865；整个 frontend 28,116 | 指令集不同，不能用数字判断哪套 ISA 更容易 |
| ARM64 JIT/backend 目录 | 11,282 | 10,636 | 接近；两者 emitter/通用设施另计 |
| dispatcher 目录 | 2,355 | 分散在 backend/address space/run code | 不作直接倍率比较 |
| IR 目录 | 4,787 | 7,065 | FEX 另有 JSON IR 定义和生成器，不在本行数口径中 |
| 公共 interface/include 目录 | 5,586 | 1,008 | FEX include 还含工具与内部状态，并非全是稳定 public API |
| utility/common 目录 | 5,471 | 3,083 | 职责范围不同 |

FEX 最大的源码块集中在 vector 指令语义、x86 opcode dispatcher、向量 lowering、memory ops，以及 ARM 编码表。应优先审计嵌入边界，再进入这些指令实现，不需要先把所有向量编码表读完。

建议按以下路径读：

1. [Core/Context.h](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/include/FEXCore/Core/Context.h) → [Context.cpp](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Context/Context.cpp)：对象、线程、运行与 state reconstruction。
2. [SyscallHandler.h](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/include/FEXCore/HLE/SyscallHandler.h) → [OpcodeDispatcher.cpp](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/OpcodeDispatcher.cpp#L38) → [JIT/BranchOps.cpp](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/JIT/BranchOps.cpp#L279)：HLE callgate 可以建立在什么控制流上。
3. [CoreState.h](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/include/FEXCore/Core/CoreState.h)、[SignalDelegator.h](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/include/FEXCore/Core/SignalDelegator.h)、[Dispatcher.cpp](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp)：寄存器归属、回调与异步状态。
4. [Allocator.cpp](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Utils/Allocator.cpp)、[SharedCodeBufferManager.cpp](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/SharedCodeBufferManager.cpp)、LookupCache/CodeCache/DiskCache：16 KiB、guard、缓存与代码生命周期。
5. [MemoryOps.cpp](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp)、[AtomicOps.cpp](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/JIT/AtomicOps.cpp)：TSO、原子、未对齐与 fault。
6. 最后按失败测试进入具体 opcode、x87、SIMD lowering，避免把平台问题误判成指令错误。

## 4. FEXCore 与 Dynarmic 真正不同的地方

| 项目 | FEXCore 当前代码 | Dynarmic 当前本地版本 | 对 shadPS4 的结果 |
|---|---|---|---|
| guest ISA | x86/x86-64 用户态 | ARM A32/A64 用户态 | Dynarmic 无法执行 PS4 x86 代码 |
| 主要执行后端 | ARM64 JIT | x64、ARM64，另有本地 riscv64 目录 | 本目标只比较 ARM64 host 路径 |
| 运行对象 | Context + InternalThreadState | A32/A64 Jit + 内部状态 | 对外应包装为自己的 CpuBackend |
| 内存 | 常规 fast path 同址直接访存，环境提供映射 | MemoryRead/Write callbacks、page table、fastmem 可选 | FEX 没有可以直接替换成 Dynarmic callbacks 的现成 MMU 插槽 |
| HLE trap | `HandleSyscall(CpuStateFrame*)`，环境处理语义 | `CallSVC`、`ExceptionRaised` | 两者都可做 HLE，FEX 不强制 guest Linux ABI |
| HLE 参数传递 | shadPS4 要从 x86 SysV 解码成 native 参数 | Switch/3DS 通常先按寄存器/SVC 协议封装 | PS4 普通 C ABI 导入是额外工作，不能由换 API 消除 |
| 重入 | 有专用 `HandleCallback` | `Jit::Run/Step` 明确禁止递归调用 | FEX 在 native→guest callback 方面反而有可复用机制，但仍需线程/ABI 适配 |
| 停止/步进 | 不直接提供同形状的 `Run/Step/HaltExecution` 产品接口 | 提供 `Run/Step/HaltExecution` 和 HaltReason | 不要以为包装一下名字就有等价暂停/单步语义 |
| TLS | guest FS/GS 与 state reconstruction | ARM TPIDR 等配置/寄存器状态 | guest TLS 与 bionic TLS 分离，由模拟器管理 |
| 内存顺序 | x86 强序在 ARM 上的保序与修复 | ARM barrier/exclusive monitor 等语义 | FEX 跨 native HLE 仍要审核同步协议 |
| signal | Core 提供协作接口；Linux frontend 另有完整策略 | fastmem 也会处理 host fault | 不是一个用 signal、另一个完全不用 |
| API 演进 | 本次已找到实质破坏性变化 | 清晰 API，但 README 也记录过不兼容变更 | 两者均应固定版本，FEX adapter 尤其要隔离私有类型 |

Dynarmic 的内存/配置契约见 [A64/config.h](https://github.com/tencentmalos/dynarmic/blob/a593d9262388e3216985b982e400f19ef9ce9749/src/dynarmic/interface/A64/config.h)，运行契约见 [A64/a64.h](https://github.com/tencentmalos/dynarmic/blob/a593d9262388e3216985b982e400f19ef9ce9749/src/dynarmic/interface/A64/a64.h#L24)。

两者都可以做同址 fastmem，区别在于 Dynarmic 还暴露了完整的按访问回调/page table 降级路径。不能把“Dynarmic 的分离更容易”理解成 guest/host 必须不同址。

## 5. 新版 FEX 已经与参考后端不兼容

本次检出的官方 HEAD 比先前锁定的 `f2b679f6` 多 389 个可达提交。仅 `FEXCore + CodeEmitter + FEXHeaderUtils` 的完整 Git diff 就涉及 **90 个文件，+5,274 / −1,341 行**；这是包含脚本/JSON/测试的 diff，不能与前文源码行数直接相减。

三项直接影响接入：

| 项目 | 旧版 `f2b679f6` | 本次 `50e6eee9` | 适配方式 |
|---|---|---|---|
| CreateThread | `CreateThread(rip, rsp, state)` | `CreateThread(state)` | 明确构造/填写初始 CPUState，不再用旧重载 |
| HandleSyscall | 返回 u64，带 SyscallArguments 参数 | `void HandleSyscall(CpuStateFrame*)` | 按 Frame 解码并回写 guest 结果 |
| OS_GENERIC | SyscallOSABI 枚举中的模式 | 枚举与该字段已移除 | 不再设置 OSABI；核查新的统一 spill/fill 路径 |

新版 [SyscallOp](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/OpcodeDispatcher.cpp#L38) 仍处理 RCX/R11 的 x86 syscall 语义；[BranchOps](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/JIT/BranchOps.cpp#L279) 使用完整 GPR/FPR spill/fill，返回状态由 frontend 写入。**移除 OS_GENERIC 不意味着不能嵌入，反而是接口统一了。**

新增 `InitDiskCache`、DiskCache/SharedCodeBufferManager 与 bitmap allocator，也扩大了页大小、缓存所有权和初始化顺序的审计范围。

因此采用两个明确基线：旧 commit 用于复现已有 shadPS4 harness；新 commit 用于后续独立 adapter 的设计与验证。它们都在这次完整 FEX Git 历史中，无需另复制一套源码。不要让参考 shadPS4 在未适配的情况下直接链接新 FEX，然后把编译失败判定成路线失败。

## 6. 16 KiB 页：源码层面的差异

### 6.1 FEX 仍有明确的 4 KiB host 假设

本次 HEAD 的 [TypeDefines.h](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/include/FEXCore/Utils/TypeDefines.h#L9) 仍定义 `FEX_PAGE_SIZE=4096`。在 `FEXCore` 的 `.h/.cpp/.inl` 中，`FEX_PAGE_SIZE/SHIFT/MASK` 共命中 20 个文件、96 行；这是定位候选的文本统计，不是需要修改的行数。

已核实两个真实 host-page 使用点：

- [GetHostVABits](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Utils/Allocator.cpp#L122) 在 `(1<<bits)-4096` 试映射，16 KiB host 上可能因地址不对齐失败。
- [CodeBuffer 构造](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/Interface/Core/SharedCodeBufferManager.cpp#L19) 将代码 buffer 最后 4 KiB 设为 guard，`UsableSize` 也减去 4 KiB。只把 mprotect 地址向下取整到 16 KiB，会保护更多实际仍被认为可用的代码空间；保护地址、保护长度、可用大小必须一同修正。

另外，当前 JIT buffer 默认使用 RWX，源代码也明确讨论 MDWE 限制；不能仅因 veneer 使用 RW→RX，就宣称整套 FEX JIT 已满足 W^X。目标 Android 是否允许所需分配必须实测，必要时设计双映射或发布/写入策略。

### 6.2 Dynarmic 有更容易降级的内存边界，但也不是自动正确

Dynarmic ARM64 的逻辑 page table 同样有 4 KiB 粒度，但它可以用 software table/callback 映射到任意 backing，不要求 host 为每个逻辑页独立 mprotect。对 16 KiB 设备，这种边界更灵活。

它的 host JIT memory 由 [oaknut/code_block.hpp](https://github.com/tencentmalos/dynarmic/blob/a593d9262388e3216985b982e400f19ef9ce9749/externals/oaknut/include/oaknut/code_block.hpp) 管理，常规 Linux/Android 路径是整块 mmap，并未在这里施加 FEX 那种尾部 4 KiB guard。这个差异能解释为什么两者的移植面积不同。

但该 oaknut 版本仍有值得修复的地方：mmap 返回值按 `nullptr` 检查而非 `MAP_FAILED`；普通 Linux/Android 分支使用 RWX，protect/unprotect 在该分支也没有实施 W^X 切换。这里只作源码发现，没有修改其他项目，也没有将其描述成已触发的真机问题。

**不能通过“Dynarmic 在我的 Android 项目能跑”推出 FEX 只需相同构建参数；也不能把所有 4096 常量都判定成错误。关键是它究竟用于 guest 查表，还是实际 host mapping/protection。**

## 7. 依赖与最小嵌入范围

FEX 当前 ARM64 指令发射器是项目内 `CodeEmitter`。在 [FEXCore CMake](https://github.com/FEX-Emu/FEX/blob/50e6eee95ae95d3257672727a9302a30b4a60a9a/FEXCore/Source/CMakeLists.txt#L111) 中，VIXL 只在启用 disassembler/simulator 时加入该链接列表；不是默认生产 JIT 必需的 emitter。Zydis 同样受可选配置控制。

| 部分 | FEXCore | Dynarmic ARM64 |
|---|---|---|
| ARM64 编码 | 自带 CodeEmitter | oaknut |
| 常规工具 | fmt、xxHash、unordered_dense、内部 utility 等 | fmt、Boost、mcl、robin-map 等 |
| 数值语义 | cephes、SoftFloat 等，按实际构建依赖审核 | common/fp 等，按所选前端审核 |
| allocator | rpmalloc/glibc hook 等受配置影响 | 标准分配与 oaknut code block 等 |
| 构建生成 | Python 生成 IR/config、项目 CMake 配置 | CMake 选择 A32/A64 与 host backend 等 |
| 完整系统层 | LinuxEmulation、Server、rootfs 等在 Core 外 | 使用方自行提供 OS/HLE |

FEX 顶层仍只接受 Linux/Windows 系统名，且有顶层外部库配置与工具生成。合理做法是新增受控 Android library 构建入口或针对性的 CMake 适配，保留 Core 生成过程和依赖；不是随手把 `FEXCore/Source/*.cpp` 全部 glob 到 NDK 工程里。

目标嵌入范围应围绕 FEXCore/FEXCore_Base、CodeEmitter、HeaderUtils、必要数值/容器/allocator 依赖，以及 adapter 实际使用的 host feature/config 代码。Linux guest loader、Linux syscall table、FEXServer、rootfs、Wine/Windows 集成不属于这条 PS4 主线。

本次把较完整的源码依赖检出，是为审计调用链提供材料，不代表它们都要进入 APK。

## 8. 是否应该把 FEX 改造成“x86 版 Dynarmic”

建议只在 **shadPS4 对外接口层**做相似抽象：

```text
shadPS4 GuestThread / GuestMemory / HLE
             |
       自有 CpuBackend 契约
             |
  FexAdapter：版本兼容、state、trap、callback、fault、invalidate
             |
        FEXCore + ARM64 JIT
```

可借鉴 Dynarmic 的：明确的执行退出原因、寄存器快照接口、单步/暂停契约、memory/callback 的职责描述，以及 CPU 核心与机型 HLE 的隔离。

不建议直接把 FEX x86 decoder/IR 接进 Dynarmic 的 ARM64 emitter。两者 IR、flags 延迟计算、x87/SIMD、内存顺序、异常精确性、寄存器分配、block linking 和 code cache 设计不同。单是 FEX guest 语义相关目录就有约 2.18 万行，而保留这些语义还会牵动它们依赖的其他层；接到另一个 JIT 不会只剩一个 decoder 转换器。

此外，这种重组依然没有消除 PS4 的 SysV ABI bridge、guest pthread/TLS、回调和 16 KiB 保护问题，却新增了跨两套 IR/后端的正确性负担。当前没有证据表明它比薄 adapter 更省工作。

## 9. 对后续实施计划的具体影响

1. **FEX 后端可从现在的源码开展工作。** 首先建立新版 API 下的最小执行/回调 harness；旧版本仅用于复现对照。
2. **16 KiB 审计集中在 host allocator/code buffer/guard/protection 边界。** 保留 guest/code 索引语义，不能全局替换常量。新版 SharedCodeBufferManager 是新增优先点。
3. **shadPS4 不依赖 FEX 私有状态类型。** 完整 guest state、线程身份和调用协议由自有接口表达，FEX 升级变化集中到 adapter。
4. **运行与测量分别交付。** 本文行数不是 APK/RSS/性能；后续需在相同工具链下测 `.text/.rodata`、JIT cache、桥接延迟、线程内存与 p95/p99 帧时间。
5. **CPU 结果不替代 PSVR 验收。** 即使 Core harness 与非 VR 游戏通过，Beat Saber 仍按上一份方案单独完成 HMD/Move/Tracker、双眼投影与实时预算。
6. **规模相近不代表调试接口同等成熟。** FEXCore 有 TF/INT3 基础，但 Linux stub 的断点、单步和寄存器协议存在具体缺口；Dynarmic 的 Step/Halt/状态 API 更直接。建议 [LLDB host/guest 视图](fex-lldb-host-guest-workflow.md) 先行，执行控制的差距与补齐门槛见 [guest debugger 专项](fex-guest-debugger-feasibility.md)。

## 10. 复现统计

在本仓根目录运行（路径不同可通过参数覆盖）：

```bash
python3 scripts/analysis/compare_cpu_core_size.py \
  --fex-revision 50e6eee95ae95d3257672727a9302a30b4a60a9a \
  --citron-revision a593d9262388e3216985b982e400f19ef9ce9749 \
  --azahar-revision 96a803e921ba19bde0fbb315264971fd268fb2ed \
  --output docs/data/fex-dynarmic-source-size-2026-09-07.json
```

脚本默认使用本仓 `references/dynarmic-citron` 和 `references/dynarmic-azahar` 两个固定版本，原工程的 Dynarmic 保持未修改。新机器按 [references 初始化说明](../references/README.md) 检出即可；也可通过参数提供其他 Git 仓库，但应保证所列 commit 可达。统计从 Git 对象读取，不受当前工作树未提交改动影响。

FEX 子模块由父仓 gitlink 固定，不使用 `submodule update --remote` 自动追新。需要构建某个历史基线时应在独立 checkout/worktree 中进行，避免不经意改变本仓记录的参考版本。
