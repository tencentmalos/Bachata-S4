# GoW4 volumetric fog guest patch — 2026-10-29

The CUSA07413 01.36 volumetric fog pass can be omitted with defined downstream
inputs. On Odin3, same-process title-menu measurements alternate fog off/on/off/on:
9.107 / 5.289 / 9.108 / 5.204 FPS. The paired mean frame durations improve from
190.620 to 109.804 ms (about 74% higher FPS). This is a short title-scene result,
without fixed clocks; it is not a playable-framerate or full-game claim.

## Exact scope and identity

Sources: `guest/games/CUSA07413/01.36/disable_volumetric_fog.c` and its recipe.
Package `gow4_disable_volumetric_fog_v1`, SHA-256
`017476bf58319a25cda69306dd90029d1da45e729f97a737bfe1a690c52881a7`.

All offsets below are RVAs of **main / eboot.bin**, exact original SELF SHA-256
`19e9f05764de160ab8560b18dabc9a113e818f9e4aaaf687dd04265a62bf2c54`,
converted ELF SHA-256
`90880b52303a2e9190c80595c16116b4e495f770f7cccd6f5e757147cb4a1c5f`.
Runtime base is `0x400000`. The analysis ELF surrogate preserves load-segment
bytes and has SHA `d3781e5b536f309f46868fbfec51e2b0c107cee1e34826b6d14120df3faf59fc`.
The vtable belongs to SCE RELRO (`0x61000010`), not an ordinary PT_LOAD segment.
This binding is not portable to other game revisions or GoW5.

Installed emulator/driver stay candidate26: APK `96699bbb87c19a98374956f4bfb171264a49ecac065c2f000331e05fce64dcb0`,
host `cefc7874ee47f1094701d5f3f3fa4c64876dcc7f24f682cbdd93d9c2bb6bfd8b`,
Turnip `24b53e0a70ed19111ee8a49bc8c028875f0a26eab023c85af8da0f922cb90664`.
No host or driver rebuild/install was required for this change. Base 720p patch,
host scale 75%, Precise buffer readback, linear-image readback off and streaming
submit on were unchanged; no public TAA flag package, host FSR/SGSR or GPU timing.

## Recovered graph contract

The graph metadata at main+`0x253e320` names pass 61 `volumetricFog`.
The selected site main+`0x34d1d0` is the complete five-byte CALL
`e8 3b b0 fe ff` to main+`0x338210`, with EDI=61, ESI=-1 and RDX=0.
Only a fallthrough xref enters this site; no interior entry was found. The return
RAX is dead before the next registration. The callee has 98 indexed direct callers
and builds graph jobs; omitting registration also omits this pass's prepare,
declare and execute jobs, instead of leaving history preparation active while
blindly skipping a shader dispatch.

The render-state pointer at main+`0x2530920` indexes 176-byte records; the backend
object for pass61 is at state+10904. Its constructor binds vtable main+`0x100ece0`,
allocates the normal 240×135×128 volumes, and creates a zero-filled 4×4×4 RGBA16F
texture at object+1072 from a 512-byte buffer. It initially publishes this texture
to all three fog inputs at main+`0x2572950`, `0x2572958`, `0x2572960`.
Normal prepare main+`0x437ba0` redirects those pointers to integrated/ping-pong
outputs; execute is main+`0x4381c0`, shader registration main+`0x43c1b0`.

The handler verifies arguments and the backend vtable, publishes that existing
neutral texture, then omits registration. Null/unexpected backend or arguments
call the bound original function. All other context registers, flags, vector/FX
state and saved stack pointer remain unchanged. Allocations and the separate
pass60 transmittance remain. SDK logs are once-only success/fallback markers.

Portable tests: **7/7**, actual production handler: normal neutral publication,
wrong pass, null state, null backend, wrong vtable, wrong view and nonzero owner.
Expected site/callee bytes and binding segment permissions were checked against
the exact converted ELF. Build and package installation passed. Runtime patch
status reports installed1, failed0, success marker1/pass61, fallback marker0.

Live PID7051 confirms state `0x1d6118c000`, backend `0x1d61191ea0`,
vtable `0x140ece0`, and all three fog pointers equal backend+1072
(`0x1d611922d0`). Raw descriptor and identity are retained in `neutral-live.json`
and `neutral-texture.bin` in the artifact root below.

## Same-process GPU command comparison

Odin3 serial `6edb0593`, PID7051/generation1, run
`33c3445fd2847b5907aa253ac4d08f05`. The initial process restart loaded the new
package; every timing and capture below belongs to this one process.

| Fog | Capture UUID / flips | Main two fog compute shaders | Integrate | Graphics render passes |
|---|---|---|---|---|
| Off | `1905f0dbb259e7a57f2df5d92e362277`, 1095–1096 | 0 + 0 | 0 | 104 / 102 |
| On | `9e3b9109e2a6216e0755aef747798779`, 3528–3529 | 226 + 158 each frame | 1 each frame | 97 / 107 |

