# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build shadPS4 `.zar` archives (and DLC folders) from PS4 PKG files.

shadPS4 does not read PKG at runtime. The supported shapes on disk are a plain
game directory or, for `/app0` and `/hostapp`, a ZArchive `.zar`. This tool
covers the whole path: unpack the PKGs, sort base/update/DLC by what their
param.sfo says, and emit the layout the emulator expects.

Layout produced (see `docs/pkg-to-zar.md` for the why):

    <out>/CUSA12878.zar           base game        -> /app0
    <out>/CUSA12878-UPDATE.zar    update overlay   -> stacked over /app0
    <addcont>/CUSA12878/<label>/  one dir per DLC  -> /addcontN

DLC stays as directories on purpose: `sceAppContentInitialize` enumerates with
`std::filesystem::directory_iterator` and skips anything that is not a
directory, so a `.zar` there would be silently ignored.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

from .pkg import PkgError, PkgFile
from .sfo import entitlement_label, is_addon, read_sfo

# Archive kinds we can unpack before looking for PKGs inside.
ARCHIVE_SUFFIXES = (".rar", ".zip", ".7z", ".tar", ".gz", ".tgz")


class ToolError(RuntimeError):
    """User-facing failure: bad input, missing dependency, unusable PKG."""


def log(msg: str) -> None:
    print(msg, flush=True)


# Transient single-line progress. Only used on a TTY -- when output is piped to
# a file or a log the carriage returns just produce one very long line.
_PROGRESS_TTY = sys.stdout.isatty()


def progress_line(msg: str) -> None:
    if _PROGRESS_TTY:
        print(f"\r\033[K  {msg}", end="", flush=True)


def progress_done() -> None:
    if _PROGRESS_TTY:
        print("\r\033[K", end="", flush=True)


def human(n: int) -> str:
    size = float(n)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if size < 1024 or unit == "GiB":
            return f"{size:.1f} {unit}" if unit != "B" else f"{int(size)} B"
        size /= 1024
    return f"{size:.1f} GiB"


# ── external tools ───────────────────────────────────────────────────


def find_zarchive(explicit: str | None) -> Path:
    """Locate the `zarchive` CLI built from externals/zarchive."""
    if explicit:
        p = Path(explicit)
        if not p.is_file():
            raise ToolError(f"--zarchive {p} does not exist")
        return p

    found = shutil.which("zarchive")
    if found:
        return Path(found)

    repo_root = Path(__file__).resolve().parents[2]
    candidates = [
        repo_root / "build" / "externals" / "zarchive" / "zarchive",
        repo_root / "externals" / "zarchive" / "build" / "zarchive",
    ]
    for c in candidates:
        if c.is_file():
            return c

    raise ToolError(
        "zarchive CLI not found. Build it with:\n"
        "  cmake -S externals/zarchive -B /tmp/zarbuild -G Ninja \\\n"
        "        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew\n"
        "  cmake --build /tmp/zarbuild\n"
        "then pass --zarchive /tmp/zarbuild/zarchive"
    )


