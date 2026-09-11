# HN0 build + host acceptance evidence (2026-09-11)

Source: `codex/android-fex-round2`, commits `5d9edb92` (HN0.1/HN0.3) and
`d6fb4df3` (HN0.2), on base `9ac6c300`. FEX gitlink `385a0cc4`. This records the
host-verifiable HN0 results. Device-side items (HN-U01/S01/L01) are NOT_RUN: the
acceptance device `9c2841a4` (AYN Thor / API 33) disconnected mid-session and no
device is attached; those must run when it returns.

## Result classes

- **build**: native arm64 `.so` + Kotlin compile — PASS
- **host-unit**: SessionCore lifecycle + JVM unit tests + v0 runner — PASS
- **CLI / app-auxiliary / app-Swan**: NOT_RUN (no device)

## build (HN-B01 partial)

Native session library cross-built for arm64-v8a via the app's own Gradle
externalNativeBuild (NDK 29.0.14206865, `FEX_BUILD_DIR=build/fexcore-android`,
`-DV0_ENABLE_FEX=ON`):

- `./gradlew :core:runtime:externalNativeBuildDebug` → BUILD SUCCESSFUL (31m55s).
- The new lifecycle sources compiled into the `.so`:
  `session_core.cpp.o`, `session_backend.cpp.o`, `session_backend_fex.cpp.o`
  (under `core/runtime/.cxx/RelWithDebInfo/4k3b3q13/arm64-v8a/`).
- `fex_context.cpp` with the HN0.2 retriable-guard fix is part of the linked
  `guest_cpu_fex`.
- Fresh `.so` (obj, unstripped): SHA-256
  `59cf989a3e39c9da9553293d1df3d95faba3db6b3db3e3282ec62da170593f01`, 33,672,736
  bytes, AArch64/ELF64, GNU build-id present, PT_LOAD alignment `0x4000` (16 KiB).
- Exported JNI surface is exactly the new 9-method contract (nativeStart,
  nativeRequestStop, nativeWaitPhase, nativeWaitTerminal, nativeTerminalErrorCategory,
  nativeTerminalDetail, nativeCurrentGeneration, nativePhase, nativeIdentity); the
  removed nativeIsRunning / nativeLastError / nativeLastStopReason are absent.

Note: `intermediates/stripped_native_libs/.../libshadps4_fex_session.so` was NOT
refreshed by the `externalNativeBuildDebug` task alone — it still holds a stale
16:29 build with the old symbols. A full `assembleDebug`/`assembleFdroidDebug`
refreshes the stripped/packaged copy; that plus install is part of the device
window and is NOT_RUN here.

Kotlin: `:core:runtime:compileDebugUnitTestKotlin`, `:feature:session:compileDebugKotlin`,
`:app:compileFdroidDebugKotlin` all compile (was: core:runtime test Kotlin broken).

## host-unit (HN-B02 + HN-S01/S02 logic)

- `session_lifecycle_tests` (SESSION_TEST_HOOKS, FakeBackend, UAF tripwire):
  **767 checks, 0 failures**, stable across 20 runs. Covers the four named
  interleavings (Stop-vs-destroy, context-not-published, start-thread failure,
  late old-watcher), GuestFault→Failed, and a 100-round rotating-interleaving
  fixture. Sanitizers unusable in this sandbox (trivial ASan/TSan binaries hang at
  init), so the in-code `alive` tripwire is the UAF detector.
- `./gradlew :core:runtime:testDebugUnitTest`: **92 tests, 0 failures** (incl. new
  ManagedSessionGenerationTest, 3 methods).
- `python3 tests/guest_cpu/test_v0_runner.py`: **23/23**.

## NOT_RUN (device)

- HN-U01: final non-exported-service app UI start/stop ×10 — needs install on a
  device.
- HN-S01/S02 device side: real Cancel/Run/DestroyThread ×100 with
  `LiveThreadCount()==0` between rounds — needs the optional
  `session_lifecycle_device_tests` (guest_cpu_fex) on a device.
- HN-A01 100× cold-start / 100× same-process rebuild under app UID with Java
  churn — device only.

The device dropped after this session began; re-attach `9c2841a4` (or a Swan) and
run: full `assembleFdroidDebug` → install → app start/stop ×10, then the device
lifecycle test. The retriable-guard fix (HN0.2) is what makes the same-process
rebuild case pass where a single early timeout previously bricked it.
