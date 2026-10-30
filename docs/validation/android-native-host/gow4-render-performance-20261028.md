# GoW4 rendering and performance follow-up — 2026-10-28

Continues [startup fixes](gow4-startup-20261028.md). Odin3 `6edb0593`, CUSA07413
01.36, mainline Turnip `e61a373f` (Mesa `68dafb89a26`), render scale 75%, high
texture quality. User authorized reclaiming shadPS4 from BOTW and continuing device
validation. No user save data or game binaries were edited. The original game profile was backed up before the readback A/B below; only this game's GPU options were changed. Parent branch is
`feature/malos/swan_performance`, HEAD `22bd5a9d76fa32b726cf14831a00373795e200ac`;
changes remain local, including prior unrelated work.

## CPU snapshot optimization

Large read-only buffer bindings repeatedly visited every memory-tracker region,
locked it and prepared cleanup even when no CPU pages needed uploading. Added an
acquire/release summary of CPU-dirty pages per region. Read-only snapshots omit clean
regions; GPU-written bindings still mark all covered GPU ownership. Binding sizes,
image alias synchronization and actual upload contents remain unchanged.

Every retry rebuilds the selected region list, including after staging allocation
waits. After all selected locks are acquired, omitted clean regions are revalidated:
a CPU write while waiting on another region must force a retry. Rollback updates the
summary, and streamed CPU pages remain dirty. Runtime A/B: `upload_diag cpu_flag on|off`
(default on). No game or shader hash special case.

Odin production tracking tests: futex 14/0, concurrency 85/0, streaming 726/0.
Disabling clean-region revalidation gives 85 checks/2 failures; the earlier retry-list
negative control has 3 direct failures before a subsequent fixture assertion. Existing
region flags and ownership tests were retained. Android native host and F-Droid APK
builds passed. Clamp warnings are now bounded per shader (64 shader budget) without
altering the actual clamped ranges; suppressing Render logs alone did not remove the
baseline bottleneck.

## Same-session title menu A/B, candidate16

APK `34d14e16295e5cf2685cb2640652453cd5b9be25805baf1c33009b317136061c`,
host `753eb2f8390b1932f39b4f86c455701e419cda0a2aa64671158b5837fcce82d9`.
Installed base.apk SHA verified. PID13385/gen1; same stationary title menu, no input.
Ring enabled in both modes; fine scopes, streaming and GPU timing off. Diagnostic
image copies are outside these comparison windows.

| Phase | Successful-present intervals | Mean ms | Min ms | Max ms |
|---|---:|---:|---:|---:|
| on1 | 79 | 172.145 | 132.952 | 265.665 |
| off1 | 79 | 225.319 | 199.670 | 299.972 |
| on2 | 79 | 151.905 | 99.778 | 200.272 |
| off2 | 79 | 221.944 | 199.047 | 266.943 |

Combined on mean162.025 ms (6.17 FPS), off223.632 ms (4.47 FPS): about38% higher
present rate in this scene. **This scene was missing background geometry; these numbers are not a playable-frame-rate result.** Scene animation and system scheduling still vary; this
is not a whole-game FPS or fixed-CPU-clock benchmark. Four raw PROF files and exact
PID/dump sidecars are retained under local `build/validation/gow4-render-performance-20261028/final-*`.
Native indexing found 80 marks/79 complete intervals each, with no truncated payload,
skipped chunks or scope-pairing errors. Boundary-open spans and missing-source flows
remain explicitly excluded. Mark source is `videoout_present_success`, not GNM ticks.
Per-thread ring retention gives only short GpuComm coverage; whole-window CPU costs
must not be inferred from the sparse retained GpuComm tail.

Candidate15's earlier prototype timings are supplementary only: its on1 window was
still settling, off2 had nearby image diagnostics, and its missing final lock-wait
revalidation was subsequently fixed. The paused on3 dump was excluded entirely.
Candidate14's 8-second fine Streaming capture dropped7 chunks and is only supporting
hotspot evidence. Coarse GPU timing had estimated CPU alignment, not calibrated
scheduler/occupancy evidence.

## Rendering investigation and readback isolation

One title frame (candidate15) has52 submissions,86 IBs,23,626 packets,1,284 actions,
223 shaders and64 host render passes. Structural checks passed. Local PM4/guest-command
siblings share UUID `5b77845760f73e79050a6d1fc44effe9`, flips426–427.

