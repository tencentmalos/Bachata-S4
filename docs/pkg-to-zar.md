# PKG → `.zar`: packaging PS4 games for shadPS4

## The three layers

shadPS4 does not read PKG files at runtime. Getting a game onto disk is three
distinct steps, and conflating them is the usual source of confusion:

```
game.pkg  --[external tool]-->  loose file tree  --[optional]-->  game.zar
 encrypted PFS container        eboot.bin + sce_sys/            single-file archive
```

- **PKG** is Sony's encrypted distribution container (PFS image inside an
  outer entry table). Upstream removed the parser in `be22674f` ("code: Remove
  fpkg code"); this fork restores it under `src/core/file_format/pkg.*` and
  `src/core/crypto/*`, rebuilt on LibreSSL (RSA/SHA/HMAC) and the fork's
  header-only `common/aes.h` instead of the Crypto++ dependency that removal
  was meant to shed. The emulator still does not mount a PKG directly — the
  restored code is for tooling and future in-app install.
- **Loose tree** is what the emulator actually mounts at `/app0`.
- **`.zar`** is [ZArchive](https://github.com/Exzap/ZArchive), a read-only
  archive format from the Cemu project, wired in here via
  `externals/zarchive`. It compresses with **zstd at level 6 over 64 KiB
  blocks** — both hardcoded in `ZArchiveWriter::AppendData`, with no CLI knob.
  Fixed-size blocks are what make random reads possible: the emulator can seek
  to any file and decompress only the blocks it needs, which is why a game can
  be mounted straight from the archive instead of being unpacked first.

  It replaces the *loose tree*, not the PKG. There is no decryption in a
  `.zar`; it just stops a game from being tens of thousands of small files.

`src/core/file_format/pfs.h` in this repo is for runtime mounting, not for
unpacking — don't mistake it for a PKG reader.

## What the emulator will and will not accept

| Content | Directory | `.zar` | Mount |
|---|---|---|---|
| Base game | yes | yes | `/app0`, `/hostapp` |
| Update / patch | yes | yes | overlaid onto `/app0` |
| Mods | yes | yes | overlaid onto `/app0` |
| DLC (addcont) | yes | yes | `/addcont0..N` |

DLC in a `.zar` needs the archive-aware addcont scan this fork adds. Upstream
enumerates with `directory_iterator` and reads `param.sfo` through a raw host
path, so an archive is skipped without a word. Three pieces fix that:

- `Core::FileSys::ListContentRoots` returns subdirectories, standalone `.zar`
  files, and — for a bundle archive — one root per directory inside it.
- `SplitArchivePath` cuts a path at its `.zar` component, so
  `addcont.zar/P1S1XXXX` names a directory inside the archive.
- `ZArchiveBackend` takes an optional sub-path, letting many mounts share one
  archive; `OpenGameBackend` and `MntPoints::Mount` both route through it.

Both `sceAppContentInitialize` and `sceAppContentAddcontMount` walk the same
sorted list, so mount indices stay consistent.

The list is sorted deliberately: `directory_iterator` order is unspecified, and
`/addcontN` indices are assigned by position. Without sorting the same install
could hand a game different mount points on different machines.

If you are targeting a build without those helpers, pass `--dlc-format dir`.
`--dlc-format zar` sits in between: one archive per DLC, no bundling.

## Layout on disk

The whole title lives in one directory, matched by **filename suffix**:

```
<games>/
  CUSA12878.zar            base            -> /app0
  CUSA12878-UPD.zar        update overlay  -> stacked over /app0
  CUSA12878-DLC.zar        all DLC         -> /addcont0..N
  CUSA12878-mods.zar       mods overlay    (highest priority)
```

Update suffixes are tried in order — `-UPDATE`, `-UPD`, `-patch` — and the
first that resolves wins; `-UPD` is the short form the packer writes. Overlay
precedence is `-mods` → update → base; the first backend holding a path wins,
and directory listings are merged across the stack (`MntPoints::Mount`,
[fs.cpp](../src/core/file_sys/fs.cpp)). Each layer can independently be a
directory or a `.zar` — they mix freely.

DLC can also live in the emulator's addcont folder, keyed by the base game's
title ID, which is what `--addcont` targets:

```
<addcont>/                     default: <UserDir>/addcont
  CUSA12878/
    addcont.zar                or one .zar / directory per package
```

Both are scanned, so the two can coexist. Either way the shape inside is the
same: a bundle is any `.zar` *without* `sce_sys` at its root, and each
top-level directory in it becomes its own content root mounted at
`<archive>/<dir>`. An archive that does have `sce_sys` at the root is a single
piece of content and is used as-is.

Matching is by `CONTENT_ID` inside each `param.sfo`, never by the name on disk.
The tool names each entry after the entitlement label purely for legibility. As
with the base game, don't leave a directory and a `.zar` of the same name side
by side — the directory shadows the archive.

## All-in-one archives

A single `.zar` can hold a whole title instead of just the game directory:

```
CUSA12878.zar
├── app/         <- the game, mounted at /app0
├── update/      <- overlaid onto /app0
└── dlc/
    ├── P1S1XXXXXXXXXXXX/
    └── P1S2XXXXXXXXXXXX/    <- each mounted at /addcontN
```

Build it with `--all-in-one`; launch it exactly like a plain archive
(`--game CUSA12878.zar`). Beat Saber's base, v2.04 update and 246 DLC come to
one 4.7 GiB file.

The three shapes are told apart by what is at the archive root, so they never
collide:

| Root contains | Meaning |
|---|---|
| `sce_sys` | a single piece of content — a game, an update, or one DLC |
| `app` | an all-in-one title |
| neither | a bundle: one directory per piece of content |

`/app0` resolves to `app/` and the update overlay to `update/`, so the guest
paths are unchanged — `eboot.bin` is still `/app0/eboot.bin`. DLC is collected
from `dlc/` alongside the addcont folder and any `-DLC` sibling, so the layouts
mix freely.

The tradeoff is granularity: swapping just the update means repacking the whole
archive. Use the separate-sibling layout when the update or DLC set still moves.

## The `.zar` root must be the game directory

shadPS4 maps a guest path straight onto an archive node: `/app0/sce_sys/param.sfo`
becomes a lookup for `sce_sys/param.sfo`. So the archive root has to *be* the
game folder, with no wrapper:

```
(zar root)
├── eboot.bin           <- depth 0
├── sce_sys/
│   └── param.sfo       <- depth 1
└── ...
```

A `CUSA12878/` directory inside the archive makes every lookup miss. Also don't
leave a same-named directory beside the `.zar` — `make_backend` prefers the
directory.

## Using the tool

```bash
python3 -m zar_packer inspect <pkg|archive|folder>...
python3 -m zar_packer build   <pkg|archive|folder>... [-o <out>] [--addcont <dir>]
```

`inspect` classifies without extracting. `build` runs the whole pipeline:
unpack containers, sort by `param.sfo` CATEGORY, extract, and pack. Output goes
to `~/game/ps4/zar/` unless `-o` says otherwise, as `<TITLE_ID>.zar` plus
`-UPD.zar` / `-DLC.zar` siblings — or one `<TITLE_ID>.zar` with `--all-in-one`.
`--addcont <dir>` puts DLC in the emulator's addcont folder instead.

Classification uses `CATEGORY` (`gd` game, `gp` patch, `ac*` addon), not the
filename — scene releases name files freely. A `gd` package carrying patch
content flags is treated as an update (merged "backport" releases do this).

### Dependencies

- `cryptography` (Python) — `pip install cryptography`
- the `zarchive` CLI, built from `externals/zarchive`:
  ```bash
  cmake -S externals/zarchive -B /tmp/zarbuild -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
  cmake --build /tmp/zarbuild
  ```
  Pass it with `--zarchive /tmp/zarbuild/zarchive`.
- `unar` for `.rar` inputs (`brew install unar`). **Prefer it over 7-Zip**:
  7-Zip silently produced 246 zero-byte files from the Beat Saber DLC RAR5
  archive, which only surfaced later as a confusing "not a PKG" error.

### Launching

```bash
shadps4 --game /path/CUSA12878.zar
```

Archives must be launched by **explicit path**. Title-ID lookup
(`Common::FS::FindGameByID`) and the Big Picture scanner both stat
`<dir>/sce_sys/param.sfo` on the host filesystem and will not find a `.zar`.

## Retail vs. fake-signed

Only fake-signed (fpkg) packages decrypt. The keys in `_pkg_keys.py` come from
the emulator's own history and do not cover retail signing. A retail PKG fails
with "PFSC magic not found after decryption" rather than producing garbage.

## Size expectations

Three different numbers, not interchangeable. Beat Saber base game:

| Stage | Size |
|---|---|
| PKG download | 243 MiB |
| Extracted tree | 292 MiB |
| `.zar` | 193 MiB |

Its v2.04 update is 4.6 GiB extracted and 4.4 GiB packed; the 246 DLC come to
79 MiB across ~3000 loose files, or 68 MiB bundled. The complete title is then
three files:

```
CUSA12878.zar       193 MiB
CUSA12878-UPD.zar   4.4 GiB
CUSA12878-DLC.zar    68 MiB
```

## Verification

The Python extractor is a port of the removed C++ code and is checked against
it. `references/ps4-pkg-tools` is the upstream-decoupled build of that same
code; extracting the Beat Saber base game with both produces **138 files that
are byte-identical by SHA-256**, and a `.zar` round-trip reproduces all 138
unchanged.

To re-run that comparison:

```bash
cmake -S references/ps4-pkg-tools -B /tmp/pkgtool-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/pkgtool-build --target ps4-pkg-tool
/tmp/pkgtool-build/ps4-pkg-tool <game.pkg> /tmp/refextract
python3 -m zar_packer build <game.pkg> -o /tmp/mine --no-zar --keep-extracted /tmp/mine-stage
diff <(cd /tmp/refextract/CUSA12878 && find . -type f -exec shasum -a 256 {} + | sort -k2) \
     <(cd /tmp/mine-stage/CUSA12878 && find . -type f -exec shasum -a 256 {} + | sort -k2)
```

Three details the port has to match exactly, all found by that diff:

- `Inode.Blocks`/`loc` sit at `0x60`/`0x64`, while the on-disk inode stride is
  `0xA8`. Reading them from the end of the record yields zero-length files.
- License entries (`0x400`–`0x403`) are zero-padded to an AES block boundary,
  decrypted whole, then truncated to the entry size. Leaving the trailing
  partial block as ciphertext corrupts the tail of `npbind.dat`.
- A 0x10000 block holds 630.1 inodes of 0xA8 bytes, so its last slice is a
  partial record and must be skipped. Only shows up once a package needs more
  than one inode block — the 2027-file update does, the 138-file base does not.

## The restored C++ path

`src/core/crypto/crypto.{h,cpp}` re-implements the five primitives PKG needs
without Crypto++:

| Primitive | Backend |
|---|---|
| RSA-2048 private op + PKCS#1 v1.5 | LibreSSL `RSA_private_decrypt` |
| SHA-256, HMAC-SHA256 | LibreSSL `SHA256`, `HMAC` |
| AES-CBC, AES-ECB (for the XTS tweak) | `common/aes.h` (header-only) |

`SSL_DEPENDENCIES` is always defined — `externals/CMakeLists.txt` falls back to
the bundled LibreSSL when no system OpenSSL is found — so this adds no new
optional dependency.

Two behavioural changes versus the code as it was deleted:

- `RSA2048Decrypt` zeroes its output when unpadding fails, so a retail package
  fails the later PFSC check instead of proceeding on noise. `PKG::Extract`
  now reports that explicitly rather than reading out of bounds.
- The license-entry padding fix above is applied here too.

The restored crypto is cross-checked against the Python port: for the Beat
Saber base game both derive `data_key = ff8391f9…37f0` and
`tweak_key = 781e9ec6…607a`.
