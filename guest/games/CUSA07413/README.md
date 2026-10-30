# God of War (2018), CUSA07413 01.36

These recipes bind the exact original SELF SHA-256
`19e9f05764de160ab8560b18dabc9a113e818f9e4aaaf687dd04265a62bf2c54`.
They do not modify the ROM library or saves. Other revisions must be reviewed
and rebound; do not copy these offsets into another executable.

| Recipe | Purpose | Validation |
|---|---|---|
| `01.36/log_arena.recipe.json` | Place the game's logging arena within Android's available VA range | Existing startup prerequisite |
| `01.36/base_720p.recipe.json` | Allocate Base PS4 render and output targets at 1280×720 | Odin3 title menu, two cold starts; actual GPU resources verified |
| `01.36/disable_taa.recipe.json` | Experimental public “Disable TAA” camera flag | Flag changes, but temporal resolve still dispatches; no measured speedup, not selected |
| `01.36/disable_volumetric_fog.recipe.json` | Omit the volumetric fog graph pass and publish the game's neutral volume | Odin3 same-process title comparison: 5.20–5.29 → 9.11 FPS; 385 fog dispatches removed |

The 720p recipe replaces one startup instruction through a C site handler. It
validates console mode, arguments and all four original dimensions before changing
them together. Pro mode and unexpected geometry preserve the original instruction's
behavior. This is an allocation-time choice: **restart after changing selection**.
Runtime enable/disable cannot resize existing resources or undo the dimensions.

Discovery credit: illusion's [720p patch](https://github.com/illusionyy/PS-Game-Patch/blob/main/patches/xml/GodOfWar-Orbis.xml),
blob `76db50465ba870f4c4aea642a1cb1839131bd683`; the camera flag credits TL431 / illusion.
That XML does not list CUSA07413. This port verifies the actual 01.36 executable,
and preserves the Pro branch instead of applying first-match wildcard replacements.

Build using an NDK Clang toolchain:

```sh
python3 tools/guest-functions/build.py --clang /path/to/ndk/bin/clang \
  --recipe guest/games/CUSA07413/01.36/base_720p.recipe.json --output build/gow4-720p
scripts/android/guest-patch SERIAL install build/gow4-720p/patch.json
scripts/android/guest-patch SERIAL select emupsp-CUSA07413 \
  gow4_android_log_arena_v1 gow4_base_720p_v1
```

`select` replaces the selection; retain any other required packages for your setup.
For this Odin3 configuration, select only `gow4_android_log_arena_v1` and restart
to restore 1080p. Desktop installation uses the common guest-patch loader, but this
recipe's desktop behavior and Pro mode have not been tested in a game session.

Portable handler regression (actual production C handler, nine geometry/state cases):

```sh
clang -std=c11 -Wall -Wextra -Werror -Iguest/custom/v1/include \
  guest/games/CUSA07413/tests/base_720p_test.c -o /tmp/gow4-720p-test
/tmp/gow4-720p-test
```

See the [device report](../../../docs/validation/android-native-host/gow4-guest-render-patches-20261029.md)
for capture identities, timing limits and unresolved render-pass costs.

## Experimental volumetric fog option

`gow4_disable_volumetric_fog_v1` replaces only pass 61's registration, after checking
its arguments and backend vtable. It publishes the constructor's zero-filled
4×4×4 RGBA16F texture to the three downstream fog inputs. Unexpected arguments
or backend identity call the original registration. Allocations and pass 60
transmittance remain intact. This changes the scene's atmosphere; it does not
disable every fog effect, TAA, or merge graphics render passes.

```sh
python3 tools/guest-functions/build.py --clang /path/to/ndk/bin/clang \
  --recipe guest/games/CUSA07413/01.36/disable_volumetric_fog.recipe.json \
  --output build/gow4-fog
scripts/android/guest-patch SERIAL install build/gow4-fog/patch.json
scripts/android/guest-patch SERIAL select emupsp-CUSA07413 \
  gow4_android_log_arena_v1 gow4_base_720p_v1 gow4_disable_volumetric_fog_v1
```

Restart once to load a newly selected package. Once loaded,
`disable gow4_disable_volumetric_fog_v1` restores the original fog registration and `enable`
omits it again; allow several frames for restored fog history to warm up. Removing
the package from the next-launch selection also restores normal fog after restart.
Only the exact CUSA07413 01.36 revision above is supported; GoW5 is not covered.

The seven-case portable regression uses the production handler and checks neutral
publication, original-call fallbacks, and unchanged remaining machine state:

```sh
clang -std=c11 -Wall -Wextra -Werror -Iguest/custom/v1/include \
  guest/games/CUSA07413/tests/disable_volumetric_fog_test.c -o /tmp/gow4-fog-test
/tmp/gow4-fog-test
```

See the [fog validation report](../../../docs/validation/android-native-host/gow4-volumetric-fog-20261029.md)
for the exact binding, recordings, timing and remaining limitations.