- Physical RGBA8 image at `0x20c8b0000` contains detailed environment/character data.
  Its semantics are unconfirmed; the local filename `albedo` is not proof of albedo.
  The attachment uses LOAD, so retained content must not be assumed current.
- RGBA8 image at `0x21a750000` is mostly uniform outside Kratos.
- Linear depth `0x20bc00000` is12000 outside Kratos. Its resolve shader isCS `0x3e77df02`.
- Candidate16 adds bounded depth-aspect dumps for combined depth/stencil formats.
  Main depth at `0x20b090000`, D32S8 image UID68, also lacks environment depth:
  1,677,276/2,073,600 pixels are0; max0.0886667. This narrows investigation before
  depth linearization, but does not identify the faulty draw/transition yet.
- Disabling target/key/texture-binding memos did not remove the artifact.
  Full barriers after passes/around dispatches also retained it; diagnostic switch
  restored off. Neither trial is proof that every possible cache/barrier issue is absent.
- Trace text `hoist conflict: copy writes` also describes image layout barriers;
  it is NOT proof of a depth reupload. Bounded upload diagnostics observed0 uploads
  in the later title window. Do not carry forward the reupload hypothesis as a finding.

Candidate17 adds an explicit one-shot `gpu_images before <uid> <graphics-shader-hash>
[layer]` to inspect an image before the next matching draw; `cancel` clears a pending
match. Readback stays renderer-owned, <=32 MiB, restores the prior subresource layout,
and records the trigger hash. It changes timing and is excluded from performance
windows. Device validation and rendering follow-up are recorded below.


### Candidates17–19: GPU inputs and missing draws

The two candidate17 depth snapshots (before the first G-buffer draw and after that
pass) both lack environment geometry. The bad frame UUID
`4547c122260b7ef7421b4d5e3ac12109` has1,281 actions /224 shaders; its PM4 structural
check passes, with no CondExec or SetPredication packet. Shader `4c56d9f0` was
initially suspected to draw the world. This attribution was wrong: a one-shot
snapshot of the actual GPU descriptors finds a roughly0.057×0.074×0.255-unit object
near Kratos. Its GPU-captured model/projection inputs produce609/609 CPU-reference
vertices inside the clip volume, with extra depth-bias flag0. This is input evidence,
not a GPU vertex-output comparison. `ba9515f2` is a vertex shader, not a fragment shader.

Candidate18 logs bounded exact flattened constants with `cb_dump`. Candidate19 adds
`upload_diag cb_snapshot <shader-hash> [skip]`: one-shot actual descriptor bytes,
<=32 MiB, with shader/binding/range metadata and push constants. It copies on the GPU
and saves after fence retirement; it does not write guest memory or force Finish.
A matching GPU capture completed on Odin3. Status and allocation-failure handling
were subsequently added in candidate20. Image/buffer snapshots change timing and are
excluded from FPS windows. Pending requests have explicit cancel controls.

Candidate19 APK `2b089f44aea6263f8ac4c59dfa4fa1c8455800f45ed7453b50084d2551d90278`,
host `168f03ec6495d2691c6fd22cfa3f0751f252c251221e8f01d5a91c3f8184ce2b`,
installed and device APK SHA checked. Driver remains `e61a373f`.

### Readback settings restore the title background

- Disabled buffers + linear images off: missing/corrupt background. Changing only
  completion-fence timing did not fix it; the temporary fence option was restored.
- Precise buffers + linear images on, PID6407/gen1: trees, ground, background and
  character lighting return. `precise19-title.png` is the result; `precise19.png` is
  an earlier loading screen and must not be used as the result.
- Precise buffers + linear images off, PID7539/gen1: the complete scene remains.
- Relaxed buffers + linear images off, PID10968/gen1: the complete title scene also
  returns. JNI confirms mode1/linear0. The transient black/noise screenshot during
  this run was the device's displayed anti-burn-in refresh overlay, not an observed
  process crash; the same PID continued rendering the menu.

Therefore linear image readback is unnecessary for this title-menu correction.
Relaxed only synchronizes when CPU writes touch GPU-owned bytes; precise CPU-read
fault tracking is not required for this observed menu. Buffer ownership/readback
behavior is implicated, but the exact guest buffer/culling callsite is not established.
Do not claim a proven shader or precise-read-fault root cause.

