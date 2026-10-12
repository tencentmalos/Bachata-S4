#!/usr/bin/env python3
"""Read a collected tar/ZIP without extracting it; summarize measured pose evidence."""
import argparse
from collections import Counter
import json
import math
import tarfile
import zipfile

def summarize(files):
    result = {}
    for name, data in files:
        if not name.endswith(("events.jsonl", "poses.jsonl", "latest.json")):
            continue
        run = name.rsplit("/", 1)[0] if "/" in name else "."
        stats = result.setdefault(run, {"events": Counter(), "pose_queries": 0, "usable_queries": 0,
                                       "tracking_states": Counter(), "device_types": Counter(),
                                       "matrix_changes": 0, "translation_min": None, "translation_max": None,
                                       "malformed_lines": 0, "_previous": None})
        if name.endswith("latest.json"):
            try:
                stats["latest"] = json.loads(data)
            except ValueError:
                stats["malformed_lines"] += 1
            continue
        for line in data.splitlines():
            try:
                entry = json.loads(line)
            except ValueError:
                stats["malformed_lines"] += 1
                continue  # A native crash/export may leave a partial final line.
            stats["events"][entry.get("event", "unknown")] += 1
            if entry.get("event") != "pose_query" and not name.endswith("poses.jsonl"):
                continue
            sample = entry.get("sample", entry)
            stats["pose_queries"] += 1
            stats["usable_queries"] += bool(sample.get("pose_usable"))
            state = tuple(sample.get(k) for k in ("tracking_type", "tracking_state", "tracking_reason"))
            stats["tracking_states"][str(state)] += 1
            stats["device_types"][str(sample.get("device_type"))] += 1
            m = sample.get("matrix_raw", [])
            if len(m) != 16 or not all(isinstance(v, (int, float)) and math.isfinite(v) for v in m):
                continue
            if stats["_previous"] is not None and any(abs(a-b)>1e-6 for a,b in zip(m,stats["_previous"])):
                stats["matrix_changes"] += 1
            stats["_previous"] = m
            p = m[12:15]  # Unconverted SDK matrix slots, no promised game-space convention.
            if stats["translation_min"] is None:
                stats["translation_min"],stats["translation_max"] = p.copy(),p.copy()
            else:
                stats["translation_min"] = [min(a,b) for a,b in zip(stats["translation_min"],p)]
                stats["translation_max"] = [max(a,b) for a,b in zip(stats["translation_max"],p)]
    for stats in result.values():
        del stats["_previous"]
    return {"note": "Queries are not hardware sample counts. Constant matrices can be valid at rest. Position capability/mode does not prove measured 6DoF translation.", "runs": result}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive")
    args = parser.parse_args()
    if zipfile.is_zipfile(args.archive):
        with zipfile.ZipFile(args.archive) as archive:
            result = summarize((name,archive.read(name)) for name in archive.namelist())
    else:
        with tarfile.open(args.archive) as archive:
            result = summarize((m.name,archive.extractfile(m).read()) for m in archive if m.isfile())
    print(json.dumps(result, indent=2, ensure_ascii=False))

if __name__ == "__main__":
    main()
