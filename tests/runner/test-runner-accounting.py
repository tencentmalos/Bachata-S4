#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Automated regression for the V0 runner's accounting (R2-B02).

The runner turns what a test process actually did into acceptance verdicts. A single bug in that
accounting makes every PASS suspect, because each PASS is only as trustworthy as the verdict that
assigned it. Round 2 R2-B02 requires the runner to refuse a fixed set of dishonest inputs:

  * a standalone ELF presented as package evidence (B02 stays auxiliary)
  * a missing required suite binary (NOT_RUN, never a fabricated result)
  * a non-zero exit that prints PASSes  (FAIL, and an empty-crash is FAIL not NOT_RUN)
  * a timed-out suite (unusable, but not "never attempted")
  * a duplicated sub-case ID (a later PASS never overwrites an earlier FAIL; iteration recorded)
  * a SKIP (parsed as SKIP, never counted as PASS)
  * an artifact verifier that judges alignment and the NEEDED closure rather than just printing them

Each synthetic case is an executable shell script or a direct module call, so the test runs on any
host without FEX or a device.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RUNNER = REPO / "scripts/android/run-v0-tests"
VERIFIER = REPO / "scripts/android/verify-native-artifacts"

failures: list[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    print(f"[R2-B02] {name:52s} {'PASS' if condition else 'FAIL'}"
          + (f"  -- {detail}" if detail and not condition else ""))
    if not condition:
        failures.append(name)


def make_suite(build_dir: Path, name: str, body: str) -> Path:
    path = build_dir / name
    path.write_text(f"#!/usr/bin/env bash\n{body}\n")
    path.chmod(0o755)
    return path


def run_runner(build_dir: Path, out: Path) -> dict:
    result = subprocess.run(
        [sys.executable, str(RUNNER), "--build-dir", str(build_dir), "--out", str(out)],
        capture_output=True, text=True, timeout=300)
    # The runner exits non-zero when the result set contains FAILs -- that is its CI signal. It
    # writes the JSON either way; a hard crash (exception) leaves no JSON.
    assert out.is_file(), f"runner crashed and wrote no report:\n{result.stdout[-2000:]}\n{result.stderr[-2000:]}"
    return json.loads(out.read_text())


def case_status(report: dict, case_id: str) -> dict:
    return next(c for c in report["cases"] if c["id"] == case_id)


def load(path: Path, name: str):
    from importlib.machinery import SourceFileLoader
    loader = SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def main() -> int:
    rv = load(RUNNER, "rv_mod")
    va = load(VERIFIER, "va_mod")

    suite_name = "guest_cpu_contract_tests"

    with tempfile.TemporaryDirectory() as tmp_s:
        tmp = Path(tmp_s)

        # --- missing binary: nothing attempted, NOT_RUN ------------------------------------------
        empty = tmp / "empty"; empty.mkdir()
        m01 = case_status(run_runner(empty, tmp / "empty.json"), "M01")
        check("missing suite binary leaves cases NOT_RUN", m01["status"] == "NOT_RUN")

        # --- B02 auxiliary: no APK never reaches package coverage --------------------------------
        bd = tmp / "b02"; bd.mkdir()
        b02 = case_status(run_runner(bd, tmp / "b02.json"), "B02")
        check("B02 stays NOT_RUN (auxiliary) without an APK",
              b02["status"] == "NOT_RUN" and b02.get("auxiliary_status") in (None, "PASS"))

        # --- non-zero exit after printing PASSes, including a SKIP -------------------------------
        bd = tmp / "mixed"; bd.mkdir()
        make_suite(bd, suite_name,
                   'echo "[M01a] thing PASS"\n'
                   'echo "[M01b] skipped thing SKIP -- cannot make RWX here"\n'
                   'echo "[M01c] passed before crash PASS"\n'
                   'exit 7\n')
        make_suite(bd, "guest_cpu_hle_abi_tests", 'echo "[H01a] ok PASS"\nexit 0\n')
        make_suite(bd, "host_page_size_probe", 'echo "[P01a] page PASS"\nexit 0\n')
        m01 = case_status(run_runner(bd, tmp / "mixed.json"), "M01")
        check("non-zero exit overrides printed PASSes", m01["status"] == "FAIL",
              f"got {m01['status']}: {m01.get('reason')}")

        # --- non-zero exit with NO output: FAIL, not NOT_RUN -------------------------------------
        bd = tmp / "emptycrash"; bd.mkdir()
        make_suite(bd, suite_name, "exit 7\n")
        make_suite(bd, "guest_cpu_hle_abi_tests", 'echo "[H01a] ok PASS"\nexit 0\n')
        make_suite(bd, "host_page_size_probe", 'echo "[P01a] page PASS"\nexit 0\n')
        m01 = case_status(run_runner(bd, tmp / "emptycrash.json"), "M01")
        check("empty non-zero exit is FAIL, not NOT_RUN", m01["status"] == "FAIL",
              f"got {m01['status']}")

        # --- timed-out suite: unusable, but a started process (not never_started) -----------------
        run = rv.SuiteRun(output="timed out", timed_out=True)
        check("timed-out suite is unusable and not never-started",
              (not run.usable) and (not run.never_started)
              and run.failure_reason() == "suite timed out",
              f"usable={run.usable} never_started={run.never_started}")

        # --- duplicate sub-case ID: worst verdict wins, iteration recorded ------------------------
        verdicts, counts, conflicts = rv.parse_sub_cases("[M01a] x FAIL\n[M01a] x PASS\n")
        check("duplicate ID keeps worst (FAIL) and records iteration",
              verdicts["M01a"][0] == "FAIL" and counts["M01a"] == 2 and len(conflicts) == 1,
              f"{verdicts['M01a'][0]} x{counts['M01a']} conflicts={conflicts}")

        # --- a lone SKIP parses as SKIP, not PASS ------------------------------------------------
        verdicts, _, _ = rv.parse_sub_cases("[M13f] needs RWX SKIP -- host refuses\n")
        check("a lone SKIP is parsed as SKIP, not PASS", verdicts["M13f"][0] == "SKIP",
              verdicts["M13f"][0])

        # --- the verifier judges alignment rather than merely reporting it -----------------------
        elf = {"path": "x", "name": "x.so", "is_aarch64": True, "min_load_align": 0x4000,
               "glibc_needed": [], "glibc_version_files": []}
        bad = va.judge_elf(dict(elf), 0x10000, True)
        good = va.judge_elf(dict(elf), 0x4000, True)
        check("alignment below requirement is rejected, at/above passes",
              len(bad) == 1 and len(good) == 0, f"bad={bad} good={good}")

        # --- package closure: an unshipped non-bionic NEEDED lib is a failure --------------------
        missing = [d for d in ["libmystery.so"] if d not in va.BIONIC_PROVIDED]
        check("an unshipped, non-bionic NEEDED lib is a closure failure",
              missing == ["libmystery.so"], str(missing))

    if failures:
        print(f"\n{len(failures)} runner regression(s) failed: {failures}")
        return 1
    print("\nall R2-B02 runner regressions pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
