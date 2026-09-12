# Android native host v1：执行 spec

日期：2026-09-11。状态：**PROPOSED / NOT_IMPLEMENTED**。这是可交给执行 AI 的新任务书；下述新增验收均为 NOT_RUN，不用历史 CPU PASS 填充。

事实依据：[当前 host 原生化评估](../android-native-host-assessment-2026-09-11.md)及[独立证据](../validation/android-native-host/2026-09-11/README.md)。源码基点 `9ac6c300ef82074014dd03046dd29134ab278075`，分支 `codex/android-fex-round2`；2026-09-11 实查 origin 为 `0e10defc`，本地领先 36 提交。执行时先记录新的 HEAD、dirty patch、submodule 状态，发现漂移时只补增量，不重做已验证修复。

## 0. 目标、继承契约和第一批交付

实现可嵌入普通 Android APK 的 ARM64/bionic shadPS4 host：主仓 loader、内存、Orbis HLE、线程和渲染/音频/输入在 host 原生运行；**FEXCore 仅执行 PS4 x86-64 guest**。沿用现有 Kotlin UI 和 `guest_cpu_fex`，移植 reference 的接口思想，避免引入第二套 guest backend。

主目标为 **Swan / Android 16 / API 36 / ARM64 / 4 KiB**；AYN Thor / API 33 / 4 KiB 为辅助验证设备。16 KiB runtime、有限 Step、PSVR/Move、Beat Saber、广泛游戏兼容不在本 spec 的核心完成条件中。现有 ELF alignment 要求保留；PS4 ABI 的 16 KiB 粒度不得改成 host 4 KiB。

[V0](android-fex-v0.md)、[Round 2](android-fex-round2.md)、[完整 G3/H3](android-fex-round2-g3-repair-h3.md) 和 [E0–E3/A0–A1](android-fex-round2-n4-to-apk.md) 的未完成要求继续有效。此 spec 增加 host 集成阶段，**不会将 Vulkan/game 倒算进历史 Round 2，也不会用新 APK 存在来关闭 G3/G4**。

**第一批执行范围：HN0 → HN1 → HN2 最小纵向闭环。** 交付一份能从最终 `exported=false` APK 的明确入口启动、使用主仓真实 loader 装入自有 ELF fixture、通过已登记的 Orbis NID 调用真实 host 函数并返回结果的产物。HN4 的纯 WSI 可在接口确定后独立推进；大规模 HLE、游戏、UI 扩展留到这一批审核后。

执行前必须阅读 [AGENTS](../../AGENTS.md)、[子仓归属](../subrepository-ownership.md)、[事务加固](../validation/v0/transaction-hardening-2026-09-08.md) 和 [poison 记录](../validation/v0/followup-poison-2026-09-08.md)。本 spec 不授权忽略子仓自身贡献规则；FEX 保持源码审阅用途。Foundation 先使用现有能力，确需改 child 时按其自有分支和交付规则操作。

## 1. 架构与所有权合同

### 1.1 进程、线程和资源

默认同进程 JNI。Kotlin 负责 UI、内容选择、权限和生命周期信号；native `HostRuntime` 负责执行。暂不增加 X server、glibc rootfs、FEXLoader、shell daemon、Vortek 或新的网络控制协议。

建议新增目录为 `src/core/host_runtime/`、`src/platform/android/`、`cmake/host/`、`tests/host_runtime/`；这些是建议文件边界，当前未存在的路径不是现成实现。

| 资源 | 唯一负责者 | 约束 |
|---|---|---|
| FEX allocator、进程诊断基础设施 | HostRuntime | 一次成功初始化，保留合法进程寿命；不可每场 ClearHooks/重装 |
| Session 状态、启动/停止/销毁命令 | 串行 session controller | 唯一执行 join/替换 owner 的位置；不持有 owner 收尾所需锁等待 |
| 模块、挂载、guest 内存、线程表、HLE registry | EmulationSession | 有 session id/generation；终态后资源可回收，不能依赖 quick_exit |
| guest ThreadHandle / Run | 对应 native owner | TLS/FS/GS 和调用栈归属明确；跨线程只发请求/等 receipt |
| mmap/backing/逻辑权限/pin/publication | 统一 guest 内存层 | Orbis MemoryManager 保留政策，不能另行修改同一地址的 OS 映射 |
| ANativeWindow / VkSurface / swapchain | render controller | 有 surface generation；回调只投递消息，渲染线程执行资源操作 |
| PCM queue / AudioOut port | native audio backend | 音频 callback 不进入 guest、不访问 JNI、不做无界等待 |

