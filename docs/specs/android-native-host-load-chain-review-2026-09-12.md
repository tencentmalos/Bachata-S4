# 现状整理:补齐 Android 正规游戏加载链路(供审核)— 2026-09-12

> 后续独立复核已发现“只差入口”“Mac不能NDK link”和部分验收推断不成立。本文保留为先前方案记录；当前执行以 [PKG v2整版spec](android-native-host-pkg-v2.md) 和 [最新复核](../validation/android-native-host/full-pkg-review-2026-09-12.md) 为准。

**目的**:把"用 TMNT 直接跑起来还缺什么"整理成可独立核对的现状 + 缺口 + 建议方案,供另一位 AI 审核后再决定是否推进。**本文档不改动任何生产代码**;所有坐标均已核对(见每条 file:line)。

分支 `codex/android-fex-round2`,HEAD `0f4fd74b`(HN4 已提交)。测试内容:`/Users/bytedance/game/ps4/TMNT.Splintered.Fate_CUSA50828_v1.08.pkg`(CUSA50828 v1.08)。

---

## 结论(一句话)

正规加载链路(`emulator.cpp::Run → linker->Execute`)在我们仓库**已经存在且完整**,但它在 **ARM64 上执行 guest 入口的最后一步是 `UNREACHABLE`**([linker.cpp:63](../../src/core/linker.cpp))。因此当前 app **绕过整条链**,在 [session_backend_fex.cpp](../../src/core/host_runtime/session_backend_fex.cpp) 里跑一段合成 x86-64 递减循环(CPU-alive 证据),与 TMNT 无关。要按 Bachata 正规链路跑真实游戏,需要按依赖顺序补齐 6 段(G-A~G-G),其中执行/HLE/VMM 核心只能在 Linux/CI 或真机 link/run 验证(本 Mac 只能 NDK 单 TU 语法检查)。

---

## 一、正规链路现状(已核对)

`emulator.cpp::Run`([emulator.cpp:272](../../src/emulator.cpp))是标准 desktop/Bachata 链路,和 desktop 完全一致:

| 阶段 | 位置 | 状态 |
|---|---|---|
| mount `/app0` (+`/hostapp`) | emulator.cpp:372-374 | ✅ 有 |
| 读 param.sfo | emulator.cpp:388 | ✅ 有 |
| 建 WindowSDL | emulator.cpp:587 | ✅ 有(frontend 耦合) |
| **HLE 注册** `InitHLELibs(&linker->GetHLESymbols())` | emulator.cpp:666 → [libs.cpp:96](../../src/core/libraries/libs.cpp) | ✅ 有 |
| **加载模块** `linker->LoadModule(eboot)` | emulator.cpp:669 | ✅ 有 |
| **执行** `linker->Execute(args)` | emulator.cpp:696 | ✅ 有 |
| Execute 内:SetupMemoryRegions / LoadLibcInternal / malloc_init / sceSysmodulePreloadModuleForLibkernel / 模拟 GnmDriver DirectMemory | linker.cpp Execute lambda | ✅ 有 |
| 设 `params.entry_addr = module->GetEntryAddress()` → `RunMainEntry(&params)` | linker.cpp:217 | ✅ 有 |
| **RunMainEntry 执行 guest 入口** | [linker.cpp:41-64](../../src/core/linker.cpp) | ❌ **ARM64 = UNREACHABLE** |

关键断点([linker.cpp:41](../../src/core/linker.cpp)):
```cpp
static PS4_SYSV_ABI void* RunMainEntry [[noreturn]] (EntryParams* params) {
#ifdef ARCH_X86_64
    asm volatile(... "jmp *%0\n" : : "r"(params->entry_addr), ...);   // desktop
    UNREACHABLE();
#else
    UNREACHABLE_MSG("RunMainEntry unimplemented for current architecture.");  // ← ARM64 撞墙
#endif
}
```
第二处同类断点:`Module::Start`([module.cpp:104](../../src/core/module.cpp))`return reinterpret_cast<EntryFunc>(addr)(args, argp, param);`——把 guest 入口当**原生函数指针**直接调用,ARM64 上语义错误(x86 guest 代码不能被 host 直接 call)。

**当前 app 实际跑什么**:[session_backend_fex.cpp](../../src/core/host_runtime/session_backend_fex.cpp) `BuildLoopRoutine` 造一段 `sub rdi,1 / jne` 递减循环,跑完跳 return gate。注释原话:"It is NOT a real PS4 game: the Android host is not yet native (that is HN1/HN2)."