The complete frame UUID `ce18103d44dffb2dce3a74fdfdd4f4c9`, flips424–425,
has52 submissions,86 IBs,46,842 packets,393 shaders,3,110 actions and21,142 host events.
Its PM4 structural check passes. It contains environment draws absent from the bad
frame. The independent cold sessions are not identical frame/pixel captures.

### Full-scene performance, candidate19

PID7539/gen1, Precise buffers / linear images off, successful-present markers,
no input, ring on, fine scopes / Streaming / GPU timing off. Each row has79 complete
intervals from80 markers. These are the complete rendered title scene, unlike
candidate16's missing-background comparison.

| Setting | Mean ms | Min ms | Max ms | FPS from mean |
|---|---:|---:|---:|---:|
| CPU summary on, stream off1 |279.327|133.236|400.028|3.58|
| CPU summary on, stream100 |239.664|133.069|332.991|4.17|
| CPU summary on, stream off2 |279.751|165.806|368.096|3.57|
| CPU summary off, stream off |359.499|333.058|466.805|2.78|
| CPU summary on again, stream off |274.684|166.321|399.932|3.64|

This confirms the clean-region summary's benefit with full geometry and a smaller
benefit from streaming submission. It is not a whole-game or fixed-clock benchmark.
Raw PROF files and sidecars are retained under `precise-only-off1`,
`precise-stream-on`, `precise-only-off2`, `precise-cpuflag-off`,
`precise-cpuflag-on2`. Indexes retain integrity and per-thread coverage boundaries.

The off2 GpuComm tail covers about0.89s, not the22.3s present window:
26 completed timeline waits total452.686ms;23 CPU-readback scopes total240.303ms
(includes waits);4,269 uploads total1,483.840MiB. Bind.DmaSync self elapsed is166.828ms.
These categories overlap; they must not be added as independent frame costs.
Fine capture from PID6407 has only191ms GpuComm coverage and is supplementary.
No scheduler/KGSL trace was acquired, so these are elapsed scopes, not on-CPU or
GPU-occupancy measurements.


Relaxed-mode cold run PID10968/gen1 also restores the complete menu: stream off
276.375ms /3.62FPS; stream100 236.715ms /4.22FPS (79 intervals each). No material
performance gain over Precise was established; do not generalize Relaxed as safe
through the game. Disabling write prediction in the same session reduces the upload
tail to162.867MiB /0.938s, but frame mean worsens to292.829ms /3.41FPS. Restoring
prediction recovers232.071ms /4.31FPS. Thus less upload traffic alone is insufficient;
the existing prediction remains enabled. The GPU fault counter read at20:03 was0.

Enabling prediction confidence decay gives239.242ms /4.18FPS, with no improvement
over the restored prediction baseline. Decay was restored off. The tests did not
change default prediction/streaming-page policy.

The same Relaxed/stream100 session proceeds through New Game → Balanced difficulty
into the axe tutorial with trees/background intact (`relaxed19-opening.png`). This
establishes opening-scene rendering, not traversal/combat/whole-game acceptance.


## Final candidate21 deployment

Android host and F-Droid APK builds passed. Installed Odin3 APK
`28a0197fe743769f53e0f97e885be754b2bc915e3c84f25c26925e3801a4a4ed`,
host `e379c11fae35d1900587b19ffb68ec73fe84cd64b7eb7099cacdb360adca8e29`.
Device base.apk SHA matches. PID16214/gen1, UUID
`1e8b08ee57619c343e496ab6a87de553`, same `e61a373f` driver. Candidate20 was built but
not installed;21 additionally serializes diagnostic arm/cancel/match-to-busy so a
new request cannot overwrite the skip count or race the active GPU copy. Five
arm/re-arm-rejection/cancel/status cycles pass20 checks on21; this is control smoke,
not exhaustive concurrency testing. `git diff --check` passes.

The per-game profile now explicitly keeps Precise buffer readbacks, linear image
readbacks off and Streaming Submit on. JNI mode2/linear0 and runtime stream100
confirm application after cold restart. The original profile bytes are saved in
local `gow4-profile-before-readback.json`; the global profile is unchanged. Precise
was retained because Relaxed brought no material FPS benefit and later scenes may
read GPU results directly. CPU summary and write prediction remain on; prediction
decay, fine scopes, GPU timing and Streaming capture are off. Completion fences
remain parse mode. `candidate21-title.png` again shows the complete title scene.

Raw source/index/sidecar artifacts are local under
`build/validation/gow4-render-performance-20261028/`; none is committed. Owned device
tracker probe executables were removed; the earlier1.8GiB GPU replay and user
captures/saves remain. No parent/dependency commit or push in this work.


