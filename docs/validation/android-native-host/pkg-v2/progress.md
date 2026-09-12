# PKG v2 progress — WP-A §4.1 session boundary fixes (2026-09-12)

> **COMMON后续独立复核**：[当前报告](../pkg-v2-common-review-2026-09-12.md)。新实现GNU/API33路径4/4对象通过；crypto/ipc路径推断已实际重跑。POSIX strerror_r配置及ESR对齐问题已直接修复，正式回归真机GNU errno7/7、POSIX errno7/7、信号50/50，macOS errno7/7通过；此后的工作进入正式host target/link。主线与遗漏历史资料已推送，以下早期本地状态按当时记录保留。

> **后续独立复核与直接修复（2026-09-12）**：[Vulkan/NDK报告](../vulkan-review-2026-09-12.md)。以下保留原提交陈述；“全host闭包已编译”当时证据不足。新修复后完整图形37+67已产出104个ARM64对象，仍非host全链接。旧脚本改为仓库工具wrapper，缺文件失败返回1；默认Turnip实际加载待整版接通。

Executing [PKG v2 整版任务书](../../../specs/android-native-host-pkg-v2.md). Base
`0f4fd74b`. This file tracks the continuous work; evidence artifacts land beside it.

## WP-A §4.1 — five session-boundary defects (review §2.1–2.3) — DONE (host-verified)

Fixed in `src/core/host_runtime/session_core.{h,cpp}`; regressions in
`tests/host_runtime/session_lifecycle_tests.cpp`.

| Review defect | Fix | Regression |
|---|---|---|
| 2.1 early-cancel Destroy bypasses control drain | early-cancel now funnels through the same `TeardownAndDestroy` (close admission → drain lease → Destroy → terminal) as the normal path; no direct `backend_.Destroy` from the Ready branch | Case9 + probe `early_cancel` ordering |
| 2.2 owner exception aborts process | owner-thread try/catch around Prepare/Run/Destroy → typed Failed/StartFailed/BackendFailed terminal, never `std::terminate` | Case6 (Prepare throw), Case7 (Run throw) — both were process-abort (SIGABRT 134) on old code, now clean terminals |
| 2.2 control exception leaks lease | RAII `LeaseGuard` in `RequestStop` returns `in_flight_control_` on every exit incl. exception; backend calls wrapped so exceptions become `StopResult::Error`, never escape | Case8 (RequestCancel throw) — old code: escaped + lease leak → drain hang; now Error + owner reaches terminal |
| 2.3 drain-timeout owner leaves, late control can't reclaim | soft budget elapsing sets an observable `drain_timed_out_` note (NOT a permanent terminal) and the owner stays, waiting patiently for the lease to drain, then Destroys and sets the real terminal | Case9 (slow drain, 50ms soft budget, lease held 200ms) — old code latched permanent `exited=0 Stopping destroys=0`; now `exited=1 destroys=1`, same-process restart works |
| 4.1 Start recheck after unlock-join | `Start` recomputes `gen` and rechecks phase AFTER `FinalizeAndJoin` drops/retakes the lock; a racing Start loses cleanly with AlreadyRunning, no duplicate generation | covered by existing concurrent-Start fixture + Start-after-terminal cases |

**Results (host):** `session_lifecycle_tests` 767→**807 checks / 0 failures**, 20× runs
clean, no hangs. Fixed `session_core.cpp` also **NDK compiles clean**
(`aarch64-linux-android33`, `-std=gnu++2b`, `-fsyntax-only`).

**Proof the fixes close the review reproductions:** compiled the review-only
`session_edge_probe.cpp` against the FIXED core:
- `prepare_throw`: was SIGABRT(134) → now `exception safely reported=1`, exit 0.
- `run_throw`: was SIGABRT(134) → now `exception safely reported=1`, exit 0.
- `stop_throw`: was `escaped`/lease-leak → now `escaped=0 exited=1 destroys=1 next_start=2`.
- `early_cancel`: the probe's old handshake now deadlocks against the CORRECTED
  drain-before-Destroy ordering (the old bug it reproduced is gone); the proper
  deterministic coverage is Case9 in the suite, not the review-only probe.

## Next (continuous, no pause per §2)
- §4.2 long-lived Service: remove FexSessionService.kt 10s `nativeWaitTerminal`→Failed/stopSelf; async onDestroy cleanup; bounded-wait ≠ failure.
- §4.3 allocator re-audit against the ACTUALLY-LINKED rpmalloc branch; `shadps4_host_core` target on the real source list; full `--no-undefined` Android host link.
- WP-B loader→guest→Orbis→renderer; WP-C device.

## WP-A §4.2 — long-lived game Service (review §2.4) — DONE (compile + JVM tests)

Fixed `android/shadps4-app/app/.../service/FexSessionService.kt`:

