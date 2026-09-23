"""Archive runs R (post-fulldump re-measure, inert fast path) and S (arena window fix) with a manifest update."""
import hashlib, json, shutil
from pathlib import Path
from PIL import Image

SRC = Path(r"C:/workspace/emulations/shadps4/build/validation/swan-baseline-20260921")
EV = Path(r"C:/workspace/emulations/shadps4/docs/validation/android-native-host/evidence/swan-bloodborne-baseline-20260921")
SCRATCH = Path(r"C:/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad/swan")
RUNS = ("bb-clinic-R-postfulldump", "bb-clinic-S-syncwindow")
TOOLS = ("measure_clinic.sh", "launch_bb.sh", "intro_only.sh", "intro_step.sh", "panel_crop.py",
         "edit_sync_window.py", "collect_evidence_rs.py")
LIMIT = 3 * 1024 * 1024
SKIP_SUFFIX = (".prof", ".partial", ".bmp", ".log")
SKIP_NAMES = ("cc-raw.png", "now-raw.png", "title-screencap.png", "intro-p1-raw.png", "cross-1-raw.png",
              "cross-2-raw.png", "crash-logcat-noisy.txt", "load_prof_summary.json")

def sha(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def main():
    manifest = json.loads((EV / "manifest.json").read_text(encoding="utf-8"))
    files = dict(manifest.get("files", {}))
    large = dict(manifest.get("large_files_not_committed", {}))
    for run in RUNS:
        src = SRC / run
        for f in sorted(src.rglob("*")):
            if not f.is_file():
                continue
            rel = f.relative_to(SRC).as_posix()
            if f.name.startswith("artifact_manifest."):
                continue
            if f.suffix in SKIP_SUFFIX or f.name in SKIP_NAMES:
                large[rel] = {"sha256": sha(f), "bytes": f.stat().st_size}
                continue
            target = EV / run / f.relative_to(src)
            target.parent.mkdir(parents=True, exist_ok=True)
            if f.suffix == ".png":
                im = Image.open(f)
                if max(im.size) > 1200 or f.stat().st_size > 1024 * 1024:
                    im.thumbnail((1200, 1200))
                im.save(target, optimize=True)
            elif f.stat().st_size > LIMIT:
                large[rel] = {"sha256": sha(f), "bytes": f.stat().st_size}
                continue
            else:
                shutil.copy2(f, target)
            files[rel] = sha(target)
        console = SRC / f"{run}.console.txt"
        if console.exists():
            shutil.copy2(console, EV / console.name)
            files[console.name] = sha(EV / console.name)
    tools = EV / "tools"
    tools.mkdir(exist_ok=True)
    for name in TOOLS:
        s = SCRATCH / name
        if s.exists():
            shutil.copy2(s, tools / name)
            files[f"tools/{name}"] = sha(tools / name)
    manifest["title"] = ("Swan Bloodborne clinic: Render Scale coverage, render-pass accounting, fused readback, "
                         "post-reflash re-measure and the mutex fast-path arena-window fix (runs A-S), 2026-09-21/22")
    manifest["builds"].update({
        "R": "apk 36528ab6 / host 71b8135e (commit 89185c8c; fast path 'installed' but inert: arena outside the "
             "hard-coded 64 GiB window); PROF 6fca7458",
        "S-crash": "apk 4ab95748 / host 02d1c83c (window table + 256 MiB GuestSyncObjects reserved AT ServiceAllocationBase; "
                   "Bloodborne SIGSEGV Guest-1 fault 0x0 within 10 s, with and without debug.shadps4.sync_fastpath=0; pids 26494/27056/28292)",
        "S": "apk a863de67 / host cf2df6be (window reserved at ServiceAllocationBase+4 GiB; fast path active); PROF 690ada9d",
    })
    manifest["notes"] = list(manifest.get("notes", [])) + [
        "Device reflashed 2026-09-22 (boot 0af47cd3), ROM fulldump policy removed by the user (ro.boot.debugpolicy=minidump); "
        "games linked from /sdcard/game/ps4/roms; no Bloodborne save existed, so run R went through New Game "
        "(brightness/controls pages, opening cutscene, character creation with a mandatory name) before the clinic.",
        "Run R first attempt (pid 15279) died with SIGSEGV SEGV_ACCERR fault 0x24d3c0040 in Guest-19 in the same second the "
        "DumpLayer 'Enable DumpLayer Support' preset was applied on the title menu; no tombstone (minidump policy), "
        "fex-fault log empty. Kept as crash-buffer.txt; not reproduced afterwards (preset stays applied).",
        "Runs R and S sit in the clinic gameplay scene after the save load; R fingerprint 1083 draws/frame "
        "(camera on the desk), S 1484 draws/frame (heavy fingerprint, character moved by the intro loop's Circle presses).",
        "hle_sync windows are 'session cumulative since start': divide by the presents recorded next to them.",
        "guest_sync_fastpath_tests: 28/29 with the game running (SF13 park-timing check), rerun with the device idle "
        "recorded in guest_sync_fastpath_tests-idle.txt.",
    ]
    manifest["files"] = dict(sorted(files.items()))
    manifest["large_files_not_committed"] = dict(sorted(large.items()))
    (EV / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(len(files), "files", len(large), "large")

if __name__ == "__main__":
    main()
