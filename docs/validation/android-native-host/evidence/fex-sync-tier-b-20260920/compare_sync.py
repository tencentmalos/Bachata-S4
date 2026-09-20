"""Summarize hle_sync capture windows (A->B summary, B->C detail) for one or more labelled runs.
Usage: python compare_sync.py <label>=<dir> [<label>=<dir> ...]
Each dir holds sync-A.json, sync-B.json, sync-C.json (or baseline-A/B/C.json)."""
import json, sys
from pathlib import Path

def load(d, names):
    for n in names:
        p = d / n
        if p.exists():
            t = p.read_text(encoding="utf-8", errors="replace")
            i = t.find('{"schema"')
            return json.loads(t[i:t.rfind("}") + 1])
    raise SystemExit(f"missing {names} in {d}")

def rows(s): return {r["operation"]: r for r in s["operations"]}

def summarize(label, d):
    d = Path(d)
    A = load(d, ["sync-A.json", "baseline-A.json"]); B = load(d, ["sync-B.json", "baseline-B.json"]); C = load(d, ["sync-C.json", "baseline-C.json"])
    ra, rb, rc = rows(A), rows(B), rows(C)
    dt = (B["timestamp_ns"] - A["timestamp_ns"]) / 1e9
    out = {"label": label, "summary_window_s": dt, "ops": {}}
    for op, b in rb.items():
        a = ra.get(op, {"completed": 0, "elapsed_ns": 0, "histogram": [0] * 6})
        calls = b["completed"] - a["completed"]
        if calls <= 0: continue
        el = b["elapsed_ns"] - a["elapsed_ns"]
        h = [b["histogram"][i] - a["histogram"][i] for i in range(6)]
        out["ops"][op] = {"calls_per_s": calls / dt, "mean_us": el / calls / 1000, "ge1ms": h[4] + h[5], "ge16ms": h[5]}
    dt2 = (C["timestamp_ns"] - B["timestamp_ns"]) / 1e9
    out["detail_window_s"] = dt2
    for op, c in rc.items():
        b = rb.get(op, {"completed": 0, "elapsed_ns": 0, "phases": [{"calls": 0, "elapsed_ns": 0}] * 5})
        calls = c["completed"] - b["completed"]
        if calls <= 0: continue
        el = c["elapsed_ns"] - b["elapsed_ns"]
        ph = [(c["phases"][i]["calls"] - b["phases"][i]["calls"], c["phases"][i]["elapsed_ns"] - b["phases"][i]["elapsed_ns"]) for i in range(5)]
        row = out["ops"].setdefault(op, {})
        row.update({"detail_calls": calls, "detail_mean_us": el / calls / 1000,
                    "lookup_us": ph[0][1] / calls / 1000, "guard_us": ph[1][1] / calls / 1000,
                    "park_pct": 100 * ph[2][0] / calls, "park_us_per_park": (ph[2][1] / ph[2][0] / 1000) if ph[2][0] else 0,
                    "publish_us": ph[3][1] / calls / 1000, "reacquire_us": ph[4][1] / calls / 1000})
    return out

runs = [summarize(*arg.split("=", 1)) for arg in sys.argv[1:]]
ops = ["Mutex.Lock", "Mutex.Unlock", "Sema.Wait", "Sema.Signal", "Cond.Wait", "Cond.Broadcast", "Rwlock.Read", "Rwlock.Unlock", "Thread.Usleep"]
for r in runs:
    print(f"### {r['label']}: summary {r['summary_window_s']:.1f}s, detail {r['detail_window_s']:.1f}s")
    print(f"{'op':<16}{'calls/s':>9}{'mean_us':>9}{'>=1ms':>7}{'d_mean':>8}{'lookup':>8}{'guard':>7}{'park%':>7}{'park_us':>9}{'publish':>8}{'reacq':>7}")
    for op in ops:
        o = r["ops"].get(op)
        if not o: continue
        print(f"{op:<16}{o.get('calls_per_s',0):9.0f}{o.get('mean_us',0):9.1f}{o.get('ge1ms',0):7d}{o.get('detail_mean_us',0):8.1f}{o.get('lookup_us',0):8.2f}{o.get('guard_us',0):7.2f}{o.get('park_pct',0):7.1f}{o.get('park_us_per_park',0):9.1f}{o.get('publish_us',0):8.2f}{o.get('reacquire_us',0):7.2f}")
    print()