---

## 二、参考蓝图(两个 reference 分歧,决定各取什么)

**`references/shadps4-arm64` @ be6bc2e9 —— 执行/HLE/VMM 的代码蓝图(采用这条)**
已有完整 ARM64 in-process FEX guest 执行,门控 `SHADPS4_ENABLE_FEX_GUEST_CPU`:
- `linker.cpp` RunMainEntry `#else` → `linker->RunGuestMain(params)`,构造 `GuestExecutionRequest{Rip=entry, Rsp, Gpr[7]=&EntryParams(RDI), Gpr[6]=exit_veneer(RSI), FsBase=GetTcbBase, MappedRanges=[code,stack,ReturnRange,CallbackReturnRange,+HLE veneers]}` → `m_fex_backend->Run(request)`。
- `module.cpp` Module::Start `#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU` → `linker->RunGuestFunction(addr,args)`(不是 cast)。
- VMM:读 `/proc/self/maps`,mmap(MAP_FIXED) 填满每个空洞 → **identity 映射(guest VA==host VA)**,USER_MAX=0x3FFFFFFFFF(Android 39-bit);非 x86 `Protect`/`Map` 去掉 PROT_EXEC(JIT 拥有可执行)。
- HLE:`LIB_FUNCTION`→`MakeHleCallAdapter`;重定位时发射 x86 veneer `mov r10,rcx; mov rax,op; syscall; ret` 并 patch GOT;FEX 陷入 syscall → `BridgeSyscallHandler::HandleSyscall` → `HleGuestBridge::Invoke` → 类型化 adapter 解 SysV 参数、调真 Orbis 函数、写回 rax/xmm0 → veneer `ret` 回 guest;re-entrant HLE→guest 用 `CallGuest`/`HandleCallback`。

**`references/Bachata-S4-android` —— out-of-process,不采用其进程/渲染模型**
Winlator/Box64 系:Kotlin `EmulationService` 用 ProcessBuilder 拉起独立 glibc `shadps4-arm64-fex`,`BACHATA/1` Unix socket 通信;渲染走**内嵌 X server(Winlator)+ Vortek WSI → AHardwareBuffer**,即 `vk_platform.cpp` 的 `createXlibSurfaceKHR`,**不是** `vkCreateAndroidSurfaceKHR`。
但其 **app UX 契约可借鉴**:PKG 在**导入时**解到扁平目录 `filesDir/games/<id>/`(含 `eboot.bin`+`sce_sys/param.sfo`),loader 拿到的是 eboot 的 host 路径,不在运行时 mount 原始 .pkg。

**我们的 HN 方案**:in-process JNI(`SessionCore`/`FexSessionBackend`,无外部进程、无 X server、无 Vortek)+ 直接 `vkCreateAndroidSurfaceKHR`(HN4)。→ 借 Bachata 的 stage 1-2(UI→service→校验 eboot、导入即解包);借 shadps4-arm64 的 stage 3-5(Run→linker→RunGuestMain→guest_cpu→HLE)作实际代码移植。

**API 适配(reference → 我们)**:reference 用扁平 `GuestCpuBackend::Run(GuestExecutionRequest)`;我们是 V0 加固的 owner-thread 模型:`CreateContext(CpuConfig, GuestAddressSpace&)` + `CreateThread(ThreadInit{entry_rip,initial_rsp,guest_tid,initial_state:RegisterPatch})` + `Run(thread, RunOptions)`([context.h:127-240](../../src/core/guest_cpu/api/context.h))。reference 硬编码 return/callback gate;**我们 `BackendCapabilities::return_gate_address` / `max_guest_address` 是 backend 上报的,linker 必须读取而非假设**。

**已有的一半基础(勿重复造)**:
- [call_adapter.h](../../src/core/guest_cpu/hle/call_adapter.h) 已有 `HleCallFrame`/`CallCursor`(SysV 整数/向量/栈参解码,指针 pin)/`HleCallRegistry::Dispatch` —— guest→host 参数解码半成品。
- [fex_context.cpp:476](../../src/core/guest_cpu/fex/fex_context.cpp) 已有 `FexSyscallDispatch` + NON-spill stop entry —— guest↔host 穿越机制。
- `StopReason::HleBoundary` 已是 API 里已知的停止原因(session_backend_fex.cpp Run 的 switch 已列)。
→ HN2 的 HLE 半场**大量已在**;缺的是重定位时的 veneer 发射 + 接到真 Orbis 符号 + re-entrant callback。

