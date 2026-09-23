"""Archive runs T (merged build, launcher kill -> /foreground) and U (top-app) with a manifest update."""
import hashlib, json, shutil
from pathlib import Path
from PIL import Image

SRC = Path(r"C:/workspace/emulations/shadps4/build/validation/swan-baseline-20260921")
EV = Path(r"C:/workspace/emulations/shadps4/docs/validation/android-native-host/evidence/swan-bloodborne-baseline-20260921")
SCRATCH = Path(r"C:/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad/swan")
RUNS = ("bb-clinic-T-merged", "bb-clinic-U-topapp")
TOOLS = ("perf_record.sh", "perf_buckets.py", "perf_subtree.py", "perf_hot_ips.py", "sched_states.py",
         "sched-only-capture.sh", "collect_evidence_tu.py")
SKIP_DIRS = ("binary_cache", "symlibs")
LARGE = (".prof", ".data", ".gz", ".bmp", ".bin", ".o", ".hex", ".partial")
LIMIT = 3 * 1024 * 1024

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
            if not f.is_file() or any(part in SKIP_DIRS for part in f.relative_to(src).parts):
                continue
            rel = f.relative_to(SRC).as_posix()
            if f.name.startswith("artifact_manifest."):
                continue
            if f.suffix in LARGE or f.stat().st_size > LIMIT and f.suffix != ".png":
                large[rel] = {"sha256": sha(f), "bytes": f.stat().st_size}
                continue
            target = EV / run / f.relative_to(src)
            target.parent.mkdir(parents=True, exist_ok=True)
            if f.suffix == ".png":
                im = Image.open(f)
                im.thumbnail((1200, 1200))
                im.save(target, optimize=True)
            else:
                shutil.copy2(f, target)
            files[rel] = sha(target)
        console = SRC / f"{run.replace('bb-clinic-', 'bb-clinic-')}.console.txt"
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
                         "post-reflash re-measure, mutex fast-path arena-window fix and merged-build GpuComm attribution "
                         "(runs A-U), 2026-09-21/23")
    manifest["builds"].update({
        "T": "apk 30733386 / host 93464ad4 (merge 3f0c47ea), session generation 1; first FPS windows top-app, then the Pico "
             "launcher was killed under memory pressure and the game dropped to cpuset /foreground (CPUs 0-3); PROF captures 1-3",
        "U": "apk 30733386 / host 93464ad4, same process, session generation 2, cpuset /top-app throughout; PROF captures 4 (light) "
             "and 5 (gpu_timing detail + hle_sync)",
    })
    manifest["notes"] = list(manifest.get("notes", [])) + [
        "Run T: device 16:39:25-26 ResManagerKillPolicy multiWindowLmkdHook killed com.picoxr.launcher (2.6 GB memtrack); the "
        "restarted home took focus and shadPS4 moved from /top-app (0-5) to /foreground (0-3); lmkd HyperHold then wrote the app "
        "memcg to zram (RSS 2.49 -> 1.44 GB). T captures after that are a 4-core control, not a baseline (launcher-kill-logcat.txt).",
        "Run T ended because `am start -a MAIN -c LAUNCHER -f 0x10200000` created a new MainActivity instance and destroyed the "
        "old Surface; the session was cancelled normally (guest return=0). Do not use am start to regain focus.",
        "Run U: frame 69.7 ms, PM4.Resume 68.9 ms per frame, r=0.995; GpuComm 92.8% running in sched; simpleperf GpuComm buckets "
        "in perf-light/perf-buckets.txt and subtree-gpucomm.txt; Guest-1 spin loop disassembly in perf-light/jit-64905ace00.dis.",
        "PROF files 1-2 and 4-5 were pulled with `adb exec-out cat` (collect_capture only serves the latest capture id); "
        "`adb shell cat` on Windows converts LF to CRLF and corrupts binaries.",
    ]
    manifest["files"] = dict(sorted(files.items()))
    manifest["large_files_not_committed"] = dict(sorted(large.items()))
    (EV / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(len(files), "files", len(large), "large")

if __name__ == "__main__":
    main()
