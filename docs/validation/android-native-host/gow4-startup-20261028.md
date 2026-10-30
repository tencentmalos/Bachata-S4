# GoW 2018 Android startup — 2026-10-28

## Result and boundary

Odin3 `6edb0593`, CUSA07413 /01.36, EmuPSP direct read (10 components,
payload_copied=0). **Startup black output is resolved through the opening axe
interaction:** candidate13 reached brightness setup, logo, menu, difficulty choice
and the opening scene; Cross and R1 were accepted and changed the game state.
Candidate14 APK `734b6229` / host `0257b488` is installed with device SHA verified.
It adds a bounded single-sample CMASK read fix after those gameplay checks.

Background rendering still has missing/blocky areas and performance is low; this
is not a full-playability or visual-correctness acceptance. Both75%/100% replay
comparisons retain missing background. See the final sections for tests, exact
artifacts and the failed CMASK-as-background-root-cause hypothesis. No parent
commit/push; preexisting worktree changes are preserved.

## Provenance

Local evidence: `build/validation/gow4-startup-20261028/` (binary/firmware/game artifacts excluded from Git).

- Original APK: `607e97614cf556ca157dd21f2b3b3ee8dfa4b7ed3c0f2a30c28ce5e580652143`.
- GoW4 validation APK: `822af203be4a1af8ee1b797068ac9543d3b428054b206b7d1dbcd422412acda8`.
- GoW4 validation embedded host: `725a67cc6864e51448a03265156bf41c59c91bb0481068c7f8ec588ce2d2f3c0`.
- SELF: `19e9f05764de160ab8560b18dabc9a113e818f9e4aaaf687dd04265a62bf2c54`.
- Converted ELF: `90880b52303a2e9190c80595c16116b4e495f770f7cccd6f5e757147cb4a1c5f` (runtime base 0x400000).
- Current APK includes previously uninstalled static-STL Turnip `e61a373f` / Mesa `68dafb89a26`, and previous presentation-profiler work. It is not an isolated driver A/B.
- `executables/inventory.jsonl`: 20,352 archive paths; manifest/export hashes checked. libc.prx and libSceFios2.prx are actual guest modules; no libSceJobManager.prx is present.

## Changes and observed transitions

1. **ShareUtility / PlayGoDialog / PRT families:** follow desktop flow, with checked guest pointers and unavailable-provider errors. Advanced ShareUtility validation was checked against local FW11 disassembly. Do not fabricate sharing success. PlayGoDialog preserves desktop installed-content completion semantics and validates structures. Set/GetPrtAperture state belongs to the MemoryManager session, including setup before the renderer exists. Page-table-statistics query remains explicitly unavailable and does not write output.
2. **Debug log arena:** original MOVABS R14 at RVA0xcd70b requests fixed `0xfaaaa00000` (about 1003 GiB). Odin3 only admits 39-bit userspace. Standalone mapping probes: 0x4000410000 and 0x7000410000 succeed, 0x8000410000 and the original address fail ENOMEM. A trial 1 TiB backend expansion was rejected/reverted; FEX policy remains 512 GiB. Exact-version package [log_arena.recipe.json](../../../guest/games/CUSA07413/01.36/log_arena.recipe.json) changes only this immediate to `0xfaaaa0000`; the game performs its own real allocation and mapping. SELF and preimage are bound. This is a game-specific address-layout workaround, not support for all PS4 virtual addresses. Installed package SHA `7a17a841ecac6a653b06deeb4f0b614649d12fb8e335e0c9526b1e539971c34d`; selected in Android profile `emupsp-CUSA07413`, no global debug override needed.
3. **NP local/offline family:** thread tuning, timeout, TSS GetData/Async pointer/option validation, DOB and country query admission including aliases. Preserve offline/not-logged-in/no-provider semantics and output untouched on errors. Candidate4 passed this stage.
4. **PKG direct-read paths:** game requests `/app0/exec/wad/ORBIS_LE/R_System.wad`; actual entry is `exec/wad/orbis_le/r_system.wad` (3,127,280 bytes). Desktop loose-file lookup already has a case-insensitive fallback; PKG reader lacked it. Add an immutable ASCII-folded index, exact match first, ambiguous case-only fallback refused. Update overlay precedence and read-only ownership remain. Candidate5 passed resource open and began graphics/audio initialization.
5. **Resource VA range:** candidate5 then had 28 BatchMap EINVALs and Guest-13/FEXMemJIT SIGSEGV at `0x4e26000000` (about 313 GiB). Production ownership ended at 288 GiB. Expand the segmented envelope to 384 GiB, retaining the upper 128 GiB of 39-bit userspace for host allocations. Existing host-map exclusion and non-overwriting reservations remain. Add exact failing-address real BatchMap/read/write/unmap regression; old boundary fails, new boundary passes. Log failed batch entry operation/address/size/offset. Candidate6 passes the crash boundary. This bounded envelope still does not admit arbitrary addresses up to the game's advertised PRT end (0x8600000000).

