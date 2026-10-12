#!/usr/bin/env python3
"""Collect persisted probe runs plus bounded Android/runtime diagnostics."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from datetime import datetime

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    base = ["adb", "-s", args.serial]
    manifest = {"collected_at": datetime.now().astimezone().isoformat(), "serial": args.serial, "files": []}
    commands = [
        ("probe-runs.tar", ["exec-out", "run-as", "org.shadps4.xrealprobe", "tar", "-cf", "-", "files/runs"]),
        ("usb.txt", ["shell", "dumpsys", "usb"]),
        ("display.txt", ["shell", "dumpsys", "display"]),
        ("activities.txt", ["shell", "dumpsys", "activity", "activities"]),
        ("runtime-services.txt", ["shell", "dumpsys", "activity", "services", "com.xreal.glassescontrol"]),
        ("probe-package.txt", ["shell", "dumpsys", "package", "org.shadps4.xrealprobe"]),
        ("control-package.txt", ["shell", "dumpsys", "package", "com.xreal.glassescontrol"]),
        ("network.txt", ["shell", "ip", "addr"]),
        ("logcat-tail.txt", ["logcat", "-d", "-t", "4000", "-v", "threadtime"]),
        ("exit-info.txt", ["shell", "dumpsys", "activity", "exit-info", "org.shadps4.xrealprobe"]),
    ]
    failed = False
    for name, command in commands:
        try:
            result = subprocess.run(base+command, capture_output=True, timeout=25)
            data, error, code = result.stdout, result.stderr.decode(errors="replace"), result.returncode
        except subprocess.TimeoutExpired as e:
            data, error, code = e.stdout or b"", "timeout (partial output preserved)", -1
        (args.out/name).write_bytes(data)
        manifest["files"].append({"name": name, "command": command, "exit_code": code, "stderr": error,
            "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        failed |= code != 0
        print(f"{name}: {len(data)} bytes, exit={code}", flush=True)
    (args.out/"manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    raise SystemExit(1 if failed else 0)

if __name__ == "__main__":
    main()
