"""Copy small Swan baseline evidence into the repo evidence directory and write manifest.json.
Large files (PROF captures, screenshots > 3 MiB) are listed with SHA-256 and byte size only."""
import hashlib, json, shutil, sys
from pathlib import Path

SRC = Path(r"C:/workspace/emulations/shadps4/build/validation/swan-baseline-20260921")
EV = Path("docs/validation/android-native-host/evidence/swan-bloodborne-baseline-20260921")
SCRATCH = Path(r"C:/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad/swan")
LIMIT = 3 * 1024 * 1024

def sha(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def main():
    manifest = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
    EV.mkdir(parents=True, exist_ok=True)
    files, large = {}, {}
    for run in sorted(SRC.iterdir()):
        if not run.is_dir():
            continue
        dst = EV / run.name
        for f in sorted(run.rglob("*")):
            if not f.is_file():
                continue
            rel = f.relative_to(SRC).as_posix()
            if f.stat().st_size > LIMIT or f.suffix in (".prof", ".partial"):
                large[rel] = {"sha256": sha(f), "bytes": f.stat().st_size}
                continue
            target = dst / f.relative_to(run)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, target)
            files[rel] = sha(target)
    tools = EV / "tools"
    tools.mkdir(exist_ok=True)
    for name in ("ab_scale.sh", "reach_clinic.sh", "rc.sh", "watch.sh", "collect_swan.py", "promo_analyze.py", "memwatch.sh", "break_delta.py", "readback_analyze.py", "startup_mem.sh", "edit_pass_resume.py", "edit_group_window.py", "edit_readback_diag.py", "edit_fused_readback.py", "edit_wide_block.py", "revert_wide_block.py", "edit_transit_cause.py", "edit_render_break_header.py"):
        src = SCRATCH / name
        if src.exists():
            shutil.copy2(src, tools / name)
            files[f"tools/{name}"] = sha(tools / name)
    manifest["files"] = dict(sorted(files.items()))
    manifest["large_files_not_committed"] = dict(sorted(large.items()))
    (EV / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(len(files), "files,", len(large), "large ->", EV)

if __name__ == "__main__":
    main()
