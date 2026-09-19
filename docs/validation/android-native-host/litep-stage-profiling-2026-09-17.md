# Litep startup and multi-frame stage delivery — 2026-09-17

## Implemented and verified

This delivery supports two separate kinds of observation: startup with zero or
warmup submission markers, and in-game loading spanning multiple submissions and
long periods without a new game image. Neither is converted to one artificial
frame or normalized by frame count.

Profiler SDK had no remote `master`; `git ls-remote --heads` verified its default
`main` at **2159879**. The previous five integration commits were rebased onto that
main on `codex/shadps4-stage-profiler`, then the no-frame control API committed as
**0ef631cd7b0859aa0eea6beb0e8739bd43e75129**. Foundation and Litep's SDK checkouts now
use that same revision. This is a local SDK commit, not a published remote pin.
Earlier GPU clock metadata and orphan-query fixes remain included.

SDK `apply_mode_transition()` applies the existing route/resync protocol under the
cold mode mutex. It creates no FrameMark and does not claim frame ownership.
Foundation uses it at initialization and streaming transitions, exposing
`non_frame_capture=1`; paused-ring restoration remains intact. Wire layout and
SDK encoder/TLS hot paths are unchanged.

shadPS4 records cookie-paired Prepare/modules/bootstrap/constructor stages, a
Session generation bookmark and actual first prepared/presented game frames.
`VideoOut.PreparedGuestFlips` / `VideoOut.PresentedGuestFrames` are separate from
SDK FrameMark: existing FrameMarks come from **sceGnmSubmitDone**, including
no-flip warmup on another owner. They are not displayed FPS.

Cold recording can be armed with `debug.shadps4.profile_startup_seconds=1..600`
before ordinary APK launch: 512 MiB bounded file capture, at most one second cold
readiness wait only when explicitly enabled. The property is now cleared to 0.
This covers native host initialization onward, not ART/process creation.

Litep **0.2.0-local.20260917.stages1** is installed through the package manager.
New tools: `analyze_stage`, `find_progress_gaps`, `start_stage_analysis`,
`stage_analysis_status`, `cancel_stage_analysis`. Stage analysis accepts source
CPU-clock bounds or a complete region cookie, distinguishes startup/in_game,
clips nested scopes, keeps physical-thread coverage/counters and refuses to
silently cross recording/session boundaries. Large-file reduction uses an mmap
spool and bounded per-thread aggregates, without millions of expanded JSON/SQLite
rows. Raw source, hash, loss, unmatched endpoints and interior boundaries remain
visible. GPU elapsed queries remain supported separately; no uncalibrated CPU/GPU
join is introduced.

Codex registration now points at stages1; all six existing managed integrations
were checked and their custom arguments/environments preserved. All 11 regenerated
Doubao wrappers passed initialize/tools/list, including Litep's 80-tool catalog.
Fresh installed MCP stdio sessions performed the actual device pulls and all stage
analyses. The previously connected MCP instance in this conversation still exposes
an older catalog and needs reconnection to use the new tools directly.

## Real TMNT measurements

Device **9c2841a4**, AYN Thor / API33 / 4KiB / system Qualcomm Adreno740 driver.
Host/JNI RelWithDebInfo, FEXCore Release, playstoreDebug APK. Existing
`tmnt_frame_recompiled_v2` guest patch is active; no guest optimization was changed
by this delivery. No RenderDoc capture, GPU query recording or live debugger.

The first ordinary-APK run (PID3821, generation1) records **145.048 s**,
**49,659,373 events**, **400,275,336 bytes** compressed, zero skipped chunks and a
complete container/footer. Boundary-open scopes remain reported; this is not
proof of complete native CPU/scheduler coverage. SHA:
`08a2db7b4e8cbe14e7927844dfe4170c70344fec8f8d659cc4743b339cb6dd39`.
Screenshots show initial black, the menu, loading and rooftop by about 90 s.
Automation itself ends TIMEOUT_UNVERIFIED; it is not a new gameplay acceptance.

