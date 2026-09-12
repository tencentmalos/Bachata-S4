#!/usr/bin/env python3
"""Deploy the exact linked host DSO and run its auxiliary CLI contract on Android."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time


def sha(path):
    with Path(path).open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--serial", required=True)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)  # Never overwrite a previous device attempt.
    result = {"status": "IN_PROGRESS", "serial": a.serial, "commands": [],
              "scope": "AUXILIARY_CLI; APK/ART/Surface/FEX/Turnip/game NOT_RUN"}
    def save():
        (a.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    def run(args, name, timeout=60):
        entry = {"argv": args, "log": name + ".log", "exit_code": None}
        result["commands"].append(entry)
        save()
        with (a.out / entry["log"]).open("w") as f:
            try:
                process = subprocess.run(args, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
                entry["exit_code"] = process.returncode
            except subprocess.TimeoutExpired:
                entry["timed_out"] = True
                raise
            finally:
                save()
        output = (a.out / entry["log"]).read_text()
        if process.returncode:
            raise RuntimeError(f"{name}: exit {process.returncode}; see {a.out / entry['log']}")
        return output.strip()
    adb = ["adb", "-s", a.serial]
    try:
        build_result = a.build / "result.json"
        build = json.loads(build_result.read_text())
        if build["status"] != "HOST_LINK_PASS":
            raise RuntimeError("build does not report HOST_LINK_PASS")
        result["build_result_sha256"] = sha(build_result)
        result["source_manifest_sha256"] = build["source_manifest_sha256"]
        artifacts = [x for x in build["artifacts"] if Path(x["path"]).name in
                     {"libshadps4_host.so", "libc++_shared.so", "host_library_smoke"}]
        if len(artifacts) != 3:
            raise RuntimeError("three matching deployment artifacts required")
        for x in artifacts:
            if sha(x["path"]) != x["sha256"]:
                raise RuntimeError(f"artifact changed since build: {x['path']}")
        result["artifacts"] = artifacts
        result["device"] = {name: run(adb + ["shell", *command], name) for name, command in
                            {"api": ["getprop", "ro.build.version.sdk"],
                             "abi": ["getprop", "ro.product.cpu.abi"],
                             "page_size": ["getconf", "PAGESIZE"],
                             "identity": ["id"],
                             "model": ["getprop", "ro.product.model"]}.items()}
        if result["device"]["abi"] != "arm64-v8a" or int(result["device"]["api"]) < build["profile"]["api"]:
            raise RuntimeError("device ABI/API is incompatible with this build")
        remote = f"/data/local/tmp/shadps4-host-library-{time.time_ns()}"
        result["remote_directory"] = remote
        run(adb + ["shell", "mkdir", remote], "mkdir")
        for x in artifacts:
            name = Path(x["path"]).name
            run(adb + ["push", x["path"], remote + "/" + name], "push-" + name)
            actual = run(adb + ["shell", "sha256sum", remote + "/" + name], "sha-" + name)
            if actual.split()[0] != x["sha256"]:
                raise RuntimeError(f"deployed hash mismatch: {name}")
        run(adb + ["shell", "chmod", "755", remote + "/host_library_smoke"], "chmod")
        # Device-side timeout also bounds the inferior if the adb client disconnects.
        command = "cd " + shlex.quote(remote) + " && " + shlex.join(
            ["env", "LD_LIBRARY_PATH=" + remote, "timeout", "30", "./host_library_smoke",
             remote + "/user-data"])
        output = run(adb + ["shell", command], "smoke", timeout=45)
        counts = re.findall(r"^host_library_smoke: (\d+) checks / (\d+) failures$", output, re.M)
        if len(counts) != 1 or int(counts[0][0]) < 1 or int(counts[0][1]) != 0:
            raise RuntimeError("missing/duplicate/failing terminal result")
        result["checks"], result["failures"] = map(int, counts[0])
        result["status"] = "AUXILIARY_CLI_PASS"
        print(output)
        return 0
    except Exception as e:
        result["status"] = "FAIL"
        result["error"] = str(e)
        print(e, file=sys.stderr)
        return 1
    finally:
        save()


if __name__ == "__main__":
    sys.exit(main())