工作 API 模板已验证可用:见 [session_backend_fex.cpp](../../src/core/host_runtime/session_backend_fex.cpp) Prepare/Run(`AddressSpaceConfig`→`GuestAddressSpace::Create`→`Map`→`CreateContext`→读 `return_gate_address`→`Write`/`Protect`→`ThreadInit`+`RegisterPatch`→`CreateThread`→`Run`→`StopReason` 处理)。

---

## 三、依赖闭包现状(HN1.1 已证)

`docs/validation/android-native-host/dependency-closure.md` 已证:**loader→linker→`linker->Execute` 全程 renderer-free + audio-free**;`core/linker.cpp`/`core/module.cpp` 零 video_core/audio include。CPU/memory 闭包里唯一硬耦合 `core/memory.cpp`→`vk_rasterizer.h` **已由 RasterizerHooks seam 修掉**(commit `d4ea078e`,[rasterizer_hooks.h](../../src/core/rasterizer_hooks.h))。

`emulator.cpp` 本身 frontend 耦合(WindowSDL/g_window/discord/video_core/`window->WaitEvent()` 循环/6× quick_exit)→ 留 desktop 侧,Android 需一个不带 window 循环的 sibling Run 驱动。

**HLE 库耦合分布(已核对)**:大部分 HLE 逻辑干净,只有 overlay UI/renderer glue 拉 ImGui/video_core:
- 干净:kernel、libc_internal、playgo、random、usbd、zlib、libpng、jpeg、ajm、avplayer、videodec、ngs2、audio3d、move、hmd
- 耦合(ImGui overlay / renderer / audio):gnmdriver(1)、videoout(2)、audio(2)、np(7,全是 trophy/dialog/commerce ImGui UI)、system(2,msgdialog UI)、pad(1,imgui_core)

app CMake 现状([android/shadps4-app/core/runtime/src/main/cpp/CMakeLists.txt](../../android/shadps4-app/core/runtime/src/main/cpp/CMakeLists.txt)):**只**编 `guest_cpu_fex` + session lifecycle + 自包含 PKG extractor,**无 shadps4_host_core**。

---

## 四、VMM 冲突(核心正确性问题,已核对坐标)

- [address_space.cpp:43](../../src/core/address_space.cpp) `USER_MIN = 0x1000000000`(64GiB),[:47/:50/:52](../../src/core/address_space.cpp) `USER_MAX` 在几十 TiB 量级。
- [fex_context.cpp:763](../../src/core/guest_cpu/fex/fex_context.cpp) `kGuestAddressPolicyLimit = 1<<36`(64GiB),[:2065](../../src/core/guest_cpu/fex/fex_context.cpp) `caps.max_guest_address = kGuestAddressPolicyLimit`。注释:above this the FEX block lookup would alias。
- Orbis `MemoryManager`([memory.cpp](../../src/core/memory.cpp))经 `impl.Map`(AddressSpace PIMPL)做 host backing。

→ 旧 VMM 产出的 guest VA 会落在 guest_cpu FEX 能寻址范围之外。HN2 必须让 Orbis map 走 guest_cpu `GuestAddressSpace::Map`(或让 VMM 在 guest_cpu 的 reservation 内分配并读 `max_guest_address`),且非 x86 去 PROT_EXEC。guest_cpu 侧 VMM 面:`GuestAddressSpace::Create`/`Map(range,permission)`/`Protect`/`Query`/`HostPointer`([address_space.h](../../src/core/guest_cpu/api/address_space.h))。

---

## 五、建议的补齐顺序(依赖序,每步解锁下一步)

> 完整版见配套 `android-native-host-full-load-chain-2026-09-12.md`(同目录)。以下为审核用摘要。