## Targeted verification

| Check | Device result |
|---|---:|
| Share/capture service validation | 173 / 0 |
| PlayGo and dialog | 235 / 0 |
| NP offline/validation | 1020 / 0 |
| PKG reader, AES, descriptor lifetime, case and overlay | 80 / 0 |
| Memory manager, PRT and high-address batch mapping | 217 / 0 |
| Old 288 GiB boundary negative control | 212 checks / 1 expected failure |

First new high-address test lacked initialized SDK metadata and asserted in CurrentSdkVersion; fixture initialization was corrected before the 217/0 run. Earlier failed NP and decoder cases are retained in local evidence, not counted as passing runs. Android host and full F-Droid APK builds passed.

## Remaining black output

- Final session remained active for minutes with advancing guest submissions, flips and host presents. No further named unsupported import or resource BatchMap failure observed in this window.
- Direct game screenshot is black. GPU image uid1 (1440×810 A2B10G10R10) readback contains 1,166,400 zero words; its identity is not claimed to be the exact same output as the earlier screenshot.
- Two complete command intervals saved as `3f857d9d8d5be0d0a35794a6ffc3fe6a.{gpu.pm4.trace,gcmdtrace.ps4}`; flips2489..2491, 92 submits, 152 IBs, 6042 packets, 166 actions, 482 host events, structural check clean. Each interval has66 direct compute+13 indirect compute+4 graphics draws. Instrumented interval durations129.4/4.1ms are not ordinary-frame performance measurements.
- The last passes are post-processing and a six-index draw. Guest watchdog shows progressing main/job/audio threads rather than one universally blocked guest. This does not prove resource completion.
- Read-only `dbg::GameLog` arena shows WAD_R_Perm, WAD_R_Lang, WAD_R_UI, WAD_R_HeroA00 connecting, then WAD_R_Son00 loaded at marker62. No later progress entry was present. Resource-load completion is the next investigation target, not a proven root cause.
- Guest PCM raw peaks remain zero; Oboe callback progress continues, stalls/errors/reconnects0. No claim of working game sound.
- One bounded live `dma_bounds off` comparison still produced black; restored on. No proof of a generic synchronization fix.
- Trophy extraction reports missing key and XML; not newly bypassed or claimed fixed here. The user-supplied encrypted firmware/game files were not published.

## Final binding audit

### Follow-up on shared-IME build (candidate 8)

APK `927928e63db7d75f8bf7c6ffd72ddf7748f3784259ae93c461c3efedb3315d08`, host `82017e63309488b1348fc26de0bb09ee4f43cf600feba87fd70baeaf3d0bb470`, installed with device APK SHA verified. PID30469/gen1/run `65904ce9b602f7d17c009cf76b026c82` still presented black. The main thread advanced through frame completion; the resource worker at RVA0x36ae40 waited on its empty queue, and the readback worker at RVA0x2cbe90 had no pending request. The WAD loader state and pending phase were zero. These snapshots rule out a blocked resource operation at those instants, not every possible loading defect.

The actual frame delta at RVA0x104ce70 was approximately 0.032 s. RVA0x20de250 is a fade coefficient, not the clock; its zero value is not evidence of a stopped timer. Five bounded script-command entry probes were not hit and were removed; those entries belong to registered script commands and do not prove ordinary game scripting is inactive. No CPU/GPU memory values were patched. The target advertises no frame-chain provider, so no aligned CPU/GPU/screenshot causal manifest is claimed.

