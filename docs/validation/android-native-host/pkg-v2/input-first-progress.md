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

## Layer 2 — Foundation Android input library — DONE (on-device verified)

Foundation subrepo `codex/shadps4-android-fex-v0`, committed + pushed `96a3eea`
(parent gitlink advanced from `087fcef`).

The injectable Kotlin mechanism under `foundation/modules/input/android/` (a plain
Android library, namespace `spatial.input.android`, referenced once from the
subrepo path via the app's `settings.gradle.kts`):
- `spatial.input.model.*` — neutral Kotlin model mirroring the C++ value types
  (Button/Axis by physical position, DeviceIdentity with connection epoch separate
  from profile key, InputPacket/InputEvent/ControlEvent, HapticCommand). Enum
  ordinals match the C++ side.
- `AndroidInputSource(context, emitExecutor, sink)` — device discovery via
  `InputManager.InputDeviceListener`, capability reporting from `motionRanges` /
  `hasKeys`, `dispatchKeyEvent`/`dispatchGenericMotionEvent` → neutral `InputPacket`
  on the injected serial executor. Reconnect → new epoch; no fabricated device; no
  user remap/dead zone applied (host does that once, matching the C++ hub contract).
- `InputSink` / `HapticFeedbackSource` — the two injection interfaces the host wires.
- `AndroidHapticsExecutor(context, source)` — bounded pump draining the feedback
  source onto the platform / per-controller `Vibrator`; no-vibrator no-op,
  single-actuator honest downmix. Migrated in spirit from citron's vibrator
  (system-vs-controller, API-31 split) without the Citron singleton / R /
  `CitronApplication` deps.

**Verified:** own namespace, references no host `R`/Compose/`ManagedSession`/
`CitronApplication`; declares NO app-specific native methods, no
`System.loadLibrary`, no `JNI_OnLoad`, no `.so`. Built as an AAR (`classes.jar`
only — no `.so`, no `jni/`, no `JNI_OnLoad`). Instrumented test on the AYN Thor
(Android 13): **4 tests / 0 failures** with an injected fake sink, referencing no
shadPS4/citron package or native library (spec §130 acceptance). Evidence:
`foundation-android-input-instrumented-2026-09-12.xml`.

Note on the app's own path: the main-repo `GamepadInputManager` +
`NativePad`/`NativePadBridge` (Layer 3) remain the production controller path
today, already on-device-verified. This Foundation library is the reusable
mechanism extraction other Foundation consumers can adopt; a follow-up can migrate
the app's producer onto it, but the app path is not blocked on that.

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
- On-device instrumented tests on the AYN Thor (Android 13, `qti/kalama` Thor):
  **6 tests / 0 failures** — full JVM→JNI→native path.
  - `NativePadInstrumentedTest` (4): buttons+sticks round-trip, stale-token session
    guard, the `NativePadBridge` + `ManagedSession` sink wiring (the dead end now
    closed), and vibration through JNI.
  - `HapticsPumpInstrumentedTest` (2): native vibration slot → `HapticsPump` drain →
    sink (rumble + cancel + empty), and the threaded start/stop cancel-all.
  Evidence: `native-pad-instrumented-2026-09-12.xml`.
- Full `com.shadps4.android` debug APK builds, installs, and launches cleanly on the
  AYN Thor (no `UnsatisfiedLinkError`, process stays up). The APK manifest carries
  `android.permission.VIBRATE` (merged from the runtime module).

Boundary kept honest: this makes real controller input reach a real PS4 pad-state
consumer. It does NOT yet run a PS4 game — wiring `OrbisPadAdapter` into the FEX
guest's `scePadReadState` is a later stage (the injection shape matches that seam,
so the swap is wiring not a rewrite).

## Haptics executor — DONE (on-device verified)

The vibration path previously reached native (`scePadSetVibration` →
`OrbisPadAdapter.SetVibration`) but nothing pumped it to hardware. Closed:
- `HapticsSink` interface (injectable mechanism) + `HapticsPump` (bounded worker
  that drains `NativePad.drainVibration` per port each tick → sink; start/stop with
  a join budget; a final `cancelAll` on stop; not session-guarded on drain so a
  stop can still deliver a final cancel — the native adapter clears the queue on a
  new session, so no stale rumble leaks into the next game).
