---
name: ps4-pkg-to-zar
description: Convert PS4 PKG files (base game, update/patch, DLC) into the layout shadPS4 mounts — .zar archives for base, update, and the whole DLC set. Use when asked to unpack a .pkg, build a .zar, install DLC or an update for shadPS4, or when handling .rar/.zip game releases that contain PKGs.
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
no decryption. Compression is zstd level 6 over 64 KiB blocks, both hardcoded
in the writer; the fixed block size is what makes random reads possible.

Base, update, mods and DLC can all be `.zar` in this fork, and a title's whole
DLC set goes into **one** bundle archive rather than one file per package. That
needs the archive-aware addcont scan added here — upstream skips
non-directories silently. Use `--dlc-format dir` if targeting a build without
it, or `--dlc-format zar` for one archive per DLC.

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
python3 -m zar_packer build /path/to/release/
```

Output defaults to `~/game/ps4/zar/`, the whole title in one directory:

```
CUSA12878.zar       base
CUSA12878-UPD.zar   update, if one was supplied
CUSA12878-DLC.zar   every DLC, bundled
```

`--addcont <dir>` puts DLC in the emulator's addcont folder instead.

Pass every source at once — base, update and DLC. A missing update is not an
error, so it is easy to forget one and get a silently incomplete build.

Run from `tools/`, or add it to `PYTHONPATH`.

Useful flags:
- `-o <dir>` — output elsewhere
- `--addcont <dir>` — install DLC straight into the emulator's addcont folder
- `--dlc-format zar|dir` — one archive per DLC, or leave unpacked
- `--no-zar` — stop at loose directories
- `--keep-extracted <dir>` — retain the intermediate tree
- `--title-id CUSAxxxxx` — when a folder mixes several games
- `--allow-no-base` — build with no base present

### 3. Launch

```bash
shadps4 --game ~/game/ps4/zar/CUSA12878.zar
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
CUSA12878-UPD.zar      update   (also -UPDATE, -patch; first match wins)
CUSA12878-DLC.zar      all DLC
CUSA12878-mods.zar     mods     (highest priority)
```

Precedence: `-mods` → update → base.

DLC is either the `-DLC` sibling above or `<addcont>/<BASE_TITLE_ID>/`; both are
scanned. Default is a single bundle with one top-level directory per package; a
`.zar` or directory per package also works. Each entry holds
`sce_sys/param.sfo` with `CATEGORY="ac"`. Names are cosmetic — the emulator
matches on `CONTENT_ID`.

A `.zar` with `sce_sys` at its root is one piece of content; without it, the
archive is a bundle and each top-level directory becomes its own content root.

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
| DLC not showing in game | Wrong `<addcont>` root or title ID subfolder. On a build without archive-aware addcont scanning, rebuild with `--dlc-format dir`. |
| Game not found by title ID | Archives must be launched by explicit path. |
| Extracted PKGs are all 0 bytes | 7-Zip on RAR5. Use `unar`. |

## Verifying the extractor

`references/ps4-pkg-tools` is the upstream-decoupled build of the same PKG code
this tool ports. When PFS handling is touched, re-check byte-exactness against
it — the comparison recipe is in
[docs/pkg-to-zar.md](../../../docs/pkg-to-zar.md#verification). Current status:
138/138 files byte-identical on the Beat Saber base game, and a `.zar`
round-trip reproduces all 138 unchanged.