不能 reset 的 singleton 必须列出并完成迁移，或明确限制该能力尚未验收；不能把正常 Stop 实现为终止进程。`:emulation` 独立进程是后续可选架构决策，需要独立 Binder/生命周期设计，不是 allocator 问题的默认解法。

### 1.2 public session 边界

在现有 API 之上增加小型 host 会话接口，不更换 `CpuContext` 契约。名称可调整，语义必须保留：

```text
LaunchDescriptor { content_kind, install_root_or_owned_fd, entry_relative_path,
                   settings_snapshot, requested_capabilities }
SessionKey       { id, generation }
Start(descriptor) -> accepted key OR structured error
RequestStop(key, reason) -> request/epoch receipt OR structured error
Query(key) -> immutable snapshot { phase, sequence, stage, stop_reason, error }
WaitTerminal(key, deadline) -> terminal snapshot OR timeout (session still owned)
AttachSurface/DetachSurface(key, surface_generation, lease)
SubmitController(key, sequence, snapshot)
```

Start accepted 不等于 Running。状态为 `Idle → Preparing → Ready → Running → Stopping → Stopped/Failed`，允许 Preparing 阶段取消或失败，允许快速执行在 UI 读取前到达终态；以保序事件补全历史，不能伪造 Running。每个 generation 恰好一个终态；旧 key/旧 surface/旧 watcher 不能控制新会话。

