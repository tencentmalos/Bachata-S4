# Guest auto tag / reverse_study delivery — 2026-09-16

The first real shadPS4 loop is implemented and exercised: existing guest C/C++
frame calibration -> PROF batch -> study0 automatic plan -> FEX probes alongside
the C/C++ patch -> ordinary APK rooftop gameplay -> new PROF batch and a narrower
next-step suggestion. No new performance fix or full regression is claimed.

## Final policy and scope

- Public MCP `reverse_study`; default backend `study0`. `study1` is reserved and
  unavailable. Public tool prefixes, package/product, Codex entry and wrapper are
  renamed. Internal SDK namespaces/source directories remain implementation details.
- Default **two static call layers**, top three measured roots. `maxProbes=0`
  removes the former default 64-probe truncation. Positive budgets are optional.
  Uncapped capacity overflow (1024 probes / 64 expanded functions) is an explicit
  error, with no partial output. A later iteration can select one `targetOffsets`
  root and choose depth 1–4. The next request is not automatically deployed.
- This TMNT plan contains **134 probes / 237 sites**, 42 first-layer and 92
  second-layer probes, no rejected functions. Three original instruction sites
  follow existing C/C++ patch trampolines. Profile SHA is in the artifacts.
- x86-64 FEX runtime is verified. AArch64 static planner is verified on a real
  ELF fixture, including BL/BLR, depth and target overrides and 71 sites without a
  64-site truncation. Other ISA metadata is portable through the analyzer; its
  automatic probe generation remains explicitly unsupported until adapted.

See [operation and contracts](../../guest-auto-tag.md) and the installed
`guest-auto-tag` / `reverse-study` skills. No per-game function naming or C
prototype is needed for timing. Actual replacement still requires the existing
SDK's proven ABI, preimages and original trampoline.

## Device and evidence

Only AYN Thor `9c2841a4`, API33 / ARM64 / 4 KiB / selected Turnip was used. Swan
was not touched. Main/FEX/Foundation had substantial previous dirty work; it is
preserved. Host/JNI remain RelWithDebInfo and FEX Release. No commit/push or
Foundation change was made for this delivery.

The new APK runs in PID27493, process run UUID
`f8c0e2ef097990597e550538cf2f2887`. Unarmed Session generation1 and armed generation2
both reach the real Leonardo rooftop, with actual stick movement/camera response
and MOVE -> ATTACK. Generation2's CPU context is33; do not confuse it with Session
generation2. Both completed normal UI Stop (`Stopped/user_stop`).

The final bounded capture has 204 complete compiled-guest frame intervals, 165
above 33.333 ms, mean74.154 ms, p95 144.628 ms. It contains 392 decoded chunks,
zero skipped chunks and no unbound guest owner/profile. During the window the
runtime reports 318,969 completed automatic intervals and 2,072,541 begin/count
hits; zero overflow and zero stale-code invalidations. There are two discarded
in-flight intervals and 32,776 unpaired successor visits; these are not fabricated timings. A successor can also be entered by a
branch that did not execute the corresponding CALL.

Integrity is deliberately not called globally lossless: capture boundaries leave
18 orphan native ends and22 unfinished native spans; one GPU-context diagnostic
is retained. The reader marks boundary incompleteness and excludes unmatched
intervals. GPU overlap is available for only the subset with complete metadata;
it is not summed into CPU wall time. The raw capture, complete per-frame report,
immutable hashes and smaller archived summaries are listed in
[artifacts](guest-auto-tag-20260916/artifacts.json).

The preceding two historical captures were also processed through the actual
MCP: 711 complete frames / 571 long frames. A provisional historical gen2 context
was corrected to33 using same-session patch status before the final policy batch.
Its evidence does not merge with the new PID's owner stream.

## What the new automatic tags resolve

Across the165 long frames, the call at `eboot.bin+0x1545f69` to `+0x19630` accounts
for8752.465 ms of measured overlap; its existing compiled `submit` phase accounts
for8770.397 ms. Native `HLE.sceGnmSubmitAndFlipCommandBuffers` overlaps8746.93 ms
and `GNM.SubmissionGate`8707.04 ms on that same host owner. This materially narrows
the guest submit wrapper to its HLE/gate wait. It does not prove the downstream
GPU wait producer or a driver scheduling root cause.

