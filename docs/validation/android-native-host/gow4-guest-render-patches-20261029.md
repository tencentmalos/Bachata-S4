# GoW4 guest render patches — 2026-10-29

Continuation of [render-pass analysis](gow4-passes-20261029.md). User requested
existing GoW4/GoW5 patches that reduce rendering cost, then game-specific guest
patches where needed. No commit/push or new APK installation in this round.

## Public patch inventory

The current [shadPS4 patch repository](https://github.com/shadps4-emu/ps4_cheats/tree/main/PATCHES)
tree has a God of War III Remastered XML; no GoW4 or Ragnarok XML was found in that
tree. The [illusion PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch/tree/main/patches/xml)
and GoldHEN repositories do contain both games, with matching file blobs:

| Game / source | Relevant options | Limit |
|---|---|---|
| [GoW 2018](https://github.com/illusionyy/PS-Game-Patch/blob/main/patches/xml/GodOfWar-Orbis.xml), blob `76db50465ba870f4c4aea642a1cb1839131bd683` | 720p Base PS4; 1080p checkerboard Pro; Disable TAA; Disable Sharpness | XML title list omits CUSA07413; exact local port required |
| [Ragnarok](https://github.com/illusionyy/PS-Game-Patch/blob/main/patches/xml/GodofWarRagnarok-Orbis.xml), blob `d480f9e4ef36dcfd3814e9fcde37597f6263d6d7` | Resolution patch; Disable TAA; Disable Sharpness | Author notes Pro 1664×936, Base size unspecified; no local GoW5 executable/runtime verification |

Both also provide FPS unlocks. Unlocking a cap alone does not reduce rendering work.
These PS4 XMLs do not provide shadow/volumetric/DoF quality switches. Windows patches
from the same author are platform-specific, not directly applicable PS4 offsets.

## Exact-build 720p adaptation

Recipe: `guest/games/CUSA07413/01.36/base_720p.recipe.json`; C handler:
`base_720p.c`. Original SELF SHA `19e9f05764de160ab8560b18dabc9a113e818f9e4aaaf687dd04265a62bf2c54`;
converted ELF SHA `90880b52303a2e9190c80595c16116b4e495f770f7cccd6f5e757147cb4a1c5f`.
The analysis surrogate SHA `d3781e5b536f309f46868fbfec51e2b0c107cee1e34826b6d14120df3faf59fc`
has byte-identical RX/RW PT_LOAD segments; only analysis headers/sections differ.
All addresses below are module RVAs, with runtime base `0x400000`.

- `0xe65ee` selects console mode from `0x2c4d37c`: mode 1 is the Pro branch.
- Base branch initializes `0x2d10ca4..0x2d10cb0` to render/output 1920×1080.
- At `0xe66de`, replace the complete five-byte `MOV EDI,1920` (`bf80070000`).
  Next instructions are `MOV ECX,ESI` and call `0x35b190`, the framebuffer allocation
  path. No consumer/call exists between the four global stores and the site.
- Handler first reproduces the original zero-extended RDI value. Only for Base mode
  with original arguments and all four original dimensions does it update them to
  1280×720, setting RDI/RSI/RDX consistently. Flags, remaining GPRs and vector state
  are preserved. Pro/unexpected geometry retains original behavior.
- The public XML's wildcard 1920 store also matches a Pro instruction. Its resolution
  table at `0xcec710` participates in Pro mode switching. This port leaves both alone.
  An initial raw-byte draft was built locally, never installed, and superseded by
  the guarded C handler.

Built package SHA `a13d7ba80021ac62b4d0a754f6cea31923f069057347d727c2d8ff719ea0d24a`.
Original ELF preimage and all five data-binding segment permissions were checked.
Nine portable production-handler cases pass: Base geometry, Pro geometry, two bad
arguments, four bad globals, and unchanged registers/flags/vector state.

Odin3 PID22158 and second cold start PID26038 both report `installed=1`, `failed=0`,
one `base_720p_applied` log event and no fallback event. Direct read of the live four
globals confirms 1280/720/1280/720. Selection retains `gow4_android_log_arena_v1`.
This changes allocation-time geometry; runtime disable is not a resolution switch.

## Capture and timing results

APK remains candidate26 `96699bbb87c19a98374956f4bfb171264a49ecac065c2f000331e05fce64dcb0`,
host `cefc7874ee47f1094701d5f3f3fa4c64876dcc7f24f682cbdd93d9c2bb6bfd8b`,
Turnip `24b53e0a70ed19111ee8a49bc8c028875f0a26eab023c85af8da0f922cb90664`.
Precise buffer readbacks, linear-image readback off, streaming submit on and host
render scale 75% are unchanged. No host FSR/SGSR or GPU timing enabled.

Warm title-menu successful-present windows, 80 marks / 79 complete intervals each:

| Selection | PID | Mean ms | FPS | LiteP index |
|---|---:|---:|---:|---|
| 1080p, camera patch runtime off |14131|200.814|4.980|`f3aaef9f61cb46edb7ee6f45eb5fdc5f`|
| 720p, original camera behavior |22158|183.968|5.436|`fec14a3e0d41425eb1e68b9315f7d567`|
| 1080p restored, original globals checked |24565|208.864|4.788|`e07ca06ca86f4f37bb48482f5b32b164`|
| 720p second cold start |26038|179.750|5.563|`4d279da4ee6f473e9c6532a1be4ad130`|

All four have zero skipped chunks, truncation and scope-pairing errors. Boundary
spans and incomplete flow edges remain explicitly excluded from causal attribution.
Two alternating pairs suggest about 9–16% improvement in this title scene, with
normal governor/thermal variation (one thermal status read was 2), not fixed clocks
or deterministic replay. This is not playable FPS or a full-game/long-run result.
The first 720p dump overlapped the device pixel-refresh overlay and is supplemental
only (`94b3efed6f64470d8361b314651a2084`, mean181.021ms); the table uses the later window.

Two-frame command captures, both structurally valid and device/local SHA checked:

| Capture | PM4 SHA | Guest-command SHA |
|---|---|---|
|1080p `5b2012ceb6edc3ddd09c98730e86d528`|`4941cc518d779c08299218a6be253a4e4092773cf4f4671f2748569fa9b3181e`|`09005ca08f0cec95191046af6385ef1d9a8c54e8e0d63f2a5a9511b1640c2b75`|
|720p `75c3131196624cf2e68fd0ff81288340`|`4426a0186a91749c1e7e0b81a83733f83c3b62cfed03f6846aa5fe3de7d27471`|`b78379b249e7786da2e63b73eeaf7f6fb9fc7dc4120cf1a9e2355d588f03a426`|

Main depth and G-buffer attachments become 1280×720. TAA shader `0x2b5ba6df` still
runs once per interval, but its five image descriptors all become 1280×720 and
dispatch becomes160×90×1 (was240×135×1). Thus guest pixel counts drop to4/9 where
these targets apply; this is not merely a lower-resolution final presentation.

Guest actions remain3078/3076 versus3075/3073, including598 compute dispatches.
Host pass instances remain roughly unchanged:101/103 versus102/107, including59/61
versus60/65 resumed instances. This patch does not merge render passes. The two
volume-processing shaders still dispatch384 times total, retaining240×135×128
volumes and128³ inputs. Reducing screen resolution alone cannot remove those calls.

## TAA camera experiment and remaining work

The public Disable TAA pattern matches RVA `0x3111b6`, `MOV byte [RBX+0x390],1` in
camera preparation `0x30ffa0`. The experimental C site writes zero instead; exact
preimage and six callers were reviewed, and runtime hit was observed. Package SHA
`613d4ebf6c743bd7f7ccd221ef4b34439c3482a4be5d0b895d9880d3fd2f69f3`.
Same PID14131: camera patch on202.113ms (4.948FPS), off200.814ms (4.980FPS).
Capture `96b3d6ad7d8b06dedbe853acd3b68a90` retains the same TAA compute resolve.
Therefore the flag is not a proven way to remove that shader's cost; leave unselected.

Reverse Study found `volumetric_fog_lighting.hlsl`, `volumetric_fog_integrate.hlsl`
and `volumetric_fog_density.hlsl` registration at `0x43c1b0`; pass/resource declarations
at `0x437ed0`, parameter preparation at `0x437480` and history setup at `0x437ba0`.
The exact link from these functions to captured shader hashes and safe fallback
outputs is not yet closed. No volume pass is skipped or dimension arbitrarily changed.
Next guest work should bind those shaders to these entries, then reduce the volume
grid/light work while preserving resource allocation and defined fallback outputs.

A separate independent-upload hoist experiment compiled the host but received no
GPU ordering validation. Its diff was archived as `opt27-buffer-upload-hoist-deferred.patch`
and removed from active source; no APK containing it was installed. Buffer write
barriers and pass ordering remain unchanged. This is not a claimed render-pass fix.

All raw PROF/sidecars, SQLite indexes, command captures, screenshots, decompilation
artifacts and the site-test build are under local
`build/validation/gow4-render-performance-20261028/opt27-*` (capture files use UUIDs).
The original replay and game saves were not changed by patch tooling.

Final state: PID26038 / generation1 / run UUID `968903cc68e1de21c45735e79be09446`,
Running in the full title scene, log-arena + 720p selected, no camera patch selected.
Ring remains enabled; no debugger attached or owned Reverse Study workspace left
open. Seven PROF files with their source sidecars and six command-capture files
were SHA-checked against retained local copies before removing the device exports;
see `opt27-device-cleanup.json`. The original replay remains on-device.