The missing guest CS hashes are `e7a719c7`, `d92dd2ba` and `48bd6c44`:
**385 fog dispatches per frame disappear and return with the runtime switch**.
The 4×4×4 neutral volume has 624/621 image references in the fog-off frames and
none in the fog-on frames. Other animation-dependent work differs; do not equate
the entire net action-count difference with fog. Fog-off actions2738/2743 versus
fog-on3160/3142. PM4/GCMD structural checks pass, both contain two complete frames.

Graphics passes still fluctuate around100: this patch removes compute work and
does not solve the independent render-pass splitting problem. Restored fog is
visibly hazier; the neutral version retains Kratos, trees and background geometry
in the captured title views. These are animated screenshots, not pixel-identical
reference frames or validation of every material/weather effect.

## Successful-present measurements

Each resident-ring snapshot contains120 successful-present markers /119 complete
cycles. All four: zero skipped chunks, no truncation, zero scope-pairing errors.
Open scopes at capture boundaries are excluded. Missing-source flow diagnostics
do not provide a wait-dependency edge. Timing samples do not overlap PM4 recording.

| Order | Fog | Mean ms | FPS | Min / max ms | PROF SHA-256 |
|---|---|---:|---:|---:|---|
| 1 | Off |109.809091|9.106714|66.332 /134.845|`6a4cbf122432f5ce33ee251e548ecafd0c5236375e9a8b753934baa355ae2628`|
| 2 | On |189.078337|5.288813|66.530 /300.996|`3afed21e92cb728cb9f55c7793490d8aabee7680a17859099159121f7edca8fe`|
| 3 | Off |109.798817|9.107566|65.201 /167.894|`da5453f0ee3b789edf328a4d6604dfd0a6dd67aeb6bd3d34fb623b09d57b789b`|
| 4 | On |192.161035|5.203969|98.094 /300.811|`4c9c4f5ebf9637ab6119b6ae29f0f89be9e97e02e2fc1b3ce4f6ba5d6e16aba4`|

GPU timing remains disabled because its earlier crash is unresolved. These are
present intervals, not summed GPU-query times. No CPU/GPU fixed-clock controls
were applied. The resident ring stayed enabled; no streaming capture was used.

## Opening smoke and final state

With fog omission enabled, Cross entered the difficulty selector and then the
opening chop prompt. Two bounded R1 inputs reached the guest (8 and 5 poll
observations, both released on timeout); the second action screenshot shows the
axe striking the tree. Kratos and background trees remain visible. This covers
the opening interaction only, not combat, later weather or all fog-dependent
materials. Opening screenshots show about9–10FPS but are not a second measured
performance population. The periodic AYN anti-burn-in pixel-refresh screen was
dismissed after identification; its noise stripe is not game corruption.

Final PID7051/gen1 remains Running, all debug pad inputs explicitly released.
Selection retains log-arena +720p +fog omission, fog package enabled (four runtime
switches), no fallback recorded. Ring stays on; fine scopes and GPU timing off.
Audio status: reconnects0/stalls0/error0, consumed471738912 frames; no listening
or long-session audio validation is implied. The existing GPU timing crash remains
unresolved. No debugger or new ADB forward was used.

The old Reverse Study workspace ID was no longer registered when cleanup ran;
its close returned false. Saved pseudocode and input identities remain local.

## Artifacts

Local root: `build/validation/gow4-fog-20261029/` (not committed).
It contains exact patch/ELF/disassembly, constructor/consumer/registration/dispatch
pseudocode, portable test executable, four raw PROF+sidecars+LiteP indexes,
`litep-measurements.json`, `litep-first-sample-recheck.json`, capture identity manifests, `same-pid-capture-comparison.json`,
`pass-comparison.json`, and original screenshots.

| Capture | PM4 SHA-256 | GCMD SHA-256 |
|---|---|---|
| Fog off |`8a2866f2d5572bdc7f2a1cfc88c51abf2c2c2f54f73f0c536311d24c623e47a7`|`73b1aef18699be913c0f67c10aa3f97cad51e3c5a4bfa71225fd56a8c7a3a16b`|
| Fog on |`f2bf93963fb0a46608207827e51f43cb0f1baeb0722eef3f71f7b758f9ccdf1f`|`fefeabf9dacf66b9c61a0f90f26564357c9d04752a68e07ea49e63cba2958528`|

All12 device exports (four PROF, four original sidecars, two PM4 and two GCMD)
were SHA-verified against the retained local copies before exact-path removal;
see `device-cleanup.json`. Original sidecar SHA uses the pull tool's source hash,
since the local sidecar adds provenance fields. The existing large replay and saves
are retained; no save file was manually edited or removed. This remains a local experimental
option, not a default for all titles; nothing was committed or pushed.