JNI 只传稳定 POD、复制字符串/数组、受控 fd/Surface 引用或不透明 id，禁止暴露 native/guest 指针供 UI 使用。所有 JNI 出口截获 C++ 异常并转换为结构化错误；不要让异常越过 JNI。`JNIEnv*` 仅用于所属线程；持久 Java 引用用正确的 global ref 生命周期。[Android JNI 约束](https://developer.android.com/ndk/guides/jni-tips)。

错误至少带 `session/generation/context/thread/invocation/operation/category` 中可获得的字段；不可归属时执行已定义的 context-failure 政策，不能默认记到当前健康 owner。字段缺失应明确，不能填入猜测值。

## 2. HN0：修复可信执行和嵌入基础

### HN0.1 Session/JNI/Service

修改当前 `fex_session_jni.cpp`、`NativeFexSession`、`FexSessionService.kt` 与 `ManagedSession` 的契约：

1. 去掉锁外无 lifetime lease 的裸 context 引用。销毁先阻止新请求，等待已进入的控制调用结束，再销毁 thread/context/space。持有 shared_ptr 并不足以自动保护 `DestroyThread`，需覆盖 thread 生命周期；不能持锁跨阻塞等待形成死锁。
2. 准备阶段 Stop 必须持久记录取消意图，context 创建前后都能取消。owner 发布 thread 时检查取消，不能丢失请求后进入长 guest Run。
3. controller 唯一负责 join；重复 Start/Stop/Shutdown 返回定义好的 AlreadyRunning/同一 receipt/已终止结果。创建 native thread 失败时回滚状态。
4. CPU stop receipt 使用原有有界要求，默认 1 秒；WaitTerminal/完整资源退出也必须有配置 deadline。timeout 保留安全所有权并报告未退出，禁止后台仍活动时宣布 Idle 并启动新 session。不可用超时后 unbounded join 绕过预算。
5. Running 由 native 完成准备并开始执行后发出；`GuestFault/BackendFailure/Unsupported/timeout` 均映射为真实失败；不能按 enum ordinal + “非 Cancel 就 exit0”。
6. Service 使用有代次的串行协调和可取消观察任务，正确处理 `startId`/旧事件；`onDestroy` 投递 stop/cleanup。第一版定义前台运行策略，退后台主动停或进入明确的已验证暂停状态，不能留下无人负责的线程。进程被系统终止后下次启动不恢复虚假 Running。
7. 提供 debug 构建的显式“CPU 自检”入口，使用共享生成的 fixture，不往用户 Library 写伪游戏记录。不复制另一份手写 guest bytes 或 include 测试 main 进生产库。

反例测试必须用 handshake 控制在“Stop 取得访问权、owner 即将销毁”“context 发布前”“启动线程失败”“旧 watcher 延迟到达”的准确边界，不能靠 sleep 碰概率。短 fixture 做 100 轮启动/停止与不同交错；保持生产 wrapper，测试 gate/trace 不进入最终 tests=OFF 包。

### HN0.2 Allocator provider 可行性门槛

先新增诊断/设计记录 `docs/validation/android-native-host/allocator-provider.md`，再更改主仓适配层。必须回答：谁拥有 JIT/object allocator VA，何时取得，如何处理占用/失败，怎样初始化 small allocator，哪些部分为进程寿命，是否会挤占 guest/ART 未来分配。

现有 `WaitForClaimableAllocatorRegion` 是时间缓解，不能作为“已检测 ART 静默”的条件。不得通过加长 sleep 或增加稳定轮数关闭本项。

调查固定 FEX 的 `AllocatorHooks.h`、`AllocatorHooks.cpp`、`Allocator.cpp`、`64BitAllocator.cpp`、HostAllocator/私有 Allocator 接口：

- Linux public `SetupHooks(size_t)` 不接收预保留 regions；Windows `HookPtrs` 不适用于 Android。
- mmap/munmap hooks 与 `SetupAllocatorHooks`、`InitializeAllocator` 是不同层次；需要证明完整分配/释放链和 rpmalloc 初始化，不得只接一个 hook 就运行。
- 在主仓建立隔离的、固定 FEX 版本的 provider 适配可行性实验：原子 claim 的区域由最终 provider 持续持有，失败可返回；逐项满足 alignment、PROT、flags、地址限制、释放及并发要求。不在未拥有地址上 MAP_FIXED，不抢占/覆盖 ART 页。
- 若需要 FEX 内部头/符号，列出版本锁和 ABI 检查，不伪装为稳定 public API。若现有 pin 无法安全完成，输出确切缺口及最小接口提案，并将该 gate 保留 BLOCKED；仍可推进不依赖它的 host 源码拆分和单测。不能擅改 FEX 子仓绕过其规则。
- 初始化失败可安全 retry 或明确永久故障；设计依据是实际资源状态。不要用 call_once 的正常返回永久吞掉瞬时失败，也不能在半初始化全局状态上盲重试。

证据包含失败现场 maps、候选区间、claim errno、每步耗时、有效 VA bits、实际 SetupHooks 路径及 allocator 统计；说明故障根因是证实还是待证。验收为普通 app UID 下 100 次冷进程启动和同进程 100 次 context/session 重建，叠加 Java 分配/线程/Surface churn；已拥有区域不被释放后重新争抢，失败注入可控退出，无 abort/地址覆盖。重复成功是补充证据，所有权正确性仍须代码证明。

### HN0.3 构建、测试和 APK 身份

- 修复现有 JVM test 编译。迁移仍有效的 lifecycle 测试；只为确实删除的 X backend 归档/移除专属测试并说明替代覆盖，禁止关闭整个 sourceSet 或删所有失败用例。查明文件 API/Unsafe 问题对应的测试编译配置。
- 辅助 profile：app min33，全部自有 native 依赖从干净目录构建 API33；Swan profile：compile/target SDK36，可选经锁定的 native API35，minSdk 必须与依赖一致。不得把同一 `FEX_BUILD_DIR` 不加身份检查用于不同 API/profile。
- 固定 NDK/JDK/CMake、实际 sysroot metadata、编译器 target、FEX/Foundation pins、STL；记录每个 prebuilt 的 API 和哈希。最终多 so 使用兼容的 `c++_shared`。依据：[NDK prebuilt/API 规则](https://developer.android.com/ndk/guides/common-problems)。
- 加载实际 APK 中的全部 native ELF 检查 ABI/Build ID/DT_NEEDED/PT_LOAD alignment，记录 APK SHA 和安装 split；不能只检查自有两个 so。生产 app 使用 `V0_BUILD_TESTS=OFF` 并验证 gate/trace 排除。
- 对最终 manifest 为 `exported=false` 的同一个 APK 使用 app 内入口或 instrumentation 驱动 UI；不以临时 exported service APK 替代验收。语义点击和可观察事件证明触发路径，不把 adb 坐标未命中解释为设备特殊输入机制。

## 3. HN1：将 desktop core 拆成可嵌入 host

### HN1.1 Target 和依赖闭包

从根 `CMakeLists.txt` 归纳源文件/依赖，形成可复用 target：common/platform、loader-memory、kernel-min、host-runtime，renderer/audio 后续独立加入。名称自行适配项目风格。desktop executable 只组合 host target 和 desktop frontend；Android JNI target 只组合 host target 和 Android adapter。

不要复制一份巨大 `CORE` 列表进 Gradle，也不要把 `main.cpp`/Emulator::Run 整体包进 so。保留 desktop 实际调用路径的构建验证；针对被触及的核心用例回归，不要求无关平台全量重建。

新增 `docs/validation/android-native-host/dependency-closure.md`，每项注明来源/pin、target、NDK 状态、Bionic 缺口、第一批是否需要：

| 类别 | 第一批处理 |
|---|---|
| fmt/spdlog/toml/必要 Boost 与 ELF 辅助 | 按实际 loader 最小闭包原生构建 |
| FEX/guest_cpu | 沿用 cmake/fex，固定 provider/API 身份 |
| Foundation | 调用现有 CMake 集成，先 DebugBus/Android diagnostics；关闭未审计网络 |
| Zydis/xbyak | 逐调用审计；guest 解码可保留，x86 stub 只能作为 guest code，经 FEX 执行 |
| SDL/ImGui/desktop window | 从 headless host 解耦，desktop adapter 保留 |
| Vulkan/glslang/sirit/VMA/必要 shader 依赖 | HN4/HN5 引入真正所需闭包 |
| FFmpeg/OpenAL/USB/Discord/updater/网络及其余库 | 根据能力矩阵后置；必需符号缺口应返回 Unsupported 或明确阻塞，不能用空 target 蒙混链接 |

不得全局定义/取消宏来掩盖 `UNREACHABLE`、ARM asm 或 ABI 未实现项。对 platform 特定 API 建薄适配，guest 语义仍归主仓。

### HN1.2 HostRuntime/Session 提取

从 `emulator.cpp` 提取配置快照、目录挂载、模块/HLE 初始化、执行和收尾。JNI 不调用 `main`/`quick_exit`；内部失败向上传 Status。列出 process/session/thread 三种静态资源，清理顺序必须覆盖 callbacks、jobs、mounts、handles、TLS、GPU/audio 引用。

实现无 Surface 的 headless profile。NullPresenter 仅表示明确无图形能力，收到不支持的 VideoOut/GNM 操作要有可归属的 Unsupported 结果，不能假返回成功。Foundation 诊断读取复制 snapshot，不直接跨线程访问运行中的 FEX CPUState；异步 JIT stop 和已 spill 安全点仍按既有调试模型区分。

交付：干净 NDK headless host target + JNI smoke 可链接/加载，desktop 受影响路径仍能构建；不启动真实游戏也应完成 session 创建、失败回滚、销毁和重建单测。

## 4. HN2：统一 VM，接入真实 loader 和最小 Orbis HLE

### HN2.1 Guest VA 和 backing 方案

先提交 `docs/validation/android-native-host/guest-va-layout.md`，包括实际 host VA bits 和 maps、guest module/stack/TLS/heap/veneer/return gate、FEX JIT/allocator、ART 区域的 ownership 表。明确哪些地址来自 PS4 ABI、哪些可重定位、哪些只用于 fixture。

首个 headless fixture 可使用现有小 reservation 与可重定位 ET_DYN，不能宣称它已支持任意游戏地址。ET_EXEC 必须满足固定地址，否则加载前结构化拒绝，禁止悄悄 relocate 不可重定位内容。

随后按实际需求扩展 GuestAddressSpace 为受控多窗口/固定 placement、file/backing 和 alias。既有类是具体所有者，不是现成的“外部 VMM adopt”接口。Orbis MemoryManager 作为同一层的 policy adapter，禁止两层分别 mmap 后同步元数据。若暂未实现高于 64 GiB，显式报告能力限制；若扩展策略上限，回归所有高位 guest 地址、tag、decoder/cache、pin 和溢出检查，不能只改常量。

强制要求：

1. OS reservation 原子取得，MAP_FIXED 仅替换本 session 已拥有的页；地址占用返回错误，不覆盖 host 映射。
2. 文件段复制/映射、BSS、权限、relocations、RELRO、guard page 按 checked range 执行；写代码先 RW、发布后 RX，保留现有事务契约。
3. 运行中修改 code/backing 继续 stop/drain → token → pre-mutation shared/local/decoder retirement → mutation → commit；失败 poison、pin/sink 排他和重试语义完整保留。
4. alias 必须有同一 OS backing 的实际一致性，`RegisterAlias` 元数据不是实现。file-backed map 的 fd/offset/长度/取消生命周期清晰。
5. guest HLE 发起内存变更时不可等待自己的 Run lease。由定义好的 HLE 离开/协调边界排队完成并安全返回；第一批可在启动前完成映射，运行中未支持的操作显式拒绝，HN3 再关闭该缺口。
6. guest R/W/X、host protection、GPU tracking protection 分开；禁止把 GPU 脏页保护失败解释为 guest 没权限，或为了 GPU 取消 CPU executable-write retirement。

### HN2.2 Loader 的第一条真实执行链

接入 `src/core/loader/`、`module.cpp`、`linker.cpp` 的必要实际实现，建立从 `LaunchDescriptor` 到模块对象/符号/入口的通路，不另写玩具 loader 冒充主仓 loader。

第一批自有 fixture 必须有可重现生成脚本和许可，带真实 ELF headers/segments/entry、最小 relocation/import 信息和校验逻辑。使用主仓明确支持的 ELF/可处理 SELF 格式；未支持的 container/加密/relocation 清晰拒绝。不要提交商业游戏/固件二进制。

最小验收流程：

1. app 明确选择自检 ELF 或真实安装内容；native 打开实际路径/fd，记录 size/hash，验证架构/segments/范围。
2. loader 建立模块并填充 guest memory，初始化 guest argv/env/入口参数/TLS 所需最小布局；任何 guest 可见地址均在受控 VM 中。
3. 解析最少一个真实 Orbis NID。建议优先 `sceKernelGetProcessTime`（当前 `time.cpp` 的 `4J2sUJmuHZQ`），按实际依赖拆出原实现；其他等价的最小无指针 HLE 也可，必须记录替代理由和实际定义。
4. resolver 给 guest 的是可执行 x86 veneer 地址，veneer 经正式 typed gate 调用真实 ARM64 HLE。不得放 host 函数地址，不能仅用 fixture 专用加法函数宣称 Orbis HLE 已接入。
5. guest 调用后验证返回语义并产生确定的 checksum/状态；以正式 return gate 返回。记录装载 entry RIP、模块区间、NID→veneer、host entry count 和结果，证明执行来自 ELF 而非 JNI 内置循环。
6. 在同一个 final APK 做 10 次启动/正常返回，并做入口不存在、坏 ELF、未知 NID、guest fault、准备阶段取消。失败不得自动退回 smoke。

本阶段不依赖完整 InvokeGuest：允许只有一层 crossing、无 guest callback 的明确 allowlist。引入 callback 的接口必须保持 Unsupported，直到 HN3 对应 H3 合同通过。

## 5. HN3：Orbis ABI、线程、TLS 和 callbacks

### HN3.1 typed registry 与迁移清单

从当前 `LIB_FUNCTION` / `LIB_OBJ` 注册生成可审阅清单，记录 library/module/version/NID、C++ 定义、参数/返回 ABI、buffer 方向/长度、guest 对象、callback、当前能力、测试 ID。5,277 是源码注册调用数，不作“已支持 API 数”。按 fixture 和后续一个非 VR 游戏的实际 imports 增量实现。

注册表区分 guest module export、HLE veneer、guest object export；stable gate id 不能等于任意 host 地址。记录 guest gate 来源与目标 descriptor，分开处理正常 HLE gate、直接 guest syscall 和未知 syscall；非法 gate/伪造地址在 native entry 前拒绝。

保留既有 typed pin 层，补全 E1/E2/N4：

- x86 SysV 的寄存器/栈参数、8 个整数和 9 个浮点跨寄存器边界、mixed 参数、返回寄存器 mask、RCX/R10 规范化只在 decode view、guest flags/XMM 保全。
- host/guest fenv、FPCR/FPSR、MXCSR、errno 和 guest errno/TLS 各归所属；不能仅设置舍入并清 FPSR 就宣称全 FP 状态保存。所有成功/失败/异常出口对称。
- buffer 零长、溢出、越界、读写方向、部分 pin 成功后失败清理、含 output 的异常；native call count 证明非法输入未进入 host。
- aggregate/sret/varargs/函数指针和 `va_ctx` 各自专门适配。无法支持的签名注册时拒绝，不能把 host `va_list` 或 native 对象布局给 x86 guest。
- 两 owner **均有真实 HLE 活动**的重叠、错误归属、Pause/Cancel/Shutdown 交错及旧 generation 矩阵；G36 的无 syscall 健康循环不能替代。

### HN3.2 所有 guest entry 的统一路径

建立 callsite audit，至少覆盖 linker 主入口、Module init/fini、pthread start、TLS destructors、heap callbacks、atexit、媒体 callback、fiber/context。所有 guest 函数指针通过统一 Run/InvokeGuest 入口；ARM `_runOnAnotherStack` 的 `blr` 仅是 host 调用，不能用于执行 guest。

完成既有 H3 的 HleScope、两层 InvokeGuest、可取消 WaitingHle、独立 guest 栈/TLS、完整寄存器/FP恢复和超时交错。不得私自调用 FEX 深层 dispatcher 绕过当前 owner/lease/stop 协议。不可用 sleep、SleepThread parking 或 host SIGILL 恢复替代确定边界。

guest pthread 使用 owner + ThreadHandle，guest pthread/sem/mutex/errno/TCB 采用 Orbis 布局或 opaque handles，不暴露 bionic pthread 数据。native TLS 不能代替 FEX FS/GS；线程重用和新 session 不能保留旧 TCB 的 `thread_local once_flag` 初始化结果。

运行中 guest mmap/protect/remap 通过 HN2 的协调事务落地，避免 HLE 持有自己的 lease 时 drain 自己。Guest join/condition wait 需进入可取消 WaitingHle，不能在持有全局锁时阻塞 native。资源退出要能收回所有 callback、pins 和等待者。

关闭条件以原 G3/H3 完整矩阵为准，同时新 host fixture 验证真实 guest pthread → TLS → HLE → join，以及 module init/fini；unsupported fiber/其他接口留在能力清单，不一并宣称 kernel 完成。

## 6. HN4：Android 原生 WSI

将 `WindowSDL` 耦合缩到 platform window/surface adapter；desktop 保留 SDL，Android 使用 Surface/ANativeWindow。渲染器不直接持有 Activity 或跨线程 `JNIEnv*`。

以有代次的 Surface lease 实现 acquire/release、尺寸改变、销毁和重建；新窗口引用与旧窗口退休按 render queue 串行处理。`SurfaceLost` 要能够重建 VkSurface，不能只在失效旧 surface 上重建 swapchain。相关引用语义依据 [Native Window](https://developer.android.com/ndk/reference/group/a-native-window)。

检查现有 Vulkan 1.3、扩展、feature bits、queue/present、formats 和 memory types，记录设备/driver UUID/版本。首版系统 Vulkan loader；设置界面若尚未真正加载自定义 driver，明确禁用该选项。缺少必要能力在 Run 前返回 UnsupportedGpu，禁止继续 assert 或假显示已选择驱动。

acquire/fence/queue 等待必须支持有界取消；Stop、Surface detach 和后台切换不能被无限 `acquireNextImageKHR` 挡住。CPU receipt 与 GPU 清理阶段分别计时，全部使用有界合同；失败时保留资源至安全状态，不能先释放 ANativeWindow 再让 GPU 继续访问。

验收：真实 native WSI 清屏/三角形 300 帧，20 次 Surface 断开/重建/旋转，包含 acquire 中 Stop，无旧 generation 呈现、无悬空引用。此结果只记作 **WSI_PASS**，不记作 guest renderer/game PASS。

## 7. HN5：接现有 guest renderer、input 和 audio

### HN5.1 真实 VideoOut/PM4/shader

沿用主仓 `video_core`、`shader_recompiler` 和 Vulkan 后端，接 HN2 guest 内存访问/alias/dirty observer。初期允许正确的保守复制策略，但须注明数据路径，不假称零拷贝。异步 GPU 任务持有必要 backing/pin，GPU completion 后释放；CPU 改写、双映射、重建/停止时不能读已释放页。

先自有 guest frame buffer + VideoOut flip，再自有 guest GNM/PM4 triangle，记录提交 packet、shader/资源、flip/present 和图像结果。与相同 fixture 的 desktop 输出比较允许范围；不能只从 host 生成 triangle 绕过 guest command processor。单纯 screenshot 不能证明输入来自 guest，须匹配该帧的 guest marker/sequence。

### HN5.2 input/audio 的实际消费

连接 ManagedSession 的 controller sink 到原生 controller/pad；有序有界队列或 snapshot，带 generation，处理断连、deadzone、按键释放。guest 调用真实 pad HLE，100 个测试输入在 guest 输出 marker/画面体现，旧 session 输入被拒绝。

复用 `AudioOutBackend`/`PortBackend` 添加 Android AAudio 实现；仅当现有依赖/设备需求证明更合适时使用 Oboe，并锁定来源。格式/采样率/声道转换和端口生命周期明确。callback 不分配、不持重锁、不等 guest，不跨 JNI；断连/重开在控制线程处理。[AAudio 指南](https://developer.android.com/ndk/guides/audio/aaudio/aaudio)。

host tone 仅验平台音频；关闭本项须 guest 经真实 AudioOut HLE 提交已知 PCM，验证输出/帧数、欠载统计、10 次 stop/restart 和无残留 callback。暂不实现 PSVR 音频/追踪。

## 8. HN6：真实内容路径和 Swan 普通 APK 验收

### HN6.1 内容导入与 UI

现有 zlib 后端可继续使用。只有 profiling 或格式支持证据要求时才引入 libdeflate；无需为“编出 pkg 库”新增依赖。

用可重现的自有 package 或合法已有测试材料验证 PKG 正例及损坏、解压长度、截断、越界路径、取消/回滚、存储不足。解压输出完整性检查要基于 PFSC 实际 padding/块格式，不任意把整块补零或多余输入当成功。Room 只在安装事务成功后提交，失败清理临时目录；路径/内容 fd 按实际授权生命周期持有。

没有合适 PKG 时，将 PKG 正例标 NOT_RUN，继续自有 ELF/安装目录的 host 验收。不得手工改 Room DB 补出 PASS。Library 的真实 LaunchDescriptor 必须被 native 消费：对不存在入口报错，不能仍跑固定循环。

在最终普通 APK 的 UI/instrumentation 路径记录：`content_opened → module_mapped → relocated → hle_ready → first_guest_entry → first_present / first_audio / input_ack → terminal`。headless 场景缺少显示/音频阶段应标明能力，不捏造事件。

### HN6.2 设备与应用身份

Swan 设备必须在线且被识别，保存 API/page size/ABI/uid/process/SELinux domain/build fingerprint 和 APK/库身份。unauthorized 设备不计验收；AYN 数据单列 auxiliary。

应用保持普通 UID、service exported=false、无 root/runtime 容器依赖。执行实际触及的原 Round 2 24 项 app 验收及本 spec 各 case，缺项 NOT_RUN，不以 CLI 结果补 app PASS。release/tests=OFF 包进行导入→启动→停止→重启、后台/前台、Surface 变化和至少 10 分钟受控综合运行，记录 Java/native 内存、线程、fd、VA/backing、GPU/audio 待完成资源。

进程寿命 allocator 的预留需单独记账，不要求其每场回到零；session 资源必须回到已定义基线，不能只看总 RSS 猜测泄漏。故障不使下一场读取旧对象；同进程第二场确实执行新模块/新 marker。

### HN6.3 首个非 VR 游戏（独立兼容性结论）

完成自有 fixture 的 host 验收后，选择用户可提供、当前 desktop 基线可运行的一个非 VR 游戏，记录版本、imports、所需 shader/extension、启动失败点和性能。没有内容则记 NOT_RUN，不能拿 CPU fixture 代替。

游戏“进入 guest”“出现第一帧”“可交互”“持续运行”分别报告，逐项补齐真实所需 HLE，不为继续启动广泛填返回成功的 stub。Beat Saber/PSVR 和 Winlator/Vortek 路径不作为这阶段的替代目标。

## 9. 新增验收登记（20 项，初始全部 NOT_RUN）

这些是 host-native case families，每个 family 的子用例另有唯一 ID 和覆盖清单。不能用 checks 总数代替 family 完成；既有 V0/Round 2 case ID 不变。

| ID | Gate | 必须证明 |
|---|---|---|
| HN-B01 | HN0/1 | 干净 profile 构建、全 ELF/API/STL 身份，受影响 desktop 构建 |
| HN-B02 | HN0 | runner 23+ / JVM 测试完整执行；新增会话单测的实际方法数 |
| HN-A01 | HN0 | allocator provider 所有权、错误路径及冷启动/同进程各 100 次 |
| HN-S01 | HN0 | 100 轮确定交错：准备期取消、重复 Start/Stop、销毁中 Stop、旧 generation |
| HN-S02 | HN0 | Ready/Running/错误终态正确，JNI 异常/创建失败、timeout 不假 Idle |
| HN-U01 | HN0/6 | 最终非导出 service 的 app UI 启动/停止 10 次；无伪 DB |
| HN-M01 | HN2 | 受控固定/可重定位 VM、占用不覆盖、边界/溢出/异常 rollback |
| HN-M02 | HN2/3 | backing/alias 实际一致、G2 全回归、运行中 HLE 内存事务无自等待 |
| HN-L01 | HN2 | 主仓 loader 的真 ELF→真 Orbis HLE→return 10 次及身份链 |
| HN-L02 | HN2 | 坏 header/segment/relocation、未知 NID、缺文件、fault 无 smoke fallback |
| HN-H01 | HN3 | typed registry/对象/ABI/buffer/FP/errno/异常完整矩阵 |
| HN-H02 | HN3 | 两 owner 同时 HLE、归属/中断矩阵与全部既有 H3 callback gate |
| HN-T01 | HN3 | 真实 pthread/TLS/init/fini/join、重用后无旧 TCB、等待可取消 |
| HN-V01 | HN4 | 实际 GPU 能力/驱动身份、不支持时结构化拒绝 |
| HN-V02 | HN4 | 300 WSI 帧/20 次 Surface 生命周期/有界退出 |
| HN-G01 | HN5 | guest VideoOut + guest GNM/PM4/shader 产物及 desktop 对比 |
| HN-I01 | HN5 | 100 输入由 guest pad HLE 消费；旧 session 输入无效 |
| HN-O01 | HN5 | guest PCM→AudioOut→AAudio、10 次退出/重开、无 callback 残留 |
| HN-P01 | HN6 | PKG 正例、损坏/取消/存储失败及事务入库；缺材料 NOT_RUN |
| HN-Z01 | HN6 | Swan 普通 APK + 10 分钟综合运行 + 原 app gate 实际覆盖 |

第一批最低交付为 B01/B02/A01/S01/S02/U01/M01/L01/L02 的对应完整子项；M02/H 系列等继续登记缺项。A01 若受固定 FEX 能力阻塞，不得称“稳定 native host 基础已完成”；交付证据和缺口，再继续独立的 HN1工作。

## 10. 结果、代码与下一次审核的交付格式

每一 gate 使用 `docs/validation/android-native-host/<gate>-<date>/`：记录 source commit/dirty patch hash、所有 gitlink/prebuilt、toolchain/API、APK SHA/每库 Build ID、设备身份、命令/exit code、唯一 case/subcase、原始有时间戳日志、失败/跳过/缺项。result JSON 显式区分 `build / host-unit / CLI / app-auxiliary / app-Swan`；重复 case、缺尾部、崩溃、timeout 必须让 runner 非零退出，不得覆盖之前 FAIL。

另交付：dependency closure、guest VA ownership 表、HLE coverage 表、session API/状态机、仍含 raw guest-call 的 audit 清单、与原 spec 的未关闭项对照。目录链接全部相对；不要改历史 manifest 让旧结果看起来来自新源码。

新增可验证的小批提交，源码/测试/证据相互可定位；按要求交付 Git 时先检查所有子仓并保存自己的 child 修改，再推进 parent gitlink。保留无关 dirty 文档/研究，显式 stage，不提交 APK、游戏、运行时下载物、临时产物或凭据。

下一次审核重点不是“新增多少 Kotlin 页面/多少 HLE stub”，而是：allocator 是否有真实所有权、Session 是否能可靠退出、game path 是否真正进入 loader、guest 地址是否全部由 FEX/受控 VM 消费，以及一个真实 Orbis 调用是否完成端到端。