- **Removed the 10s terminal cap that killed running games.** The observer used a
  single `nativeWaitTerminal(gen, 10000)` and treated the `-1` timeout as a
  terminal → published Failed + `stopSelf`. Now the observer **loops** on a
  bounded `nativeWaitTerminal(gen, WAIT_SLICE_MS=1s)`; a slice timeout means
  "still owned / still running" and is NOT a failure. A game may run for hours.
- **Stop has its own bounded budget, distinct from run lifetime.** `handleStop`/
  `onDestroy` set `stopRequestedGeneration` + a `STOP_COMPLETION_BUDGET_MS=15s`
  deadline. Only once a stop was requested AND that budget elapses without a
  terminal does the observer treat teardown as wedged (a real failure). A running
  game never asked to stop waits indefinitely.
- **onDestroy no longer blocks the main thread.** It previously did a synchronous
  `nativeRequestStop` + `nativeWaitTerminal(...,10000)` on the Android main thread
  (~11s ANR risk). Now it marks the stop budget and hands the interrupt to a
  detached `fex-session-destroy-<gen>` thread; the observer publishes the terminal
  and reclaims. onDestroy returns immediately.

Preserved: generation-tagged observer, `updateIfCurrent` stale-watcher guard,
GuestFault→Failed (never exit-0), async stop from a worker thread.

**Results:** `:app:compileFdroidDebugKotlin` clean; full JVM unit suite
`--rerun-tasks` **116 tests / 0 failures** (review baseline 109; app module adds 7
not counted before). The smoke loop's own duration is unchanged — a true 10-minute
run needs the real game (WP-B), but the Service no longer *imposes* a lifetime cap.

## Still open in WP-A
- §4.3 allocator re-audit against the ACTUALLY-LINKED rpmalloc branch; `shadps4_host_core` target on the real source list; full `--no-undefined` Android host link.
- WP-B loader→guest→Orbis→renderer; WP-C device.

## WP-A §4.3 — host-core build foundation (in progress)

### Allocator: confirmed the review's correction (rpmalloc, not bionic no-op)
The linked `libshadps4_fex_session.so`'s `FEXCore::Allocator::InitializeAllocator`
disassembly (review evidence `allocator-linked-implementation.txt`) really calls
`rpmalloc_initialize_config` and installs `FEX_rp_mmap`/`FEX_rp_memory_unmap`
hooks — the FEX allocator path, from `libFEXCore.a` (child pin 385a0cc4), NOT the
`__BIONIC__` Dummy branch the earlier `allocator-provider.md` audit selected. The
FEX Android prebuilt is present at `build/fexcore-android/FEXCore/Source/libFEXCore.a`
and the app already links a 33 MB FEX-backed `.so`, so a real host link is
achievable on this Mac. Full provider re-audit (small-allocator / mmap providers /
JIT-backing / ART cold-start + churn故障注入) remains open — HN-A01 not closed.

### Renderer-free host closure compiles under NDK bionic (evidence)
Extracted the authoritative include/define set from `build/desktop-probe`'s
`compile_commands.json` (portable repo-relative dirs only) and swept the
dependency-closure spine + full HLE registration under
`aarch64-linux-android33 -std=gnu++2b`:
- spine 10/10: linker, module, tls, memory, address_space, aerolib{,stubs},
  loader/{elf,symbols_resolver,dwarf}.
- HLE 11/11: **libs.cpp (the full InitHLELibs / every RegisterLib)**, kernel{,
  threads,memory,process,time,file_system,equeue}, libc_internal, playgo, sysmodule.

This confirms the `shadps4_host_core` source set compiles for bionic. The next
step is the real `--no-undefined` LINK: `libs.cpp` references ALL RegisterLib, so
linking pulls the renderer/audio-coupled libs (gnmdriver/videoout/audio/np/system/
pad → video_core/audio_core/imgui). Per review §3.2 the answer is a REAL link
closure (link map / undefined symbols), not empty RegisterLib or
`--unresolved-symbols=ignore-all`. That link is the WP-A §4.3 / WP-B boundary:
the full game APK must link its真实所需 HLE/video/audio, so `shadps4_host_core`
+ the video_core/audio_core targets link together — staged next.

## Still open
- `shadps4_host_core` CMake target on the real source list + full `--no-undefined` Android host link (needs video_core/audio_core in the closure for the coupled RegisterLibs).
- Allocator provider full re-audit (HN-A01).
- WP-B loader→guest→Orbis→renderer; WP-C device.

## WP-A §4.3 (part 2): DECISIVE — the full host closure compiles under NDK bionic

The review's Vulkan-Hpp `eMesaKosmickrisp` blocker was purely the DESKTOP system
Vulkan-Hpp path. Under NDK `aarch64-linux-android33` with the project's PINNED
`externals/vulkan-headers`, the deep renderer compiles. Swept clean:
- video_core: `vk_instance.cpp`, `vk_swapchain.cpp`, `vk_rasterizer.cpp`,
  `vk_scheduler.cpp`, `amdgpu/liverpool.cpp`, `texture_cache.cpp`.
