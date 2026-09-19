# Guest auto tag and iterative long-frame analysis

The public MCP is `reverse_study`, with `study0` as its default backend. `study1`
is reserved and currently returns `BACKEND_UNAVAILABLE`.

This workflow generates profiling sites from measured offsets and the exact
analysis image. It does not require a hand-written wrapper, semantic function
name or C prototype for every timing interval. Replacing a function with guest
C/C++ still requires a proven prototype and ABI.

## One iteration at a time

The default is **two static call layers**, initially from at most three measured
hot functions. The selected function is layer 1: its calls are timed from CALL
through the immediate successor. Layer 2 inspects verified direct callees and
times calls inside them. This is a static expansion depth, not the runtime stack
depth. Recursion and concurrent owners still require independent runtime pairing.

There is **no default 64-probe truncation**. `maxProbes=0` includes all verified
sites within the selected depth. An explicit positive `maxProbes` requests a
partial budget, shared across functions, with coverage recorded in the profile.
The present FEX backend can install at most 1024 probes / 2048 instruction sites;
the planner can expand at most 64 functions and bounds individual function scans.
An uncapped plan exceeding capacity returns an error without publishing a partial
profile. Narrow roots or split captures rather than silently omitting callsites.

After inspecting one capture, select the hot function with `targetOffsets` and
use it as the next root. Depth 3–4 is an explicit option. A batch produces a
suggestion; it never deploys or recursively executes subsequent plans. Probe
count alone does not predict overhead: also inspect hits per second, event rate,
recording gaps and enabled/disabled scene comparisons.

## Tools and deployment

1. Capture a real gameplay scene using the existing PROF capture endpoint. Keep
   PID, Session UUID/generation, CPU context, module/container SHA, analysis-image
   SHA and preferred imagebase. Existing compiled guest frame/phase tags can seed
   the first iteration; otherwise supply measured roots and use guest-submit cadence.
2. `reverse_study.guest_profile_batch(captureManifests=[...], outputDirectory=...)`
   produces per-capture long-frame reports and `next-probes.request.json`.
3. Open the exact analysis image with `reverse_study.workspace_open`, read schema,
   then call `reverse_study.guest_auto_tag_plan(workspaceId=..., requestPath=...,
   outputDirectory=...)`. Optional `depth`, `maxProbes`, `targetOffsets` override
   this iteration. SHA/base/mode mismatches are refused.
4. `scripts/android/guest-auto-tag SERIAL deploy PROFILE` copies and hashes the
   profile, then sets `debug.shadps4.guest_auto_tag` for the next Session. Start
   through `scripts/android/open-last-game SERIAL`, reach and verify the scene.
5. `scripts/android/guest-auto-tag SERIAL status` reports context-bound hit and
   completion counters. `enable`/`disable` affect the installed table; `clear`
   removes the startup property for future Sessions.
6. Capture with the command below, then batch the emitted `capture.json`.

```sh
scripts/android/guest-auto-tag SERIAL capture \
  --output FRESH_DIR --seconds 15 --profile PROFILE \
  --analysis EXACT_ANALYSIS_IMAGE --imagebase 0 \
  --recipe guest/games/CUSA50828/01.08/rooftop.recipe.json
```

Use imagebase zero only when verified for the input. `--recipe` is optional and
binds the existing compiled guest SDK frame/phase counters after checking the
active patch package and CPU context. The capture command checks Session
continuity, refuses native-debugger attachment and existing active capture,
checks the pulled PROF hash, and retains before/after status.

## Runtime design and limits

`GuestCpu::ExecutionProbes` is installed once before the first owner. The FEX
adapter inserts `GuestProfileProbe` IR at exact guest instruction markers, with
architectural registers/flags/FP state preserved around the host observer. It
neither patches guest memory nor uses an HLE syscall. Each physical Run has a
bounded logical-owner/invocation stack; x86-64 pairing uses probe ID plus caller
RSP. Names encode profile digest, context, guest thread, generation and invocation.

No installed profile emits no probe callbacks. An installed but disabled table
still executes callbacks, so it is not the unarmed performance baseline. Active
recording costs native observer transitions and two PROF counters per completed
call. Default game/debugger behavior should be measured separately from this mode.

Exact executable bytes and mapping identity are checked before insertion. Code
changes disable stale profiling rather than reusing obsolete attribution. Startup
C/C++ patches provide relocation provenance: probes of stolen instructions follow
the original trampoline; surrounding call pairs include the selected replacement.
`tools/guest-functions/build.py` and the versioned guest SDK remain the compiler
and ABI boundary. Auto tag does not invent signatures or automatically rewrite
hot functions.

The batch reader preserves wire ordering and logical ownership, discards open
pairs across transport gaps/recording boundaries, and clips/merges overlapping
intervals per frame. Guest elapsed time includes native HLE and descheduling.
Host overlap is not on-CPU time; GPU overlap is separate and not added to CPU time.
Incomplete/non-returning calls remain diagnostic counters rather than durations.
Indirect callees without a unique static target are timed but are not assigned a
guessed next function. Coverage is limited to installed verified sites.

## Portable workflow

The shared module, MCP and `guest-auto-tag` skill live under
`spatial_mcp_publish/dev_tools/mcp`. The PROF analyzer is independent of guest ISA.
The static planner implements x86-64 SysV and AArch64 AAPCS64 little-endian calls.
The shadPS4 runtime adapter is x86-64-on-ARM64 FEX. AArch64 static validation does
not mean an AArch64 runtime adapter has been added here. ARM/Thumb, x86-32,
PowerPC, MIPS and RISC-V require explicit probe backends; analysis artifacts may
carry those architectures, but unsupported probe generation is refused.

Use the packaged `guest-auto-tag` and `reverse-study` skills. No per-game naming
or prototype work is required for automatic timing; reconstructed symbols and
proven ABI information are reused when a later C/C++ replacement is warranted.

## 从标定到 C/C++ 重编译

见 [选择性重编译](guest-selective-recompilation.md)：新 `reverse_study.guest_recompile_bundle` 输出带 code/data 依赖的函数证据，guest builder 验证行为契约并绑定原 guest 函数/数据。用原机器码做隔离差分，然后在实际游戏分时开关核验。`--compiled-only --recipe` 可在未装 auto probes 的 Session 捕获 C/C++ phase；recipe 必须给出精确 analysis SHA/module/imagebase。
