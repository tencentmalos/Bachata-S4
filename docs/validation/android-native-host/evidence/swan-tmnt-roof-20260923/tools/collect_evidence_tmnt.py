"""Archive the Swan TMNT rooftop baseline (run A) with a manifest."""
import hashlib, json, shutil
from pathlib import Path
from PIL import Image

SRC = Path(r"C:/workspace/emulations/shadps4/build/validation/swan-tmnt-20260923")
EV = Path(r"C:/workspace/emulations/shadps4/docs/validation/android-native-host/evidence/swan-tmnt-roof-20260923")
SCRATCH = Path(r"C:/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad/swan")
TOOLS = ("pad.sh", "panel_auto.py", "measure_clinic.sh", "break_delta.py", "perf_record.sh", "perf_buckets.py",
         "perf_subtree.py", "sched_states.py", "sched-only-capture.sh", "collect_evidence_tmnt.py")
SKIP_DIRS = ("binary_cache", "symlibs")
LARGE = (".prof", ".data", ".gz", ".bmp", ".bin", ".o", ".hex", ".partial")

def sha(p):
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

files, large = {}, {}
for f in sorted(SRC.rglob("*")):
    if not f.is_file() or any(p in SKIP_DIRS for p in f.relative_to(SRC).parts):
        continue
    rel = f.relative_to(SRC).as_posix()
    if f.suffix in LARGE or (f.stat().st_size > 3 * 1024 * 1024 and f.suffix != ".png"):
        large[rel] = {"sha256": sha(f), "bytes": f.stat().st_size}
        continue
    target = EV / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    if f.suffix == ".png":
        im = Image.open(f); im.thumbnail((1200, 1200)); im.save(target, optimize=True)
    else:
        shutil.copy2(f, target)
    files[rel] = sha(target)
(EV / "tools").mkdir(parents=True, exist_ok=True)
for name in TOOLS:
    s = SCRATCH / name
    if s.exists():
        shutil.copy2(s, EV / "tools" / name)
        files[f"tools/{name}"] = sha(EV / "tools" / name)
manifest = {
    "title": "Swan TMNT (CUSA50828 v1.11.0) tutorial rooftop baseline and GPU-bound attribution, 2026-09-23",
    "report": "../../swan-tmnt-roof-baseline-20260923.md",
    "device": "Swan PB3110PGL6240001G, boot 0af47cd3-09dc-45ea-b47d-b223e120d3c5",
    "builds": {"A": "merge 3f0c47ea; apk 307333860ea793ce; libshadps4_host.so 93464ad4ce986a6c (Build ID d9618178); "
                    "Turnip 86ca472f; Render 0.5 / Texture medium; pid 31682 generation 1"},
    "frequency_lock": "spatial-debug-tool swan-evt-legacy, operation 1d3957f57e504f23893e7ed32fb9e24c (recorded policy0 max baseline 1900800)",
    "notes": [
        "Terms of use dialog on first boot after the reflash was accepted by the user in the headset.",
        "While loading the rooftop a Pico Settings panel (About) took focus; the game showed 'controller disconnected' and ran in "
        "/foreground until the user closed the panel. All measurements are /top-app (cpuset-log.txt).",
        "GPU busy 98-99% was sampled after unlocking (GPU 902 MHz); locked FPS 19.6 vs unlocked ~25 matches the 902/726 ratio.",
        "PROF captures pulled with `adb exec-out cat`; collect_capture only serves the latest capture id.",
    ],
    "files": dict(sorted(files.items())),
    "large_files_not_committed": dict(sorted(large.items())),
}
(EV / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
print(len(files), "files", len(large), "large")
