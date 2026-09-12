# NDK/bionic host closure + AAudio + Vulkan surface (2026-09-12)

Parallel first batch of the host-side NDK/bionic work: prove the loader/memory/
kernel-min closure compiles for bionic, add an AAudio output backend, and add the
Android Vulkan surface branch. Completion bar per the user: **NDK cross-compile
passes** (single-TU `-fsyntax-only` against the API 33 sysroot; full headless
target link is a later stage). Real-device audio/graphics is HN4/HN5.

Toolchain: NDK `29.0.14206865`, `--target=aarch64-linux-android33`, `-std=gnu++2b`
(the project's standard; `-std=c++20` on this host's libc++ lacks `std::to_underlying`
and would false-fail), `-DUSE_OS_TZDB=1` (as the desktop build sets). Desktop
non-regression checked per-TU via `build/desktop-probe/compile_commands.json`.

## Increment A — host closure compiles under NDK (17/17 TUs PASS)

All single-TU `-fsyntax-only`, aarch64-android33:

| TU | Status |
|---|---|
| core/loader/elf.cpp, symbols_resolver.cpp, dwarf.cpp | PASS |
| core/module.cpp, linker.cpp, tls.cpp | PASS |
| core/aerolib/aerolib.cpp, stubs.cpp | PASS |
| core/memory.cpp (via HN1.1 RasterizerHooks seam), address_space.cpp | PASS |
| core/libraries/kernel/time.cpp, threads.cpp, memory.cpp, process.cpp, kernel.cpp | PASS |

Two genuine bionic gaps found and fixed (the rest were only include-path
discovery, not code defects):

1. **`kernel/time.cpp` — `std::chrono::current_zone()` absent in NDK libc++.**
   bionic ships no `<chrono>` time-zone database. The file already had a
   HowardHinnant-`date` fallback for `__APPLE__`/`__FreeBSD__`; extended the guard
   (both the `#include <date/tz.h>` and the `current_zone()` call) to `__ANDROID__`,
   using the `date` library the project already links, with `USE_OS_TZDB=1` reading
   bionic's `/system/usr/share/zoneinfo`. The target HLE `sceKernelGetProcessTime`
   (NID `4J2sUJmuHZQ`) lives in this TU but uses no tz DB; only
   `sceKernelConvertLocaltime` needed the fix. Reference: azahar uses the same
   `date` library on Android.
2. **`kernel/kernel.cpp` — libuuid (`uuid/uuid.h` / `uuid_generate`) absent on
   bionic.** Added an `__ANDROID__` branch to `sceKernelUuidCreate` that builds an
   RFC 4122 v4 UUID from `arc4random_buf` (always available on bionic) with the
   correct version/variant bits, instead of linking libuuid.

Not addressed here (out of this batch, noted for HN1/HN2): `module.cpp:104`
(guest entry cast to a native function pointer) and `linker.cpp` `RunMainEntry`
(x86 inline asm) **compile** because they sit behind `ARCHITECTURE_x86_64` gates,
but their ARM64 execution path must be replaced with `CpuContext::Run` in HN2 —
compiling is not the same as executing guest entry on ARM64. A full
`shadps4_host_core` library link still needs the CMake target split (HN1.1 design,
not yet built) and cannot be verified on this macOS host (desktop full build
blocked by environment).

## Increment B — AAudio output backend (PASS)

New `src/core/libraries/audio/aaudio_audio_out.cpp` + `AAudioOut : AudioOutBackend`
(guarded `#if defined(__ANDROID__)` in `audioout_backend.h`); `audioout.cpp`
selects it on Android.

Model: **blocking write** (`AAudioStream_write` with a 200 ms timeout), not a data
callback. shadPS4's `PortBackend::Output(void*)` is already a guest-driven blocking
push at buffer-period intervals, so writing straight to AAudio matches the contract
and means there is **no audio-callback thread** to keep RT-safe — no locks, no
allocation, no JNI, no guest re-entry, because there is no callback. This is
stricter than citron's Oboe data-callback (which takes `sample_count_lock` in the
callback). Disconnect handling: `AAUDIO_ERROR_DISCONNECTED`/`INVALID_STATE` from
write rebuilds the stream once and retries (the recovery citron gets from Oboe's
`onErrorAfterClose`). S16→float conversion up-front (mirrors the SDL backend's
Convert*), guest float copied through; channel count verified against the granted
stream, mismatch refused rather than emitting garbled audio.

