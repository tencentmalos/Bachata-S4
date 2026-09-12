# 补齐正规游戏加载链路 — gap map + ordered plan (2026-09-12)

> 后续独立复核已发现“只差入口”“Mac不能NDK link”和部分验收推断不成立。本文保留为先前方案记录；当前执行以 [PKG v2整版spec](android-native-host-pkg-v2.md) 和 [最新复核](../validation/android-native-host/full-pkg-review-2026-09-12.md) 为准。

User directive: 按 Bachata-S4 加载真实游戏的完整正规链路补齐,**不做只供测试的最短路径**,缺什么按依赖顺序逐步补。测试内容 = TMNT (CUSA50828 v1.08).

This supersedes the earlier "minimal HN2 smoke" framing. Evidence: two reference
traces (shadps4-arm64 @ be6bc2e9 in-process FEX core; Bachata-S4-android
out-of-process Winlator/Vortek) + our-side reads. See memory
`shadps4-android-real-game-load-chain`.

## The normal chain, and where OUR fork breaks it

Desktop/standard chain (already present in `src/emulator.cpp::Run`):

```
Run(eboot) → Mount /app0 → param.sfo → WindowSDL → InitHLELibs(&linker->GetHLESymbols())
  → linker->LoadModule(eboot) → linker->Execute(args)
      → SetupMemoryRegions → main_thread.Run{ preload libs, GnmDriver init,
          params.entry_addr = module->GetEntryAddress(); RunMainEntry(&params) }
      → RunMainEntry:  ARCH_X86_64 = asm jmp *entry_addr   (desktop)
                       ARM64       = UNREACHABLE_MSG(...)   ← BREAKS HERE
```

Because ARM64 hits UNREACHABLE at the last step, the app bypasses the whole chain
and runs a synthetic decrement loop in `session_backend_fex.cpp`. To run TMNT the
正规 way we must make the real chain reach guest execution on ARM64, then compose
it into the app.

## Reference divergence (decides what we copy from where)

- **shadps4-arm64** = the code blueprint for exec/HLE/VMM (in-process, FEX, gated
  `SHADPS4_ENABLE_FEX_GUEST_CPU`). Port its SHAPE, adapt to our hardened
  `CpuContext`/`GuestAddressSpace` API (ref uses a flatter `GuestExecutionRequest`).
- **Bachata-S4-android** = out-of-process (ProcessBuilder + `BACHATA/1` socket) +
  Winlator X server + Vortek→AHB. We do NOT copy its process/render model; our
  plan is in-process JNI + direct `vkCreateAndroidSurfaceKHR`. We DO borrow its
  app UX contract: PKG extracted to a flat dir at import (`filesDir/games/<id>/`
  with `eboot.bin`+`sce_sys/param.sfo`), loader handed the host eboot path.

## Ordered gap list (dependency order — each unlocks the next)

