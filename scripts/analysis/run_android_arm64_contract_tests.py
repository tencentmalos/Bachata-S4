#!/usr/bin/env python3
"""Run reference C++ transport tests on the host; this does not test Android/FEX."""
import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Optional host test log path")
    args = parser.parse_args()
    core = ROOT / "references/shadps4-arm64"
    gtest = ROOT / "externals/cpp-httplib/test/gtest"
    sources = [
        core / "src/platform/bachata/runtime_client.cpp",
        core / "src/platform/bachata/controller_snapshot.cpp",
        core / "src/platform/bachata/audio_transport.cpp",
        core / "tests/platform/test_bachata_runtime_client.cpp",
        core / "tests/platform/test_bachata_controller_snapshot.cpp",
        gtest / "src/gtest-all.cc",
        gtest / "src/gtest_main.cc",
    ]
    for source in sources:
        if not source.is_file():
            raise SystemExit(f"Missing reference/dependency source: {source}")
    cxx = os.environ.get("CXX", "clang++")
    info = {
        "host": platform.platform(),
        "core_commit": subprocess.check_output(
            ["git", "-C", str(core), "rev-parse", "HEAD"], text=True
        ).strip(),
        "scope": "C++ socket/controller/audio tests only; no Android/Java/FEX/Vulkan",
    }
    log = [json.dumps(info, ensure_ascii=False, indent=2)]
    with tempfile.TemporaryDirectory(prefix="bachata-contract-") as temporary:
        binary = Path(temporary) / "runtime-contract-tests"
        command = [cxx, "-std=c++23", "-pthread"]
        # Apple libc++ in Xcode 16.3 hides jthread/stop_token behind this switch.
        if platform.system() == "Darwin":
            command.append("-D_LIBCPP_ENABLE_EXPERIMENTAL")
        for include in [core / "src", gtest / "include", gtest]:
            command += ["-I", str(include)]
        command += [str(path) for path in sources] + ["-o", str(binary)]
        log.append(shlex.join(command))
        build = subprocess.run(command, capture_output=True, text=True, timeout=120)
        log.append(build.stdout + build.stderr)
        result_code = build.returncode
        if result_code == 0:
            test = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            log.append(test.stdout + test.stderr)
            result_code = test.returncode
    text = "\n".join(log) + f"\nEXIT={result_code}\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end="")
    raise SystemExit(result_code)


if __name__ == "__main__":
    main()