Candidate21 also reaches Balanced difficulty → opening axe tutorial with complete
background (`candidate21-opening.png`, SHA47bdb66eab88b6dc2f115ea7a2c0b8185812a385f3a26c268e6d31c727b3a494).
The actual descriptor snapshot `buffers-77049445264615-4c56d9f0` completes8,530,952
bytes; binary/metadata/push files were pulled and all three device/local SHAs agree.
Status returns unarmed/not busy afterward. This diagnostic copy is outside performance
comparison windows. All injected buttons were released. Final native audio log has
nonzero peak0.226 and stall/error/reconnect/xrun0; historical starved800 is not zero,
and this does not establish long-session listening quality. GPU faultcount remains0.
The session is left Running at the tutorial, no debugger/ADB forwarding or active
image/buffer probe; the ring remains on, Streaming capture off. Capture-related
anti-burn-in overlays were excluded by taking the final screenshot directly from
the app render target.

Remaining work: characterize the exact GPU-byte ownership/CPU overwrite responsible
for the missing world, then reduce DMA synchronization traversal, upload traffic and
GPU drains with correctness-preserving changes. The current roughly4FPS scene is
not acceptable playability. No new claims about traversal, combat, saves, long runs,
other games, or desktop rendering are made.


Final candidate21 opening-prompt window:80 successful-present marks /79 complete
intervals, mean235.026586ms (4.255FPS), min166.357135ms, max300.186197ms.
PROF SHA `85050c2f4334a1fb262dce913cbd3bdcbfbc0fa2e8123446e60af9f7f1f84037`,
index `c2ea5415d7ce4307879f9ae5451bde24`, no skipped chunks, truncation or pairing errors.
This window follows all image/buffer copies and menu input; stationary axe prompt,
Precise buffers / linear off / stream100 / CPU summary on. It is not an opening-scene
A/B. GPU query records are intentionally absent (GPU timing off), and no scheduler
trace was attached.


## 2026-10-29 continuation: ordered dispatch and CPU directory summaries

The user requested further optimization. Candidate22 tested a single-workgroup GPU
loop for the already-supported single-wave ordered ADD/SWAP path, preserving guest
x/y/z IDs, TG_SIZE, fresh body register initialization and a workgroup memory barrier
between virtual groups. Odin GPU validation covered 12,739,824 checks / 0 failures
in both legacy and batch modes; reversed legacy dispatch order produced 2,122,368
failures. Six full SPIR-V modules passed Vulkan 1.3 validation. An initial invalid
selection-merge placement was caught in the isolated probe and fixed before installation.

Candidate22 APK `f9f2133e9b3c2cc907fbac3d37ed387e6f7f0207770f51964859cfcd2d250125`,
host `21e29a977d1ab033b3569bc5766fdc264309a4ae226344265585cb4434baffd5`:
same PID5794 full title menu measured off/on/off 264.978/267.094/266.256 ms.
There was **no measured gain**, so batching is off by default and at runtime.
These windows were separated in wall time and are not fixed-temperature/clock tests.
The earlier on1 240.507 ms window includes a different startup/settling phase and is
not part of this comparison. `watch_stream` on/off also had no meaningful gain
(262.451/261.606 ms); it was restored off. Each retained FPS window has 80 successful
present marks / 79 intervals, no skipped chunks, truncation or scope pairing errors.
The 3-second fine Streaming capture lost 15 chunks and is supporting hotspot evidence only.

Candidate23 adds one CPU-dirty bit per region in its existing 4 MiB directory. Read-only
snapshots can skip clean directory bits instead of visiting every 256 KiB leaf twice.
Only fully published managers are eligible; directory holes retain the creation path.
All selected locks are acquired before revalidating skipped directory/leaf summaries.
Retries rebuild the selection, and dirty/clean publication, rollback and streaming pages
update the directory summary. Written bindings still visit all regions. No upload,
binding range, image alias or GPU ownership is omitted. Runtime `upload_diag cpu_directory
on|off` is independent of the existing leaf summary; both default on.

Odin production-header regression: concurrency **221/0**, streaming **726/0**.
Disabling only directory revalidation yields **221 checks / 2 failures**, including a
CPU write to an omitted region while waiting on another region lock. Native host and
F-Droid builds pass. Tests and exact compiler argv are retained in
`build/validation/gow4-render-performance-20261028/tracker-directory-tests/`.

