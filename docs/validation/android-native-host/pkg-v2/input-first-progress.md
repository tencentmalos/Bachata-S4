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

## Still open (continuous, same package)
- Layer 2: Foundation Android Kotlin library (AndroidInputSource / InputSink /
  HapticsExecutor) — Gradle library, injectable, no app-specific native methods.
- Layer 3: main-repo app JNI bridge (session token + epoch validation, RAII
  lease), OrbisPadAdapter (Button/Axis → PS4 bits/0..255/center, user/port,
  scePadRead/ReadState history + scePadSetVibration), overlay merge, connect the
  currently-unconsumed ManagedSession controller sink.
- Then SDL removal (imgui_core Android path already scoped; audio/mouse/camera/
  settings via host-UI interface; input_handler→settings_dialog_layer decouple;
  drop SDL3 link + externals SDL for Android; zero SDL symbols / no SDL JNI_OnLoad).
- Then production Runtime / real PKG per the integrated spec.