### G-A. HN1 target-split: `shadps4_host_core` static lib (BUILD FOUNDATION)
Today `add_executable(shadps4 ...)` aggregates everything; the app CMake compiles
only `guest_cpu_fex` + session lifecycle. There is no reusable core lib. Extract a
renderer/SDL/audio-free `shadps4_host_core` = COMMON + loader + linker + module +
tls + aerolib + memory (via RasterizerHooks) + address_space + libraries/kernel +
the HLE registration (libs.cpp + all RegisterLib) + guest_cpu. dependency-closure.md
already proved this set is headless-clean (only memory.cpp→vk_rasterizer coupling,
already fixed d4ea078e). Desktop `shadps4` and the Android JNI both link it.
Compile gate: NDK single-TU per file + full link on Linux/CI (macOS host can't full-build).

### G-B. HN2.1 VMM unification (guest VA must live in the FEX reservation)
`address_space.cpp` USER_MIN 0x1000000000 / USER_MAX tens-of-TiB vs guest_cpu FEX
`kGuestAddressPolicyLimit = 1<<36`. Reference solves it by reserving the WHOLE
guest VA window (identity, /proc/self/maps gap-fill mmap MAP_FIXED) and capping
USER_MAX = 0x3FFFFFFFFF (39-bit Android). Our path: route Orbis `MemoryManager`
maps through guest_cpu `GuestAddressSpace::Map` (or make the plain VMM reserve
inside guest_cpu's reservation and read `BackendCapabilities::max_guest_address`).
Non-x86 must drop PROT_EXEC (JIT owns exec). This is the correctness core.

### G-C. HN2.2 guest entry bridge (RunMainEntry/Module::Start → CpuContext::Run)
Replace the ARM64 `UNREACHABLE` in `linker.cpp:41 RunMainEntry` and the
`reinterpret_cast<EntryFunc>` in `module.cpp:104 Module::Start` with a
`Linker::RunGuestMain`/`RunGuestFunction` that: CreateContext(CpuConfig, space) →
CreateThread(ThreadInit{entry_rip, initial_rsp, guest_tid, initial_state:
RDI=&EntryParams, RSI=exit_veneer, FsBase=TCB}) → Run(thread). Read
`BackendCapabilities::return_gate_address` for the exit veneer (do NOT hard-code).
Gate behind our own `SHADPS4_ENABLE_FEX_GUEST_CPU`-equivalent (or V0_ENABLE_FEX).

### G-D. HN2.3 HLE veneer + dispatch wiring (guest→host Orbis→guest)
We already have `call_adapter.h` (HleCallFrame/CallCursor/HleCallRegistry::Dispatch)
and `fex_context.cpp` FexSyscallDispatch + NON-spill stop entry. Missing: (1) veneer
emission during linker relocation (emit `mov r10,rcx; mov rax,op; syscall; ret`,
patch GOT, register the veneer page as an executable GuestAddressSpace range);
(2) route the FEX syscall trap → HleCallRegistry::Dispatch → real Orbis symbol;
(3) re-entrant HLE→guest callback path (CallGuest/HandleCallback) for heap malloc etc.
First real HLE round-trip target (normal chain already calls these in Execute):
sceSysmodulePreloadModuleForLibkernel, sceKernelAllocateDirectMemory.

### G-E. Android Run driver (emulator.cpp sibling, no SDL/window loop)
`emulator.cpp::Run` is frontend-coupled (WindowSDL, g_window, discord,
`window->WaitEvent()` loop, video_core includes). Build an Android-side Run entry
that does the renderer-free stages (mount, sfo, InitHLELibs, LoadModule, Execute)
against `shadps4_host_core`, driven by the in-process `SessionCore` backend
(replace the synthetic loop in `FexSessionBackend` with the real
loader→Execute→guest run). Surface comes from HN4/HN5, not a WaitEvent loop.

### G-F. App wiring: PKG import → run
PKG extractor already exists (pkg_extractor.cpp, PFS/PFSC/eboot). Wire: import →
`filesDir/games/<id>/` (eboot.bin + sce_sys) → verify → session start hands the
host eboot path to the Android Run driver. (Bachata's ContentImporter contract.)

### G-G. Renderer/Surface (HN4 partial done → HN5)
HN4 landed the Android WSI swapchain seam (bounded acquire, surface rebuild).
Remaining: app `ANativeWindow_fromSurface`→`window_info.render_surface` handoff,
a frame-producing path (the real guest GNM→Vulkan renderer, which the host_core
must also include for a game that draws), device WSI_PASS. TMNT will not display
until this + the video_core pipeline are in the host closure.

## Sequencing & environment
G-A → G-B → G-C → G-D are the load-bearing spine and can be developed with NDK
single-TU syntax checks here; **full link/run needs Linux/CI or on-device**
(macOS host cannot full-build: Vulkan-Hpp eMesaKosmickrisp + libc++ stop_token).
G-E/G-F are app-integration once the spine links. G-G (display) is last and
pulls the whole video_core into the closure. Do NOT skip to a test-only shim;
each step is a real piece of the normal chain.

## Milestone acceptance (normal-chain, not smoke)
- M1 (G-A): `shadps4_host_core` configures + per-TU NDK compiles; desktop still builds on CI.
- M2 (G-B+C): real eboot ELF → loader → CpuContext::Run reaches guest entry on ARM64 (Linux/CI link or device), no UNREACHABLE.
- M3 (G-D): guest reaches the first real Orbis HLE (sceSysmodulePreload / AllocateDirectMemory) and returns; libc init runs.
- M4 (G-E+F): TMNT eboot imported + launched in-process through the app; reaches guest main + HLE init (no renderer yet).
- M5 (G-G): first presented frame from the real renderer; device WSI_PASS.