Candidate23 APK `70ac8f4703329315b54ec0b5d0abfdab566dc33416b37a0e0b9a036e6e86dbcf`,
host `9ff04d2418208269e70262639fcd419657cb62b46ae97e9201c5858f5a2acfe8` was installed,
and device base.apk SHA verified. PID4227/gen1, run UUID
`d1fa2fa72c098c71114ec8e40dd78ff6`; unchanged Turnip `e61a373f`. Full title menu,
Precise buffers / linear images off / stream100 / ordered_batch off / watch_stream off.
No game input during the four consecutive comparison phases. Ring on, fine/Streaming/GPU
timing off. Screen evidence `opt23-ready.png` retains the full environment and title menu.

| Directory summary | Complete present intervals | Mean ms | Min ms | Max ms |
|---|---:|---:|---:|---:|
| off1 |79|258.653|197.794|336.306|
| on1 |79|201.269|98.213|301.819|
| off2 |79|264.129|166.604|333.631|
| on2 |79|200.422|99.841|300.534|

Pooled mean261.391→200.845 ms, **3.826→4.979 FPS (+30.1%)** in this title scene.
Each raw PROF plus sidecar is retained under `opt23-{off1,on1,off2,on2}` with a disk
index: 80 marks /79 complete intervals, 0 skipped chunks/truncation/pairing errors.
This is not fixed-clock or whole-game evidence, and **about 5 FPS remains unplayable**.
Per-thread retention still prevents treating the whole present window as a full CPU trace.

A subsequent, separate GpuComm CPU-clock 997 Hz / FP sample (`opt23.perf`) recorded
4,575 samples, 0 lost, 3 broken callchains, 9.961 seconds elapsed and about4.589 seconds
sampled CPU time. UploadMemory accounts for48.15% including children; memcpy self17.20%,
and mprotect including children22.01%. Symbols from the recorded installed binary are
retained; kernel symbol addresses are restricted. These percentages are of the sampled
GpuComm CPU, not the whole process or frame. Readback scopes in the ring tail also show
serial GPU waits, motivating a bounded readback-window experiment; no gain from that
experiment is claimed here yet.

### Candidates24–26: bounded readback trial and diagnostic limitation

Candidate24 (`0785fa07`, host`7d4270d5`) adds a diagnostic readback window option,
64–16384KiB in powers of two, default512KiB. Ownership/arena clamping remains.
PID23569 consecutive title windows (79 valid present intervals each, zero data
loss/truncation/pairing errors) measured512KiB/4MiB/512KiB/16MiB/512KiB at
194.097/191.564/200.004/194.932/206.335ms. The16MiB phase also has an opened Detail
overlay and is not cleanly comparable. Larger windows reduce the observed readback
call count but increase copied bytes; no reliable FPS gain. Restored512KiB.

Enabling GPU timing detail caused SIGSEGV for PID23569 at14:16:30 and candidate25
PID29796 at14:24:19. No exact crash PC or GPU timing capture was retained. Native
Debugger startup failed its LLDB version preflight before creating a session; no
debugger/forward remained. The old crash-buffer record is unrelated and excluded.

A separate real Turnip capability issue was corrected: KGSL advertises calibrated
timestamps despite not implementing GPU-clock retrieval. Both EXT/KHR advertisements
now require a non-KGSL backend. Driver`24b53e0a` retains static C++ linkage and passes
the pure-C load/device/queue-fence probe; the old`e61a373f` advertises the extensions
and returns an unwritten zero GPU timestamp in the negative probe. This capability
fix **did not resolve the profiler SIGSEGV**. It is not evidence that ordinary
timestamp queries are unsupported. GPU timing remains off pending root-cause work.

Candidate25 APK`bef52ad5`/host`a330ee68` included that driver. Candidate26 adds the
user-requested flat StatusLayer Detail restoration and is installed with verified
APK SHA`96699bbb87c19a98374956f4bfb171264a49ecac065c2f000331e05fce64dcb0`, host
`cefc7874ee47f1094701d5f3f3fa4c64876dcc7f24f682cbdd93d9c2bb6bfd8b`. Native/APK builds
pass. Its full-title smoke window is210.129ms/4.759FPS, not a new optimization A/B.
The [TAA/pass/scaling investigation](gow4-passes-20261029.md) retains exact resource,
shader and consecutive-frame evidence. No parent/dependency commit or push.