- `VibratorHapticsSink` — Android `VibratorManager`/`Vibrator` impl mapping the two
  DS4 motors to a combined amplitude; safe no-op with no actuator.
- `FexSessionService` starts the pump after `NativePadBridge.begin()` and stops it
  after the terminal / in `onDestroy`, so the actuator follows the game generation.
- Runtime manifest declares `android.permission.VIBRATE`.
- Verified in the 6-test on-device suite above (`HapticsPumpInstrumentedTest`).

## SDL removal from the Android host — DONE (host link + on-device verified)

`libshadps4_host.so` now builds and links `--no-undefined` on Android with SDL
fully removed (desktop keeps SDL unchanged). Approach: SDL-only translation units
are excluded from the Android source lists; shared TUs that also run on Android are
guarded with `__ANDROID__`; the `SDL3::SDL3` link and the `externals/sdl3`
subdirectory are gated to non-Android.

Excluded on Android (`BUILD_HOST_CORE`): `imgui_impl_sdl3`, the big_picture SDL
settings dialog (`settings_dialog_imgui` / `settings_dialog_layer`),
`input_handler`, `input_mouse`, `sdl_mouse`, `sdl_audio_in`, `sdl_audio_out` (plus
the already-excluded `big_picture` / `imgui_impl_sdlrenderer3`). Added on Android:
`null_audio_in` (no-mic fallback) and `aaudio_audio_out`.

Guarded shared TUs (desktop path intact under `#ifndef __ANDROID__`):
- `imgui/renderer/imgui_core.cpp` — SDL include + `Sdl::*` / `SDL_Event` /
  `SDL_GetWindowDisplayScale` paths (Android input comes from its own adapter).
- `input/controller.{h,cpp}` — `controller.h` forward-declares `SDL_Gamepad` /
  `SDL_JoystickID` on Android (the `GameController` state machine stays for the pad
  HLE); SDL LED/rumble/discovery guarded; `TryOpenSDLControllers` logs in Player 1
  without SDL so `scePadOpen` still works; vibration goes through the app haptics
  path.
- `libraries/audio/audioin.cpp` (NullAudioIn), `mouse/mouse.cpp`,
  `camera/camera.cpp`, `emulator_settings.cpp`, `user_manager.cpp`, `ipc/ipc.cpp`,
  `devtools/layer.cpp`, `np/trophy_ui.{h,cpp}` — SDL message boxes / audio / device
  access guarded; Android returns the honest not-available / no-op path.

**Verified** (`docs/.../sdl-removal/nm-verification-2026-09-13.txt`): the linked
arm64 `.so` has **0 real SDL API functions** (`SDL_Init`/`SDL_CreateWindow`/… none),
**0 SDL `JNI_OnLoad` / `org_libsdl` Java exports**, **0 SDL undefined imports**, and
**no libSDL in DT_NEEDED**. The only remaining `SDL`-named symbols are four OF OUR
OWN functions whose signatures reference SDL types by forward declaration
(`ImGui::Core::ProcessEvent(SDL_Event*)`, `GameController::ConnectController(SDL_Gamepad*)`,
`GameControllers::TryOpenSDLControllers()`, `Frontend::Window::GetSDLWindow()`) — no
SDL implementation code. On-device host contract on the AYN Thor (Android 13):
`host_library_smoke` **61 checks / 0 failures**, matching the pre-removal baseline.

## Still open (continuous, same package)
- Layer 2 follow-up: migrate the app's own producer (`GamepadInputManager`) onto
  the Foundation Android library so the reusable mechanism has a production consumer
  (the app path works today via `NativePad`/`NativePadBridge`).
- Production Runtime / real PKG per the integrated spec: Turnip native loader, VMM
  unify, guest entry → `CpuContext::Run`, typed HLE, and binding `OrbisPadAdapter`
  into the guest `scePadReadState`, then TMNT device acceptance (10 min + Stop + 3
  same-process restarts).
