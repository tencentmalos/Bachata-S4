---
name: ps4-pkg-to-zar
description: Convert PS4 PKG files (base game, update/patch, DLC) into the layout shadPS4 mounts — .zar archives for base/update and plain directories for DLC. Use when asked to unpack a .pkg, build a .zar, install DLC or an update for shadPS4, or when handling .rar/.zip game releases that contain PKGs.
---

# PS4 PKG → shadPS4 `.zar`

Turn PS4 PKG files into what shadPS4 actually mounts. Full background in
[docs/pkg-to-zar.md](../../../docs/pkg-to-zar.md); this file is the operating
procedure.

## The one thing to get right

shadPS4 never reads PKG at runtime. Three layers, and they are not the same
thing:

```
game.pkg  --[unpack]-->  loose tree  --[optional pack]-->  game.zar
```

`.zar` (ZArchive, from Cemu) replaces the *loose tree*, not the PKG. It carries
no decryption.

**DLC cannot be a `.zar`.** `sceAppContentInitialize` enumerates with
`directory_iterator` and skips non-directories, so an archive is silently
ignored. Base and update can be either.

## Procedure

### 1. Inspect before doing anything

```bash
python3 -m zar_packer inspect /path/to/release/
```

Classification comes from `param.sfo` CATEGORY (`gd` game, `gp` patch, `ac*`
addon), not the filename. Confirm the title ID and that a base package exists —
an update alone is not bootable.

### 2. Build

```bash
python3 -m zar_packer build /path/to/release/ \
    -o ~/games \
    --addcont ~/Library/Application\ Support/shadPS4/addcont \
    --zarchive /tmp/zarbuild/zarchive
```

Produces `~/games/CUSA12878.zar`, `~/games/CUSA12878-UPDATE.zar` if an update
was present, and one directory per DLC under `<addcont>/CUSA12878/`.

Run from `tools/`, or add it to `PYTHONPATH`.

Useful flags:
- `--no-zar` — stop at loose directories (for debugging, or if you prefer trees)
- `--keep-extracted <dir>` — retain the intermediate tree
- `--title-id CUSAxxxxx` — when a folder mixes several games
- `--allow-no-base` — build an update archive with no base present

### 3. Launch

```bash
shadps4 --game ~/games/CUSA12878.zar
```

Explicit path is required. Title-ID lookup and the Big Picture scanner both
stat `sce_sys/param.sfo` on the host filesystem and cannot see inside a `.zar`.

## Setup

`cryptography` and the `zarchive` CLI:

```bash
pip install cryptography
cmake -S externals/zarchive -B /tmp/zarbuild -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build /tmp/zarbuild
```

For `.rar` inputs install `unar` (`brew install unar`) and prefer it over
7-Zip — 7-Zip silently produced 246 zero-byte files from a RAR5 DLC archive,
surfacing much later as a confusing "not a PKG" error. The tool already
prefers `unar` when both are present.

## Naming rules

Overlays are matched by filename suffix and must be siblings:

```
CUSA12878.zar          base
CUSA12878-UPDATE.zar   update   (or -patch; -UPDATE wins if both exist)
CUSA12878-mods.zar     mods     (highest priority)
```

Precedence: `-mods` → `-UPDATE`/`-patch` → base.

DLC goes under `<addcont>/<BASE_TITLE_ID>/<anything>/`, each with
`sce_sys/param.sfo` where `CATEGORY="ac"`. Folder names are cosmetic; the
emulator matches on `CONTENT_ID`.

## Archive root layout

The `.zar` root must be the game directory itself — `eboot.bin` at depth 0,
`sce_sys/param.sfo` at depth 1. A wrapper folder inside the archive makes every
lookup miss, because guest `/app0/X` maps directly to archive node `X`.

Don't leave a same-named directory beside the `.zar`; the directory wins.

## Failure modes

| Symptom | Cause |
|---|---|
| "PFSC magic not found after decryption" | Retail-signed PKG. Only fake-signed (fpkg) decrypt; the keys here don't cover retail. |
| "no base game PKG found" | Only update/DLC supplied. Get the base, or `--allow-no-base`. |
| "truncated -- header declares N bytes" | Incomplete download, or a multi-part archive missing a volume. |
| DLC not showing in game | Wrong `<addcont>` root, wrong title ID subfolder, or DLC was packed as `.zar`. |
| Game not found by title ID | Archives must be launched by explicit path. |
| Extracted PKGs are all 0 bytes | 7-Zip on RAR5. Use `unar`. |

## Verifying the extractor

`references/ps4-pkg-tools` is the upstream-decoupled build of the same PKG code
this tool ports. When PFS handling is touched, re-check byte-exactness against
it — the comparison recipe is in
[docs/pkg-to-zar.md](../../../docs/pkg-to-zar.md#verification). Current status:
138/138 files byte-identical on the Beat Saber base game, and a `.zar`
round-trip reproduces all 138 unchanged.