The next measured targets are `+0x19630`, `+0x15c0080`, and `+0x24c60`, respectively
from submit, jobs/semaphore and frame-begin waits. They are recorded in the next
request; no deeper profile has been deployed. Investigate these measured boundaries
before expanding unrelated subsystems or assigning semantic names to unknown code.

## Observer overhead

Same armed generation, automatic input stopped, ring/GPU queries on, 15 seconds
per phase, no debugger and no file capture during the A/B/A comparison:

| Recording | Guest FPS | Process CPU cores (all threads) |
| --- | ---: | ---: |
| OFF A1 |13.365|1.806|
| ON B |13.502|1.824|
| OFF A2 |13.381|1.812|

The ON window sees approximately132.9k begin/count hits and20.4k completed pairs
per second. This short GPU-constrained scene shows no detectable FPS regression;
it is not proof of zero CPU overhead or a universal acceptable probe budget.
Installed-but-disabled retains generated callback cost. The preceding separate
unarmed generation measured13.115 FPS, with a different camera position, so do
not interpret that cross-session difference as a speedup. The restored unarmed generation later measured13.373 FPS and1.797 process CPU
cores over15 seconds. These short windows do not establish a portable overhead
percentage; restore details are recorded below.

## Focused validation and repairs

- Real FEX native `guest_patch_tests --auto-only`: **48/48**. Exact identity/ISA/
  preimage and range negatives; immutable install; integer/stack/vector/sret;
  relocated CALL pairing; recursive/nested owner retirement; epoch toggle;
  compiled patch ON/OFF; code publication changes arithmetic correctly while
  stale profiling disables and refuses rearm. No default probe table emits calls.
- Portable batch tests: **15/15**, including owner/invocation separation, recording
  and transport gaps, interval union, SDK/auto frame-address collision, duplicate
  raw-capture rejection, multi-ISA metadata and bounded decompression.
- Android helper tests: **3/3**, including empty `Client:` line parsing and exact
  deployed C/C++ package SHA/hook/counter binding. A first real capture failed its
  final package check because the multiline parser consumed the first field;
  fixed and recaptured, with the failed attempt retained and not marked PASS.
- Real study0 architecture/override/refusal checks: **11 checks**. Unsupported
  mode, wrong SHA, wrong ABI and study1 all refuse explicitly.
- Local reverse-study release installed through the package manager:
  `0.3.0-local.20260916.autotag6`. All **11** generated wrappers pass initialize and
  tools/list; reverse_study exposes48 tools including the three new profiling
  tools. Six existing Spatial registrations preserve arguments/environment and
  resolve their current installations. `codex mcp list` verifies the renamed entry.
  Existing already-connected clients may need reconnection to refresh tool names.

Earlier fixture failures (missing ring activation, incorrect register index and
missing all-instruction FEX markers) are preserved in build artifacts. The final
JIT callback uses the correct guest RSP register and emits instruction markers
only for installed probes; guest state preservation is tested on the actual FEX
backend. No unrelated game/HLE/audio/graphics rewrite was included.

## Final cleanup and restore

The startup profile property is cleared; Session generation3 reports
`disabled_at_startup`, so it has no installed auto-probe table or generated probe
callbacks. A supplemental visual review confirms real rooftop movement/camera
response, dialogue and ATTACK after restore. The restore helper windows timed
out without an in-loop review; their original TIMEOUT_UNVERIFIED results remain
unchanged and are not counted as additional automated passes. Generation1 and2
are the accepted in-loop gameplay validations above.

File capture2 is finalized/inactive, automatic input has ended, TracerPid=0.
The game is left running, with its existing guest C/C++ calibration, ring and GPU
queries retained. The next automatically suggested profile has not been deployed.
Owned study workspaces and smoke-test MCP processes are closed. This conversation's
old connected tool catalog does not hot-reload; new clients use `reverse_study`.
