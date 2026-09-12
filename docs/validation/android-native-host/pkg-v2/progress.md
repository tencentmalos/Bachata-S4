# PKG v2 progress — WP-A §4.1 session boundary fixes (2026-09-12)

Executing [PKG v2 整版任务书](../../specs/android-native-host-pkg-v2.md). Base
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