def extract_container(archive: Path, dest: Path) -> None:
    """Unpack a .rar/.zip/etc into ``dest``.

    Prefers `unar`: 7-Zip silently produces zero-byte files on some of the
    RAR5 archives these packages ship in, which then fail much later with a
    confusing "not a PKG" error.
    """
    dest.mkdir(parents=True, exist_ok=True)

    if shutil.which("unar"):
        cmd = ["unar", "-q", "-f", "-o", str(dest), str(archive)]
    elif shutil.which("7zz"):
        cmd = ["7zz", "x", "-y", f"-o{dest}", str(archive)]
    elif shutil.which("7z"):
        cmd = ["7z", "x", "-y", f"-o{dest}", str(archive)]
    else:
        raise ToolError(
            "no archive extractor found. Install one with: brew install unar"
        )

    log(f"  unpacking {archive.name} with {cmd[0]} ...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        tail = (result.stderr or result.stdout).strip().splitlines()[-5:]
        raise ToolError(
            f"{cmd[0]} failed on {archive.name}:\n  " + "\n  ".join(tail)
        )


def run_zarchive(zarchive: Path, src_dir: Path, out_zar: Path) -> None:
    out_zar.parent.mkdir(parents=True, exist_ok=True)
    if out_zar.exists():
        out_zar.unlink()
    result = subprocess.run(
        [str(zarchive), str(src_dir), str(out_zar)], capture_output=True, text=True
    )
    if result.returncode != 0 or not out_zar.is_file():
        tail = (result.stderr or result.stdout).strip().splitlines()[-5:]
        raise ToolError(f"zarchive failed:\n  " + "\n  ".join(tail))


# ── PKG classification ───────────────────────────────────────────────


@dataclass
class Classified:
    path: Path
    title_id: str
    content_id: str
    category: str
    title: str
    version: str
    kind: str  # "base" | "update" | "dlc"
    entitlement: str = ""


def classify(pkg_path: Path) -> Classified:
    """Read a PKG's param.sfo and decide whether it is base, update, or DLC.

    CATEGORY is authoritative: "gd" is a game, "gp" a patch, "ac*" additional
    content. Filenames are not trusted -- scene releases name things freely.
    """
    pkg = PkgFile(pkg_path)
    pkg.open()

    if not pkg.sfo:
        raise PkgError(f"{pkg_path.name}: no param.sfo entry in the PKG")

    from .sfo import parse_sfo

    sfo = parse_sfo(pkg.sfo)
    category = str(sfo.get("CATEGORY", ""))
    title_id = str(sfo.get("TITLE_ID", "")) or pkg.title_id
    version = str(sfo.get("APP_VER") or sfo.get("VERSION") or "")

    if is_addon(sfo):
        kind = "dlc"
    elif category == "gp":
        kind = "update"
    elif category == "gd":
        # A "gd" package carrying patch flags is a merged/backport update.
        flags = pkg.content_flags
        patchy = {"FIRST_PATCH", "SUBSEQUENT_PATCH", "DELTA_PATCH", "CUMULATIVE_PATCH"}
        kind = "update" if patchy.intersection(flags) else "base"
    else:
        kind = "base"

    return Classified(
        path=pkg_path,
        title_id=title_id,
        content_id=str(sfo.get("CONTENT_ID", pkg.content_id)),
        category=category,
        title=str(sfo.get("TITLE", "")),
        version=version,
        kind=kind,
        entitlement=entitlement_label(sfo),
    )


def collect_pkgs(inputs: list[Path], workdir: Path) -> list[Path]:
    """Resolve inputs into a flat list of PKG paths, unpacking containers.

    Multi-volume RAR sets are handled by only feeding the first part to the
    extractor; the rest are picked up automatically by `unar`/`7zz`.
    """
    pkgs: list[Path] = []
    containers: list[Path] = []

    def visit(p: Path) -> None:
        if p.is_dir():
            for child in sorted(p.rglob("*")):
                if child.is_file():
                    visit(child)
        elif p.suffix.lower() == ".pkg":
            pkgs.append(p)
        elif p.suffix.lower() in ARCHIVE_SUFFIXES:
            containers.append(p)

    for item in inputs:
        if not item.exists():
            raise ToolError(f"input not found: {item}")
        visit(item)

    # Multi-volume: keep .part1 / .part01, drop the rest.
    filtered: list[Path] = []
    for c in containers:
        stem = c.name.lower()
        if ".part" in stem and not (
            ".part1." in stem or ".part01." in stem or ".part001." in stem
        ):
            continue
        filtered.append(c)

    for index, container in enumerate(filtered):
        dest = workdir / f"unpacked_{index}_{container.stem[:40]}"
        extract_container(container, dest)
        for found in sorted(dest.rglob("*.pkg")):
            if found.is_file() and found.stat().st_size > 0:
                pkgs.append(found)

    # Deduplicate while preserving order.
    seen: set[Path] = set()
    unique: list[Path] = []
    for p in pkgs:
        rp = p.resolve()
        if rp not in seen:
            seen.add(rp)
            unique.append(p)
    return unique


# ── extraction ───────────────────────────────────────────────────────


def extract_pkg(pkg_path: Path, dest: Path, quiet: bool = False) -> int:
    """Extract one PKG into ``dest``. Returns the number of PFS files."""
    pkg = PkgFile(pkg_path)
    pkg.open()
    pkg.prepare()

    dest.mkdir(parents=True, exist_ok=True)
    pkg.extract_sce_sys(dest)

    last = [0.0]

    def progress(name: str, index: int, total: int) -> None:
        now = time.monotonic()
        if quiet or (now - last[0] < 0.5 and index != total):
            return
        last[0] = now
        progress_line(f"[{index}/{total}] {name[:60]}")

    count = pkg.extract_pfs(dest, progress=None if quiet else progress)
    if not quiet:
        progress_done()
    return count


@dataclass
class Plan:
    base: Classified | None = None
    updates: list[Classified] = field(default_factory=list)
    dlcs: list[Classified] = field(default_factory=list)


def build_plan(items: list[Classified], title_id: str | None) -> tuple[str, Plan]:
    """Group classified PKGs by title, picking the newest update."""
    if not items:
        raise ToolError("no usable PKG files found in the inputs")

    if title_id is None:
        counts: dict[str, int] = {}
        for c in items:
            counts[c.title_id] = counts.get(c.title_id, 0) + 1
        title_id = max(counts, key=lambda k: counts[k])
        if len(counts) > 1:
            others = ", ".join(f"{k} ({v})" for k, v in counts.items() if k != title_id)
            log(f"note: multiple title IDs present; using {title_id}. Ignoring: {others}")

    plan = Plan()
    for c in items:
        if c.title_id != title_id:
            continue
        if c.kind == "base":
            if plan.base is None or c.version > plan.base.version:
                plan.base = c
        elif c.kind == "update":
            plan.updates.append(c)
        else:
            plan.dlcs.append(c)

    plan.updates.sort(key=lambda c: c.version)
    return title_id, plan


def unique_dlc_dirname(dlc: Classified, used: set[str]) -> str:
    """Pick a stable, filesystem-safe directory name for a DLC.

    The emulator matches DLC by CONTENT_ID inside param.sfo, not by folder
    name, so this only has to be unique and readable.
    """
    base = dlc.entitlement or dlc.content_id.replace("-", "_") or "dlc"
    safe = "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in base)
    name = safe
    n = 2
    while name in used:
        name = f"{safe}_{n}"
        n += 1
    used.add(name)
    return name


# ── commands ─────────────────────────────────────────────────────────


def cmd_inspect(args: argparse.Namespace) -> int:
    with tempfile.TemporaryDirectory(prefix="zarpack-") as tmp:
        pkgs = collect_pkgs([Path(p) for p in args.inputs], Path(tmp))
        if not pkgs:
            raise ToolError("no .pkg files found in the given inputs")

        log(f"found {len(pkgs)} PKG file(s)\n")
        rows: list[Classified] = []
        for p in pkgs:
            try:
                rows.append(classify(p))
            except PkgError as exc:
                log(f"  [skip] {p.name}: {exc}")

        for kind in ("base", "update", "dlc"):
            group = [r for r in rows if r.kind == kind]
            if not group:
                continue
            log(f"{kind.upper()} ({len(group)}):")
            for r in group:
                extra = f" entitlement={r.entitlement}" if r.entitlement else ""
                size = human(r.path.stat().st_size)
                log(
                    f"  {r.title_id}  v{r.version:<8} cat={r.category:<3} "
                    f"{size:>10}  {r.title[:40]}{extra}"
                )
            log("")
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    out_dir = Path(args.output).resolve()
    zarchive = None if args.no_zar else find_zarchive(args.zarchive)

    addcont_dir = Path(args.addcont).resolve() if args.addcont else out_dir / "addcont"
    keep_dir = Path(args.keep_extracted).resolve() if args.keep_extracted else None

    with tempfile.TemporaryDirectory(prefix="zarpack-") as tmp:
        tmpdir = Path(tmp)
        stage_root = keep_dir if keep_dir else tmpdir / "stage"
        stage_root.mkdir(parents=True, exist_ok=True)

        log("== scanning inputs ==")
        pkgs = collect_pkgs([Path(p) for p in args.inputs], tmpdir)
        if not pkgs:
            raise ToolError("no .pkg files found in the given inputs")
        log(f"found {len(pkgs)} PKG file(s)")

        classified: list[Classified] = []
        for p in pkgs:
            try:
                classified.append(classify(p))
            except PkgError as exc:
                log(f"  [skip] {p.name}: {exc}")

        title_id, plan = build_plan(classified, args.title_id)
        log(
            f"\n== plan for {title_id} ==\n"
            f"  base:    {'yes v' + plan.base.version if plan.base else 'MISSING'}\n"
            f"  updates: {len(plan.updates)}"
            + (f" (using v{plan.updates[-1].version})" if plan.updates else "")
            + f"\n  dlc:     {len(plan.dlcs)}"
        )

        if plan.base is None and not args.allow_no_base:
            raise ToolError(
                "no base game PKG found. shadPS4 needs the base game to mount "
                "/app0; an update alone is not bootable. Pass --allow-no-base "
                "to build the update archive anyway."
            )

        out_dir.mkdir(parents=True, exist_ok=True)
        produced: list[str] = []

        # ── base ──
        if plan.base:
            log(f"\n== base: {plan.base.path.name} ==")
            base_stage = stage_root / title_id
            if base_stage.exists():
                shutil.rmtree(base_stage)
            n = extract_pkg(plan.base.path, base_stage, quiet=args.quiet)
            log(f"  extracted {n} files ({human(dir_size(base_stage))})")
            verify_game_dir(base_stage, "base game")

            if zarchive:
                target = out_dir / f"{title_id}.zar"
                log(f"  packing -> {target.name}")
                run_zarchive(zarchive, base_stage, target)
                log(f"  {human(target.stat().st_size)}")
                produced.append(str(target))
            else:
                produced.append(str(base_stage))

        # ── update ──
        if plan.updates:
            upd = plan.updates[-1]
            log(f"\n== update v{upd.version}: {upd.path.name} ==")
            upd_stage = stage_root / f"{title_id}-UPDATE"
            if upd_stage.exists():
                shutil.rmtree(upd_stage)
            n = extract_pkg(upd.path, upd_stage, quiet=args.quiet)
            log(f"  extracted {n} files ({human(dir_size(upd_stage))})")
            verify_game_dir(upd_stage, "update")

            if zarchive:
                target = out_dir / f"{title_id}-UPDATE.zar"
                log(f"  packing -> {target.name}")
                run_zarchive(zarchive, upd_stage, target)
                log(f"  {human(target.stat().st_size)}")
                produced.append(str(target))
            else:
                produced.append(str(upd_stage))

        # ── DLC (always plain directories) ──
        if plan.dlcs:
            log(f"\n== dlc: {len(plan.dlcs)} package(s) ==")
            dlc_root = addcont_dir / title_id
            dlc_root.mkdir(parents=True, exist_ok=True)
            used: set[str] = set()
            ok = 0
            for index, dlc in enumerate(plan.dlcs, start=1):
                name = unique_dlc_dirname(dlc, used)
                dest = dlc_root / name
                if dest.exists():
                    shutil.rmtree(dest)
                if not args.quiet:
                    progress_line(f"[{index}/{len(plan.dlcs)}] {name}")
                try:
                    extract_pkg(dlc.path, dest, quiet=True)
                except PkgError as exc:
                    progress_done()
                    log(f"  [{index}/{len(plan.dlcs)}] {name}: FAILED -- {exc}")
                    continue
                if not (dest / "sce_sys" / "param.sfo").is_file():
                    progress_done()
                    log(f"  [{index}/{len(plan.dlcs)}] {name}: no param.sfo, skipping")
                    shutil.rmtree(dest, ignore_errors=True)
                    continue
                ok += 1
            progress_done()
            log(f"  installed {ok}/{len(plan.dlcs)} DLC into {dlc_root}")
            produced.append(str(dlc_root))

        # ── summary ──
        log("\n== done ==")
        for item in produced:
            log(f"  {item}")

        if plan.base and zarchive:
            log(
                f"\nRun with:\n"
                f"  shadps4 --game {out_dir / (title_id + '.zar')}"
            )
        if plan.dlcs:
            log(f"  (add --set-addon-folder {addcont_dir} if it is not your default)")

    return 0


def dir_size(path: Path) -> int:
    return sum(f.stat().st_size for f in path.rglob("*") if f.is_file())


def verify_game_dir(path: Path, label: str) -> None:
    """Sanity-check an extracted directory before it is packed.

    The archive root has to be the game directory itself -- shadPS4 maps
    /app0/<x> straight onto archive node <x>, so a wrapper folder would make
    every lookup miss.
    """
    sfo = path / "sce_sys" / "param.sfo"
    if not sfo.is_file():
        raise ToolError(f"{label}: no sce_sys/param.sfo in {path}")
    if not (path / "eboot.bin").is_file():
        log(f"  warning: {label} has no eboot.bin at the root (ok for some updates)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="zar-packer",
        description="Build shadPS4 .zar archives from PS4 PKG files.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_inspect = sub.add_parser(
        "inspect", help="classify PKGs without extracting anything"
    )
    p_inspect.add_argument("inputs", nargs="+", help="PKG files, archives, or folders")
    p_inspect.set_defaults(func=cmd_inspect)

    p_build = sub.add_parser("build", help="extract PKGs and produce .zar output")
    p_build.add_argument("inputs", nargs="+", help="PKG files, archives, or folders")
    p_build.add_argument("-o", "--output", required=True, help="output directory")
    p_build.add_argument(
        "--addcont",
        help="DLC install root (default: <output>/addcont). Point this at the "
        "emulator's addcont folder to install directly.",
    )
    p_build.add_argument("--title-id", help="only process this title ID")
    p_build.add_argument("--zarchive", help="path to the zarchive CLI")
    p_build.add_argument(
        "--no-zar",
        action="store_true",
        help="stop after extracting; leave plain directories",
    )
    p_build.add_argument(
        "--keep-extracted",
        help="keep the intermediate extracted tree in this directory",
    )
    p_build.add_argument(
        "--allow-no-base",
        action="store_true",
        help="proceed even when no base game PKG is present",
    )
    p_build.add_argument("-q", "--quiet", action="store_true")
    p_build.set_defaults(func=cmd_build)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (ToolError, PkgError) as exc:
        print(f"\nerror: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
