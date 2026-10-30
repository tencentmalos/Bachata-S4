# God of War III Remastered: texture arena

Compatibility candidate for **CUSA01740 / 01.02**, pinned to the original
`eboot.bin` SELF SHA256
`6e72330e9092e5164049b865e5568f2d450904001a73fe0a2f4b39194c195b3c`.
It is not automatically enabled for this or any other game/version.

On Odin3, entering a new area exhausted the game's 680.602 MiB streaming texture
arena. Its allocator freed the new record on failure but left the parent pointing
to it; the caller ignored the zero return. Subsequent textures reused that record,
so unrelated materials sampled the same data. Two natural transitions produced
45 and 59 such failures. See the
[investigation](../../../../docs/validation/android-native-host/gow3-textures-20261025.md).

The package changes the original **1.25 GiB video arena to 2.25 GiB before renderer
initialization**. The game then requests/maps the larger backing allocation and
constructs its heap and texture arena normally. It does not change the capacity
of an existing allocation, bypass GPU retirement, or recover already corrupted
texture records. Reloading the game is required.

This follows the larger-arena approach previously tested on desktop, with exact
module identity, instruction preimages and typed guest bindings. The old
10-15 experiment concerned startup corruption with different readback settings;
it did not test the allocation failures reproduced here.

## Build and use

```sh
python3 tools/guest-functions/build.py --clang /path/to/ndk/bin/clang \
  --recipe guest/games/CUSA01740/01.02/texture_arena.recipe.json \
  --output build/gow3-texture-arena
scripts/android/guest-patch SERIAL install build/gow3-texture-arena/patch.json
```

Select **Expanded texture arena** in the game's Guest Patches options and start
a new session. For a controlled development trial, use `guest-patch SERIAL
override <patch.json>`; clear that override after the trial. The selection belongs
to the actual game library profile (an EmuPSP reference may use
`emupsp-CUSA01740`, rather than the title ID, as its profile ID).

Disable the selection and restart to restore the original size. Disabling a hook
while playing cannot undo an allocation made during initialization.

## Diagnostics and limits

The initialization log records original/selected video size, the real initializer
result and the resulting texture capacity. The allocation wrapper preserves all
arguments and the original result; counters are atomic, and logs are limited to
the first allocation and first 64 failures. It never creates a substitute record
or pretends a failed allocation succeeded.

The package builds successfully (`3da72970`). It has been copied to Odin3 but is
**not selected or running yet**. Same-route visual validation, real backing size,
failure counters and longer play remain to be checked. Expanding the arena is a
capacity mitigation; it does not repair the game's general failure-handling bug
if a later workload exceeds even the expanded capacity. Other versions, devices
and Windows execution have not been validated with this package.