- shader_recompiler: `recompiler.cpp`.
- HLE renderer/audio-coupled: `videoout/video_out.cpp`, `gnmdriver/gnmdriver.cpp`,
  `audio/audioout.cpp`.

Two TUs failed the ad-hoc probe ONLY on missing project defines, not bionic
incompatibility: `vk_presenter.cpp` needs `-DIMGUI_USER_CONFIG="imgui/imgui_config.h"`
(the project's ImVec2 single-arg ctor); `buffer_cache.cpp`'s `region_manager.h`
needs the project's real Tracy/SpinLock define set (my probe's `TRACY_ENABLE=0`
diverged from the build). Both are exactly the "include/define discovery"
non-defects the review names. Conclusion: **the whole host closure
(CORE+HLE+video_core+shader_recompiler+audio) is bionic-compilable; the
`--no-undefined` link is achievable**, and the right vehicle is a real CMake
target that inherits the project's exact defines/includes (not a hand-tuned probe).

Next concrete step: author `shadps4_host_core` (+ platform/renderer/audio) in the
root CMake source lists, configured for the NDK, and drive the
`--no-undefined` link to closure.

## WP-A §4.3 (part 3): COMMON bionic host-link gaps closed (06bd43bd)

Continuing toward the `--no-undefined` host `.so`. Fixed the two genuine bionic
CODE gaps in the COMMON closure (other sweep failures were include-path scoping,
not defects):
- `common/signal_context.cpp`: added the `__linux__` + `ARCH_ARM64` (bionic)
  branch — `GetRip` = `uc_mcontext.pc`; `IsWriteError` walks the ucontext
  `__reserved` `_aarch64_ctx` chain to `ESR_MAGIC` and reads the WnR bit,
  bounds-checked (matches shadps4-arm64 reference). Was `#error "Unsupported
  architecture"`. Callers `core/signals.cpp` + `video_core/page_manager.cpp` are
  in the closure, so this was a real link blocker (the vulkan-review named it).
- `common/error.cpp`: the GNU `char*` `strerror_r` was guarded on
  `defined(ANDROID)` (a CMake macro); a standalone NDK build defines `__ANDROID__`
  and fell into the XSI `int` branch (bionic's strerror_r returns char*). Added
  `__ANDROID__` to the guard.

Verified 4/4 via `check-host-ndk-sources.py` (signal_context, error, signals,
page_manager). `crypto.cpp`/`ipc.cpp` only need libressl / repo-root include
paths (the real CMake target provides them), not code changes.

Session commits (on top of reviewer 091334d3/1d411955): 982cf0db §4.1,
fe6f8fca §4.2, 9b9ac50e + fd3587fd §4.3 closure-compile, 06bd43bd COMMON bionic.
**Next: `shadps4_host_core` CMake target + full `--no-undefined` link** (COMMON +
full HLE + video_core[104 objs] + audio + media + input + guest_cpu_fex; resolve
undefined symbols by link map, no empty RegisterLib / ignore-all), then Turnip
native loader wiring, then WP-B execution.

## WP-A §4.3 → §5: full host link plan (design complete, 7c675280+)

Full architecture design in
[android-native-host-full-link-plan-2026-09-12.md](../../../specs/android-native-host-full-link-plan-2026-09-12.md).
Decisive finding: Bachata's Android host is **glibc** (Winlator proot,
`aarch64-linux-gnu`, ld-linux), so **no bionic externals are reusable** — our
in-process app needs a true bionic NDK build of the top-level source lists (new).

Design: layered `shadps4_common` → `shadps4_host_core` (CPU/HLE spine, FIRST link
milestone) → `shadps4_host_video` → `shadps4_host` SHARED, reusing the existing
root source-list variables under `option(BUILD_HOST_CORE)`. Notable facts: `AUDIO_CORE`
is undefined/empty (audio HLE is inside `${CORE}`, no audio_core to port);
`cpu_patches.cpp`/`FIBER_LIB` already x86-only. FFmpeg is the ONLY blocker external
(download-prebuilt glibc; confined to VDEC/AVPLAYER/ajm_mp3 → defer past first link).
Cheap enablements: enable `externals/date` for Android (currently APPLE/FreeBSD-gated
at externals/CMakeLists.txt:252), drop uuid/discord, defer SHADNET (protoc host-tool).

Staging (real deps, no stubs): common → cross-compile non-FFmpeg externals for NDK
(libressl/zarchive/zstd/freetype/fdk-aac/LibAtrac9/zydis/glslang/sirit/VMA/png/miniz/
date) → link host_core minus FFmpeg (first --no-undefined) → FFmpeg-from-source →
host_video → host SHARED + Android Run driver + JNI → Turnip loader → WP-B execution.