Builder: `AAUDIO_FORMAT_PCM_FLOAT`, `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`,
`AAUDIO_SHARING_MODE_SHARED`, `AAUDIO_USAGE_GAME`, capacity = 4 guest buffers.
Verified: NDK `-fsyntax-only` PASS against `aaudio/AAudio.h`; `libaaudio.so` present
at API 33. Real-device host-tone is HN5 (needs the backend wired into a running
session).

## Increment C — Android Vulkan surface branch (PASS)

`WindowSystemType::Android` added to `sdl_window.h`. `vk_platform.cpp`
`CreateSurface` gains an Android arm: `vk::AndroidSurfaceCreateInfoKHR{.window =
(ANativeWindow*)window_info.render_surface}` → `createAndroidSurfaceKHR`;
`GetInstanceExtensions` adds `VK_KHR_ANDROID_SURFACE_EXTENSION_NAME`;
`#include <android/native_window.h>` under the Android guard. The renderer only
ever sees the opaque `ANativeWindow*` handle — never the Java Surface or a JNIEnv
(matches azahar/citron). Verified: NDK `-fsyntax-only` PASS (`vulkan_android.h`,
`vkCreateAndroidSurfaceKHR`); desktop `vk_platform.cpp` non-regression PASS.

Reserved for HN4 (not done here, and not compile-verifiable in this batch because
the swapchain TU pulls in the whole renderer + ImGui graph):
- **Bounded `acquireNextImageKHR`.** `vk_swapchain.cpp:108` uses
  `std::numeric_limits<u64>::max()` (infinite). The reference research flagged that
  neither azahar nor citron interrupt a blocked acquire on surface loss/detach;
  spec HN4 requires a bounded timeout + stop flag. Left unchanged this batch — a
  change there cannot be NDK-compiled standalone yet.
- Android identity pre-transform, swapchain recreate on SurfaceLost/OUT_OF_DATE,
  the surface handoff protocol (azahar `NotifySurfaceChanged`/`recreate_surface_cv`
  + release-old-window), and the present thread — all HN4.
- Decoupling `CreateSurface` from `Frontend::WindowSDL&` (it transitively drags SDL3
  via `sdl_window.h`): the Android path only needs `WindowSystemInfo`. This is the
  HN4 `WindowSystemInfo` seam; this batch adds the Android case without yet cutting
  the SDL include.

## Not reusable from foundation

Foundation currently exposes only DebugBus (+ Android dumpsys); it has no
AAudio/ANativeWindow facility, so the audio and Vulkan surface code is new shadPS4
code referencing azahar/citron. Foundation reuse stays limited to DebugBus
diagnostics (already wired). No FEX child change; no X server / Vortek.

## Test PKG for later stages

`/Users/bytedance/game/ps4/TMNT.Splintered.Fate_CUSA50828_v1.08.pkg` (CUSA50828
v1.08, 1.87 GB) is the user-provided title for later import/run testing (HN2 real
ELF onward, HN6 PKG import). Not exercised in this batch (host loader/renderer not
yet wired).

## Verification commands

- NDK per-TU: `aarch64-linux-android33-clang++ -std=gnu++2b -fsyntax-only
  -fexceptions -DANDROID -D__ANDROID__ -DARCH_ARM64 -DUSE_OS_TZDB=1 <includes> <tu>`
  (include roots: src, externals/{fmt,spdlog,json,vulkan-headers,magic_enum,
  ext-boost,xbyak,zydis,zydis/.../zycore,sdl3,robin-map,tracy/public,date}/include).
- Desktop non-regression: single-TU from `build/desktop-probe/compile_commands.json`
  with `-fsyntax-only` (memory.cpp, audioout.cpp, time.cpp, kernel.cpp,
  vk_platform.cpp all PASS).
- Host regression unaffected: `session_lifecycle_tests` 767/0, v0 runner 23/23,
  JVM 92/0.