**PlayGo lead:** game globals contained active/enabled bytes1/1, handle0,20 content chunks and all-zero locus bytes. Static startup at RVA0xce5aa ignores `scePlayGoInitialize`'s result; the call at RVA0xce7ff similarly sets the active byte after `scePlayGoOpen` regardless of success. The bridge's scratch-buffer check required one containing VM mapping, rejecting otherwise valid adjacent mappings. A production-header Odin3 regression reproduced this: old248 checks/2 failures (initialize/open), fixed248/0, including denied read-only segment, unmapped gap and overflow. Fix opts only this byte buffer into the existing atomic multi-mapping admission; no pin crosses PlayGo metadata I/O. Direct capture of the game's initializer return code was not obtained: initial-wait MCP attach reported stopped epoch0 and could not mutate; LLDB default software-breakpoint requests were rejected by the provider's x86 length contract. Both debugger clients were cleaned up, and the second test session was stopped normally. In-game validation of this fix follows below.

### PlayGo completion and resource-tail follow-up (candidates9–10)

Candidate9 APK `d5f7794706a60009c16792561a3e6381f1bf7324831f1c5a1467b1271c428549`, host `bad8c37c141c50145a45f9c597abb9243bf6502f63fb4af1fa70c68b1970972b`, was installed with device SHA verification. PID9668/gen1 and gen2 reached combat-HUD Lua initialization after successful PlayGo initialization. Both then reported `combathud.lua:466: Failed to find meter Blood`, through `MeterGetMax`, `GetRageSizePercent`, `UpdateRageMeterSize`, `setup` and UI FSM initialization. The guest deliberately traps with `int0x41` at main RVA0x267a4a; no new native crash tombstone is claimed.

Gen3/run`acfadc58fcfe6eb12a474b17fe68882a` was stopped at RVA0x629bc1 before a Health lookup. Static `MeterGetMax` walks actor+0x958 and compares each definition hash. Read-only evidence (`meter-probe.json`, `dcb-tail-probe.json`) shows (decode register `rawHex` as little-endian; this session accidentally retained the client big-endian default, so its `numericHex` fields must not be used):

- actor`0x1e86b6a3d0`,11 linked nodes, valid next/previous/owner links;
- Health definition`0x1e802a1920`, hash`0x9b19dbe095711f08`, plausible limits;
- other10 definitions point to slots`0x1e8166d450..0x1e8166d498`, each containing its own address, with zero data around them;
- PlayGo handle at`0x2271aac` is1; all20 locus bytes at`0x2271ac0` are3.

The DCB is `exec/dc/orbis_le/wad_r_heroa.dcb`,22,472,240bytes, SHA256`4b291989754dd6d1f233c31c2485e04322ba443b81f9d569a0d437e9d017f97a`, read directly from the EmuPSP base package without altering it. Matching the inline hero definition establishes the loaded bias`0x1e802a0c30`. The ten-slot array is file offset`0x13cc820`; first relative offset0x50 leads to`0x13cc870`, which contains Blood hash`0x010dbf22d67c1449` and its real definition. The observed self-pointer is consistent with relocating a zero relative offset; the pre-relocation value itself was not captured. This is evidence of missing loaded tail data, not a missing source definition. The exact original game read request/return was not captured; attribution to the bridge limit must still be confirmed by the next game run.

`guest_storage_hle.cpp` capped Read/Pread/Preadv at16MiB before passing them to the backend. The desktop forwards the full regular-file request. Candidate10 preserves the full read size, validates every output mapping before I/O, and keeps backend EOF/error/cursor/descriptor ownership behavior. Bounded write admissions are unchanged. `LargeFileReads` tests18MiB+257bytes through plain files and ZArchive, adjacent mappings, positioned/vector reads, EOF and denied tails. Odin3 original host897/18 failures; corrected host897/0. Whole host and F-Droid APK build pass. Candidate10 was installed in the background after stopping the owned shadPS4 test process, with device APK SHA verified (`candidate-10-install.txt`, `candidate-10-device-sha.txt`); no game launch followed. Candidate10 APK`b48daa5413cc172ca96e822011adf8e5761ff9dc8f0186f9474545cb16ba66ba`, host`c2a53e5748456f42b06be41f41c7cb38700102fe342f7493b3f7519bdedd6bcf`.

