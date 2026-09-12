# Android input-first progress — Foundation modules/input (2026-09-12)

Executing [android-foundation-input-first.md](../../specs/android-foundation-input-first.md).
Order: controller in → Android SDL removal → continue production Runtime/PKG.

## Layer 1 — Foundation `modules/input` C++ core — DONE (host + NDK verified)

Foundation subrepo `tencentmalos/foundation` branch `codex/shadps4-android-fex-v0`,
committed + pushed `087fcef` (parent gitlink advanced from 1f70088).

- New standalone module `foundation/modules/input`: `spatial::input` value types
  (`input_types.h`) + `spatial::input::InputHub` (`input_hub.{h,cpp}`). The shared
  MECHANISM only — device registry, event/state model, axis normalization, haptic
  queue. PS4 semantics stay in the consumer.
- Standalone: depends only on the C++ stdlib + threads. No SDL/JNI/XR/ImGui/FEX/
  Orbis/allocator/Foundation-module-system. Registered behind
  `FOUNDATION_ENABLE_INPUT` (default OFF) so existing hosts are unchanged.
- **Verified:** contract test `foundation_input_tests` 38 checks / 0 failures on
  the host (control correctness incl. stick/trigger normalization + NaN/Inf/
  oversized rejection; stale-epoch/wrong-session/reconnect identity; multi-device
  isolation; focus-lost release; haptics enqueue/cancel-supersede/unsupported/
  drain/remove). Also cross-compiles to a real AArch64 relocatable object under
  the Android NDK (aarch64-linux-android33).
- README documents the two independent consumption paths (C++-only; Android
  Kotlin library with no app-specific JNI/JNI_OnLoad/.so), threading/lifetime
  contract, and the citron migration source (2106bcd8, logic only).

Satisfies the plan's "真正共用" acceptance dimension for the C++ core: the reusable
mechanism exists in exactly one place (Foundation) and builds without SDL/JNI/XR
or the main repo.

## Layer 3 — native OrbisPadAdapter + pad JNI + Kotlin bridge — DONE (on-device verified)

The seam the Kotlin input stack was missing. Before this, `GamepadInputManager`
(physical pad) and the touch overlay both resolved input into a `ControllerSnapshot`
(already in PS4 `Ps4Button` bits, sticks [-1,1], triggers [0,1]) →
`ManagedSession.submitController()` → a Kotlin sink **no native code consumed**. No
`OrbisPadAdapter` and no pad JNI existed.

New:
- `src/core/host_runtime/orbis_pad_adapter.{h,cpp}` — `Core::HostRuntime::OrbisPadAdapter`.
  Reuses the REAL `Libraries::Pad::OrbisPadData` layout (single source of truth, no
  shortcut POD) and the exact production conversion `pad.cpp/ProcessStates` uses:
  sticks → u8 centred at 128, triggers → u8 0..255, button bits passed through.
  Per-port state (4 ports), session/generation guarded like SessionCore (a stale
  token from a previous game is refused), lock-guarded consistent reads, latest-wins
  vibration slot the Android side drains. No SDL, no FEX, no guest_cpu — depends only
  on `pad.h` + the stdlib.
- `android/shadps4-app/core/runtime/src/main/cpp/orbis_pad_jni.cpp` — thin JNI
  marshaller (jlong bits + jfloats, never a native pointer), every boundary
  catches C++ exceptions → defined ordinal. Folded into the existing
  `libshadps4_fex_session.so`, so no new `System.loadLibrary`. All 10
  `Java_..._NativePad_native*` symbols export in the shipped arm64 `.so`.
- `NativePad.kt` (typed external decls + wrappers) and `NativePadBridge.kt`
  (attaches the `ManagedSession` slot sink, mints/ends a native input session
  token per game generation, forwards each snapshot to native under the token).
- `FexSessionService` wires `NativePadBridge.begin()` after `nativeStart` and
  `NativePadBridge.end()` after the terminal — the pad generation follows the game
  generation.

**Verified:**
- Host contract `orbis_pad_adapter_tests` **59 checks / 0 failures** (conversion,
  session/generation, port isolation + connect/disconnect, vibration
  enqueue/cancel/drain). Registered in `cmake/fex` as a first-class host gate.
- Same test cross-compiled to an arm64 device executable and run **on the AYN Thor
  (arm64/API33/4KiB bionic): 59/0**.
- On-device instrumented test `NativePadInstrumentedTest` on the AYN Thor (Android
  13, `qti/kalama` Thor): **4 tests / 0 failures** — full JVM→JNI→native path:
  buttons+sticks round-trip, stale-token session guard, the `NativePadBridge` +
  `ManagedSession` sink wiring (the dead end now closed), and vibration through JNI.
  Evidence: `native-pad-instrumented-2026-09-12.xml`.
- Full `com.shadps4.android` debug APK (50M) builds, installs, and launches cleanly
  on the AYN Thor (no `UnsatisfiedLinkError`, process stays up).

Boundary kept honest: this makes real controller input reach a real PS4 pad-state
consumer. It does NOT yet run a PS4 game — wiring `OrbisPadAdapter` into the FEX
guest's `scePadReadState` is a later stage (the injection shape matches that seam,
so the swap is wiring not a rewrite).

## Still open (continuous, same package)
- Layer 2: Foundation Android Kotlin library (AndroidInputSource / InputSink /
  HapticsExecutor) — Gradle library, injectable, no app-specific native methods.
  The main-repo `GamepadInputManager` already fills this role for the app today;
  extracting the reusable Kotlin mechanism into Foundation is the remaining part.
- Vibration executor on the Android side (drain `NativePad.drainVibration` →
  `VibratorManager`); haptics currently reach native but no Android pump executes.
- Then SDL removal (imgui_core Android path already scoped; audio/mouse/camera/
  settings via host-UI interface; input_handler→settings_dialog_layer decouple;
  drop SDL3 link + externals SDL for Android; zero SDL symbols / no SDL JNI_OnLoad).
- Then production Runtime / real PKG per the integrated spec, incl. binding
  `OrbisPadAdapter` into the guest `scePadReadState`.