| 步 | 内容 | 环境 | 本机可验证到 |
|---|---|---|---|
| **G-A** | HN1 target-split:抽 `shadps4_host_core` 静态库(COMMON+loader+linker+module+tls+aerolib+memory[经 RasterizerHooks]+address_space+kernel+libc_internal+guest_cpu;renderer/SDL/audio-free)。desktop 与 Android JNI 都 link 它。 | 本机 NDK 单 TU + desktop configure;full link 需 Linux/CI | configure 通过 + 逐 TU 编译 |
| **G-B** | HN2.1 VMM 统一:Orbis map 落进 FEX reservation(读 `max_guest_address`),非 x86 去 PROT_EXEC。 | Linux/CI 或真机 | 单 TU 语法 |
| **G-C** | HN2.2 guest 入口桥:`RunMainEntry`/`Module::Start` 的 ARM64 分支 → `CreateContext`+`CreateThread`+`Run`(读 `return_gate_address`,不硬编码);门控 V0_ENABLE_FEX。 | Linux/CI link 或真机 run | 单 TU 语法 |
| **G-D** | HN2.3 HLE veneer + dispatch:重定位发射 veneer + patch GOT + 注册可执行 range;FEX syscall 陷入 → `HleCallRegistry::Dispatch` → 真 Orbis 符号;re-entrant callback。首个真 HLE 往返:sceSysmodulePreloadModuleForLibkernel / sceKernelAllocateDirectMemory。 | 真机 run | 单 TU 语法 |
| **G-E** | Android Run 驱动:emulator.cpp 的 renderer-free sibling(无 SDL/window 循环),用 SessionCore 驱动真 loader→Execute→guest,替掉合成 loop。 | Linux/CI + 真机 | 单 TU 语法 |
| **G-F** | app 接线:PKG import→`filesDir/games/<id>/`→校验→session 启动把 eboot host 路径喂给 Run 驱动。 | 真机 | Kotlin/JNI 逻辑 |
| **G-G** | 渲染/Surface:HN4 已备 swapchain seam;补 `ANativeWindow_fromSurface`→`render_surface` handoff、真 GNM→Vulkan renderer 进闭包、device WSI_PASS。TMNT 出画在这一步。 | 真机 | — |

**里程碑验收(正规链路,非 smoke)**:M1(G-A)host_core configure+逐 TU 编过、desktop CI 仍构建;M2(G-B+C)真 eboot→loader→CpuContext::Run 到 guest 入口无 UNREACHABLE;M3(G-D)到首个真 Orbis HLE 并返回、libc init 跑;M4(G-E+F)TMNT 导入+in-process 启动到 guest main+HLE init(无渲染);M5(G-G)首帧+device WSI_PASS。

---

## 六、环境限制(交付/验证)

- 本 Mac **无法 desktop 全量 build**(Vulkan-Hpp `eMesaKosmickrisp` + libc++ `stop_token`/`jthread`,未改文件同样报)。
- 本机可做:NDK 单 TU `-fsyntax-only`(aarch64-android33 / gnu++2b / `-DUSE_OS_TZDB=1`,pinned externals/vulkan-headers)、desktop configure、host 单元测试(session_lifecycle_tests 767/0)、v0 runner。
- **G-B~G-D 的 full link / guest run 只能在 Linux/CI 或 AYN Thor `9c2841a4`(API33/arm64/4KiB)真机**。本机产出诚实标 NOT_RUN,把确切 CI/设备命令写进 spec。
- 不改 FEX 子仓(pin `385a0cc4`);不误用 Windows HookPtrs;guest_cpu allocator pre-owned-region provider 仍 BLOCKED(见 `docs/validation/android-native-host/allocator-provider.md`)。

---

## 附:审核者可直接核对的坐标

- 断点:linker.cpp:41-63(RunMainEntry ARM64 UNREACHABLE)、module.cpp:104(entry cast)
- 正规链路:emulator.cpp:666/669/696、libs.cpp:96(InitHLELibs)
- 合成 loop:session_backend_fex.cpp(BuildLoopRoutine)
- VMM 冲突:address_space.cpp:43/47、fex_context.cpp:763/2065
- 已有 HLE 基础:call_adapter.h(HleCallFrame/CallCursor/HleCallRegistry::Dispatch)、fex_context.cpp:476(FexSyscallDispatch)
- 已修耦合:rasterizer_hooks.h、memory.cpp(commit d4ea078e)
- 工作 API 模板:session_backend_fex.cpp Prepare/Run
- app CMake 现状:android/shadps4-app/core/runtime/src/main/cpp/CMakeLists.txt
- 蓝图:references/shadps4-arm64/src/core/{linker.cpp,module.cpp,guest_cpu/,fex/}、references/Bachata-S4-android(out-of-process,仅借 app UX)