No guest data/instruction patches were used to bypass the meter assertion. The guest debugger was stopped with ownershipReleased=true and its breakpoint/forward removed. Gen3 was found backgrounded while BOTW was foreground; no further foreground switching was attempted. After background DebugBus queries timed out, the owned shadPS4 test process was stopped; debugger properties reset. A foreground test slot was requested from the user. Game/menu, audio and in-game Chinese-panel visual acceptance remain outstanding.

`guest-imports-final.json`: 496 bridges; 43 named refusals (initial audit52). Refusals remain deliberate, not fake successes. JobManager implementation/provider is absent; GNM alternatives/debug queries, VideoOut alternatives, kernel signal/unwind/cancellation and Coredump paths remain unimplemented. None was called in the final observed startup window, which is narrower than full gameplay coverage.

| Library | NID | Function |
|---|---|---|
| libSceCoredump | `+YX0z-GUSNw` | `sceCoredumpAttachMemoryRegion` |
| libkernel | `-YTW+qXc3CQ` | `sceKernelInternalMemoryGetModuleSegmentInfo` |
| libSceJobManager | `4g1ee34TMH8` | `_ZN3sce3Job10JobManagerD1Ev` |
| libSceGnmDriver | `6YRHhh5mHCs` | `sceGnmRequestFlipAndSubmitDoneForWorkload` |
| libkernel | `8OnWXlgQlvo` | `_sceKernelRtldThreadAtexitDecrement` |
| libSceGnmDriver | `EJapNl2+pgU` | `sceGnmComputeWaitSemaphore` |
| libkernel | `Fjc4-n1+y2g` | `__elf_phdr_match_addr` |
| libSceJobManager | `GS9DSbzC+MY` | `_ZN3sce3Job10JobManager10initializeEPvmjjjmPKNS1_12ConfigParamsE` |
| libSceVideoOut | `IOdgHlCGU-k` | `sceVideoOutSubmitChangeBufferAttribute` |
| libkernel | `IafI2PxcPnQ` | `scePthreadMutexTimedlock` |
| libSceGnmDriver | `Kl0Z3LH07QI` | `sceGnmLogicalTcaUnitToPhysical` |
| libSceGnmDriver | `KnldROUkWJY` | `sceGnmGetNumTcaUnits` |
| libSceVideoOut | `MTxxrOCeSig` | `sceVideoOutSetWindowModeMargins` |
| libSceJobManager | `Nm2BhAwYNFk` | `_ZN3sce3Job10JobManager27calculateRequiredMemorySizeEPKNS1_21MemorySizeQueryParamsE` |
| libkernel | `Tz4RNUCBbGI` | `_sceKernelRtldThreadAtexitIncrement` |
| libSceGnmDriver | `URDgJcXhQOs` | `sceGnmInsertThreadTraceMarker` |
| libSceGnmDriver | `UtObDRQiGbs` | `sceGnmDestroyWorkloadStream` |
| libkernel | `VADc3MNQ3cM` | `signal` |
| libSceJobManager | `VTKezfYu-v4` | `_ZN3sce3Job10JobManager24setPersistentThreadCountEj` |
| libkernel | `VkTAsrZDcJ0` | `sigfillset` |
| libSceJobManager | `W-yXgQmaozU` | `_ZN3sce3Job10JobManager27getSequenceFactoryInterfaceENS1_8PriorityE` |
| libkernel | `WlyEA-sLDf0` | `sceKernelTruncate` |
| libSceGnmDriver | `XiyzNZ9J4nQ` | `sceGnmSetWaveLimitMultipliers` |
| libkernel | `aPcyptbOiZs` | `sigprocmask` |
| libSceGnmDriver | `cUCo8OvArrw` | `sceGnmDrawIndirectCountMulti` |
| libSceJobManager | `chxnvsxxfBY` | `_ZN3sce3Job10JobManager8shutdownEv` |
| libSceJobManager | `cwMzzdesLlY` | `_ZN3sce3Job10JobManagerC1Ev` |
| libSceGnmDriver | `dewXw5roLs0` | `sceGnmGetPhysicalCounterFromVirtualized` |
| libSceGnmDriver | `f33OrruQYbM` | `sceRazorIsLoaded` |
| libSceGnmDriver | `fzJdEihTFV4` | `sceGnmGetProtectionFaultTimeStamp` |
| libkernel | `hHlZQUnlxSM` | `getrusage` |
| libSceGnmDriver | `iFirFzgYsvw` | `sceGnmGetLastWaitedAddress` |
| libSceGnmDriver | `jHdPvIzlpKc` | `sceGnmDrawIndexMultiInstanced` |
| libSceGnmDriver | `k7iGTvDQPLQ` | `sceGnmGetShaderStatus` |
| libkernel | `kbw4UHHSYy0` | `__pthread_cxa_finalize` |
| libkernel | `mo0bFmWppIw` | `sigreturn` |
| libSceGnmDriver | `nEyFbYUloIM` | `sceGnmGetShaderProgramBaseAddress` |
| libSceGnmDriver | `oL4hGI1PMpw` | `sceGnmGetGpuBlockStatus` |
| libkernel | `qBDmpCyGssE` | `scePthreadCancel` |
| libSceGnmDriver | `stDSYW2SBVM` | `sceGnmDrawOpaqueAuto` |
| libSceGnmDriver | `tZCSL5ulnB4` | `sceGnmGetGpuInfoStatus` |
| libkernel | `tZY4+SZNFhA` | `msync` |
| libSceGnmDriver | `u9YKpRRHe-M` | `sceRazorCaptureImmediate` |