A second final-APK run (PID8165, generation1) validates the distinct real-present
markers with **15,727,985 events**, zero skipped chunks, in a 35 s capture.
`Startup.GuestEntry` is 0.818 s after recording begins and
`Startup.FirstGuestPresent` is 18.783 s. A successful present is not an automatic non-black-pixel classification; screenshots
remain the visual evidence. The exact **Session.Generation.1 →
FirstGuestPresent** interval is **18.744 s**. It contains 56 GNM source markers
(54 from a warmup worker, two from the main owner); calling the first of those a
rendered frame would have understated startup by roughly 16 s.

The loading analysis is an explicitly reviewed **31.989 s observation** from the
first run, bounded by screenshot/status evidence. It spans the loading plateau
and the recovery toward rooftop. It is not claimed as the exact semantic begin/end
of all game loading. It contains 108 GNM source markers: 98 on a background owner,
10 on the main owner. This is precisely why a frame-only analysis is misleading.

| Main guest owner scope | Startup to real first present | Reviewed loading interval |
|---|---:|---:|
| scePthreadCondWait | 3.830 s / 12,479 calls | 8.336 s / 347 calls |
| sceKernelUsleep | 2.161 s / 2,022 calls | 7.975 s / 7,390 calls |
| scePthreadMutexLock + Unlock | 3.526 s / 436,918 pairs | 4.460 s / 560,169 pairs |
| GNM.SubmissionGate | 0.051 s | 0.885 s |
| PrepareModulesAndServices | 0.514 s | outside observation |
| sceVideoOutOpen | 0.812 s | outside observation |
| sceKernelPread | 0.038 s / 1,735 calls | work also occurs on other owners |

These are **elapsed instrumented scopes**, including scheduling/lock waits, not
CPU samples or a causal critical path. The rows above are disjoint main-thread
scopes; do not add unrelated worker waits or nested full-runtime scopes. FEX
entry/exit and uninstrumented guest execution are not all included in HLE scopes.
Streaming/screenshot overhead was not A/B measured, so there is no speedup claim.

The data changes the optimization priority: main-owner synchronization and polling
are substantial in both phases. GPU submission-gate waiting is a small fraction
of this loading observation and almost absent before first presentation. This
alone does not exclude GPU work/dependencies elsewhere. Actual disk-read elapsed
calls are comparatively short in the measured owners; file caching or a module
loader rewrite is not supported as the first optimization by this evidence.

Next useful work is to correlate the main cond/usleep predicates with their
producer owners and guest semantic callers, then optimize polling/wake behavior
without dropping synchronization. The repeated mutex pairs justify advancing the
compiled guest mutex fast path with the existing ownership/cond/cancel protocol;
3.526/4.460 s are not guaranteed savings. Use the new stage endpoints for matched
A/B captures plus CPU samples, not menu FPS. No game mutex/audio/renderer policy
or scheduling delay was changed merely to improve this trace.

## Verification, artifacts and limits

- MCP 58 tests pass, including zero-frame stages, multi-frame/thread clipping,
  cookie routing, reset/generation refusal, malformed/truncated containers,
  SHA mismatch and cancellation preserving raw input.
- SDK focused `test_litetrace_route` plus native/Python smoke pass; runtime input
  fingerprint still matches after commit. OpenSpec validation passed separately.
- Foundation host existing ring/capture smoke and new no-frame stage smoke pass.
  Android no-frame ring/file/ring snapshots decode with expected regions, zero
  frames, no truncation/skipped chunks; pause restoration is verified.
- Host and JNI builds and ordinary APK deployment pass. No full game regression,
  GPU calibration, performance overhead acceptance or new playability claim.

[Shared workflow](../../../foundation/docs/guides/litep-stage-profiling.md) is in
Foundation. [Small evidence](litep-stages-20260917/) includes stage reports and final
binary identities. Complete raw files/screenshots and installed MCP logs remain at
`build/loading-profile-20260917/`; release provenance/wrapper results remain in
`~/workspace/spatial_mcp_publish/_out/litep-stages1/`.

Final APK SHA **a2e98d7d…** / host **1765d319…** / JNI **fa728c03…**; full values in
`litep-stages-20260917/final-artifacts.json`. Final device is PID8165/gen1, live menu,
ring ON, file capture finished, startup property0, automatic input OFF, no debugger.
Both SDK checkouts are clean on the local stage branch. Main/Foundation/MCP changes
remain local; all prior unrelated dirty work is preserved. No main/Foundation
commit/push or force-push of the earlier SDK branch was performed.