## Final installation and cleanup

Final UI revision APK `c73dda7a70db758973e803fc85a908080a33de5cc9535a4e634c0e89ac409d78`, host `5344f4e8c1a9340013caf2b44dc0e6e95f1bc4f2729077721fb80e3398309503`, installed with base.apk SHA verified. GoW4 was stopped through its normal UI; service disappeared. The earlier shell service-stop attempt was rejected by the non-exported service and had no effect. No active debugger/ADB forwards/global guest_patch override. Selected exact-version GoW4 package remains in its game profile. Five owned capture files verified against device SHA before removing remote copies; local captures retained. Owned native-test directory and verified executable export removed. Original unrelated diagnostics/screenshots preserved.

Follow-up cleanup: owned device probe directory and UI XML removed; no shadPS4 PID or ADB forward remained. Reverse Study workspace closed. Local raw evidence retained; no automatic continuation or device monitoring is running.

## Authorized foreground follow-up: candidates10–12

The user authorized taking Odin3 back from BOTW on 2026-10-28. Candidate10 ran twice
(PID7617 and PID8254). Both passed the former PlayGo/resource-tail point without a
`meter Blood` report, then aborted translating compute shader`0x38c7b95b` at
`DS_ORDERED_COUNT`. This is a later blocker, not yet full gameplay acceptance or a
fresh read-only inspection of the Blood pointer table.

The original20,964-byte shader has ADD at0x2fa0 (`offset1=3`) and SWAP at0x2fd0
(`offset1=0x13`), using GDS bases0/4/8 and the ordered term from`TG_SIZE[16:6]`.
The renderer now admits compute single-wave groups (1..64 threads, host subgroup64,
GDS, ordered index0, release+done ADD/SWAP). It issues groups in x/y/z dispatch order
using`dispatchBase` and compute memory barriers; it does not rely on device-wide
spinlocks or silently proceed out of order after a timeout. Multiwave groups, pixel
ordered append and other mode/index encodings remain explicitly unsupported.
TG_SIZE wave count, wave index, first-wave bit and 11-bit ordered term are populated.
The original dispatch dimensions are retained in otherwise unused compute viewport
push words. Indirect counts are synchronized through the buffer-cache readback path
before binding. This conservative path has not been performance-optimized.

The [KytyPS5 ordered-append investigation](https://github.com/KytyPS5/KytyPS5/pull/1199)
was used as a semantic cross-check: unordered add/swap can mix particle-emitter prefix
sums. Its bounded cross-workgroup spin implementation was not adopted.

`android_ordered_count_probe` runs actual GCN ADD/SWAP encodings through the production
translator, GDS resource patching and SPIR-V emitter, then the production Turnip loader
on Odin3. Twelve GPU cases: full/lane39/empty EXEC, ADD/SWAP,2052 groups as1D and19×9×12.
Every destination lane, first-active source selection, nonzero GDS base, neighbouring
GDS words, TG_SIZE and order past2048 are checked: **6,303,804 checks/0 failures**.
Deterministically reversing group order is the negative control: same checks/**525,824
failures**. All six emitted modules pass`spirv-val --target-env vulkan1.3`.
The first probe invocation had an incorrectly named driver file and did not run GPU
work; it is not counted as a passing run.

Candidate11 APK`63f56764db5f14f857ca7d3de2d35bbca3037b14e6a12d865b90c964cc71b8a1`,
host`03709f6423ee2400b9574f24148dad99c01a1cdfa7f3e020b8e120070265dbbb`, was installed
and device APK SHA matched. PID12705/gen1 passed ordered-count translation and then
stopped on fragment shader`0x572ce611`: `Unsupported resource descriptor word1:
BitwiseAnd32`. The400-byte shader clears NUM_FORMAT via`S_AND_B32 s1,s1,0xc3ffffff`
before an r128 sample. Candidate12 preserves a constant AND on texture descriptor
word1 as a view operation, applies it on every fetch, and includes it in descriptor
identity. It composes with existing image post-operations; arbitrary dynamic masks
are still refused. Cache binary/meta versions advance for the changed lowering and
persistent resource layouts.

Production SRT regression reproduced four failures on candidate11 (551 checks/4
failures). Candidate12 passes567/0, covering r128/full descriptors, either operand
order, distinct masked/unmasked views and source-format refresh. The old test target
had a stale`ObjectPool<Inst>` fixture; it was updated to current`IR::InstPool` before
running either comparison. Candidate12 game acceptance follows below (allocation failure).


## Candidate12–14: bounded uploads, first gameplay and CMASK

Candidate12 (APK `e4674a5a`, host `bba9a0d1`) passed both shader blockers but aborted
allocating a 512 MiB HostUncached staging buffer. Driver heap usage was 17,218 MiB;
several legal near-unbounded buffer descriptors required uploads spanning roughly
440–490 MiB. Dedicated large staging allocations could accumulate before retirement.

`BufferCache::SynchronizeMemory` now publishes CPU snapshots as page-aligned chunks
of at most 16 MiB, including snapshot retry padding. Each copy is recorded before
reserving the next chunk, allowing the existing bounded staging ring to submit/wait
and reuse completed blocks. Guest binding size is unchanged; image alias matching
still runs once on the complete binding. This does not bound every dedicated staging
consumer or change the separate deferred-download ownership rules.

Odin3 production GPU regression:

- Old 128 MiB upload: 100,663,291 checks / 3 failures, all in the allocation-shape
  assertions; byte comparisons passed. It retained a dedicated 128 MiB block.
- New 128 MiB upload: 100,663,294 / 0, staging128 MiB across eight16 MiB blocks.
- New320 MiB under the low-memory policy: 251,658,238 / 0, staging capped at256 MiB
  across16 blocks for all three whole-range rewrites. Unaligned heads/tails and every
  returned word were checked; the extra three checks enforce the policy budget.

Candidate13 APK `3161e44237d2356e0303d614aebc82a5ed05e630ad4897d46fdd23720d1a6d95`,
host `2c5284814c30db6a379b13f847b01de25e2367e59d2d1bb9d4090607525a0600`, installed
with matching device SHA. PID18339/gen1 reached brightness setup, Santa Monica logo,
main menu, difficulty selection and the opening axe interaction. Cross changed the
menus and R1 triggered the axe action. Audio output became nonzero; final Oboe status
had0 stalls/errors/reconnects. No claim about long-term sound or normal frame rate.
Driver memory snapshots were4,370 then4,015 MiB; HostUncached staging was1,024 MiB
(64 blocks), rather than the prior17 GiB allocation failure. These are different
startup stages, not a same-frame memory benchmark.

Background geometry/lighting still had large missing blocks. The screenshot requested
by the user is `gow4-user-screenshot.png` in the local evidence folder (1920×1080);
it was taken after the one-frame recording and differs from the earlier scene state.
Do not claim the changed lighting proves a fix or that recording has no side effects.
A temporary runtime filter `*:Info Render:Critical` reduced repeated range-clamp log
spam during this diagnostic session; it did not change persisted settings and was
cleared by the subsequent process restart.

One replay was captured and copied with matching SHA
`1368019e40971148618495bebbb8c4f566561646e285fd7234bda605f868599b` (1,912,296,414 stored
bytes;3.8 GiB initial guest address pages). Structural check passed; the first checker
attempt lacked Python zstandard and was rerun after installing that dependency into
the local validation directory. Both75% and100% replays completed with203 waits,
0 forced waits,0 divergences. Both retain missing background; their cold-cache output
also differs from the live captured frame. One-frame75% capture/writeback is not a
pixel-exact whole-game replay claim. Evidence retains the captured frame, both replay
outputs and summaries separately.

Candidate14 adds expanded single-sample CMASK reads (`0xf` per nibble), consistent
with GCN NOAA metadata definitions in the pinned Mesa `ac_descriptors.h`. The host
color image is already expanded, while the guest CMASK can still contain stale
fast-clear codes. Sample count is retained when registering CMASK; multisample CMASK
is left unsupported because it also describes FMASK. GPU tests register real color
images and read their metadata through the production buffer cache: normal unregistered
memory stays unchanged; single-sample and repeated reads are expanded;2×/4× queries
retain the prior path. Old host9216/2048 failures; candidate149216/0.

Candidate14 APK `734b6229a92796fb3737abdb982e2ef0f0e99ac4c4f57225b9a47d8cd6c0bb64`,
host `0257b488ddfac2f491aa203d4c5538f99d27e7e402f33e72a2781105005e52b3`, installed
with matching device SHA. Visual acceptance follows below.


### Candidate14 replay boundary

The same75% recording completed with203 waits/0 forced/0 divergences. Four
single-sample CMASK reads at0x21af48000 and0x227c70000, each0x5000 bytes, exercised
the new path. Comparing output pixels with candidate13 found changes only inside
bounding box(765,70)..(1309,810), around the character; the missing background remains.
Thus CMASK was an actual unsupported interface but is **not established as the root
cause of the remaining background failure**. The1.75s versus7.99s replay wall time
is not a valid performance result (cache/build state differs).

The ordinary game had reached the opening axe prompt and responded to R1 before
this comparison. Full visual correctness, sustained game performance, later areas,
long-run stability and in-game Chinese IME appearance remain unverified. No claim
that all GoW4 issues are fixed. Candidate14 retains the bounded semantic CMASK fix;
no generic multisample metadata emulation is claimed.


### Final ordinary-launch check and cleanup

Candidate14 was restarted without replay properties. PID12402/gen1 loaded the actual
game and reached the main menu (`candidate-14-title.png`); the background problem
still reproduces. Its first normal-run log confirms the new CMASK path executes;
no new assertion, unsupported-shader abort or allocation failure in this window.
The installed base.apk SHA matches candidate14. Original75% render scale/high texture
settings were preserved. All debug pad ports were inactive, no owned ADB forwards
or debugger sessions remained, and `gpu_replay`, `gpu_replay_options`, `shader_dump`
were empty; guest debug port/wait were0. The owned `/data/local/tmp/gow4-ordered-20261028`
probe directory was removed. The named1.8GiB replay is deliberately retained both on
the device and locally for the unresolved rendering investigation; it is evidence,
not an active recording. Other user captures/cache/game files were not removed.

Android host/APK builds and final `git diff --check` passed. New synthetic GPU probes
and targeted regression results above do not constitute an all-games regression.

At handoff, Back placed the ordinary session in `Paused / background`, samePID12402/gen1. No active test input; user can resume it.
