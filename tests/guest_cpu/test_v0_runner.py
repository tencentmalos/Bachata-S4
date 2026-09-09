"""Acceptance reporting regressions. No device, compiler or network required."""
import contextlib
import io
import json
import subprocess
from pathlib import Path
import runpy
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
NS = runpy.run_path(str(ROOT / "scripts/android/run-v0-tests"))
G = NS["main"].__globals__
SuiteRun = NS["SuiteRun"]


class RunnerTests(unittest.TestCase):
    def run_report(self, host=None, device=None):
        absent = lambda *a: SuiteRun(error="not built", never_started=True)
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "result.json"
            with patch.dict(G, {
                "run_suite": host or absent,
                "run_device_suite": device or absent,
                "probe_device": lambda *_: {"attached": False},
                "git": lambda *a, **kw: "review-test",
                "CheckNativeElf": lambda *a: NS["NotRun"]("not inspected"),
                "CheckPublicApiOnlyConsumer": lambda: NS["NotRun"]("not compiled"),
                "CheckDesktopConfigure": lambda *a: NS["NotRun"]("not configured"),
            }), patch.object(sys, "argv", ["runner", "--build-dir", td,
                  "--fex-build-dir", td, "--out", str(out)]), contextlib.redirect_stdout(io.StringIO()):
                NS["main"]()
            data = json.loads(out.read_text())
            self.assertTrue((Path(td) / "result-suites/api_contract_tests.txt").exists())
            return data, {case["id"]: case for case in data["cases"]}

    def test_never_started_and_scope(self):
        data, cases = self.run_report()
        self.assertEqual(cases["M01"]["status"], "NOT_RUN")
        self.assertEqual(data["summary"]["failed"], 0)
        self.assertEqual(data["summary"]["deferred_by_scope"], 3)
        self.assertEqual(data["summary"]["in_scope_total"], 57)

    def test_no_output_failure_and_timeout(self):
        for run in (SuiteRun(exit_code=7), SuiteRun(timed_out=True)):
            with self.subTest(run=run):
                _, cases = self.run_report(host=lambda binary: run if binary.name == "guest_cpu_contract_tests"
                                           else SuiteRun(error="not built", never_started=True))
                self.assertEqual(cases["M01"]["status"], "FAIL")

    def test_skip_cannot_complete_acceptance(self):
        checks = {s: ("PASS", "") for s in NS["SUITE_MAP"]["M13"]}
        checks["M13f"] = ("SKIP", "RWX refused")
        _, cases = self.run_report(host=lambda _: SuiteRun(cases=checks, exit_code=0))
        self.assertEqual(cases["M13"]["status"], "NOT_RUN")

    def test_device_pass_completes_host_skip(self):
        checks = {s: ("PASS", "") for s in NS["SUITE_MAP"]["M13"]}
        skipped = dict(checks, M13f=("SKIP", "RWX refused"))
        _, cases = self.run_report(host=lambda _: SuiteRun(cases=skipped, exit_code=0),
                                   device=lambda *a: SuiteRun(cases=checks, exit_code=0))
        self.assertEqual(cases["M13"]["status"], "PASS")

    def test_partial_failure_outranks_missing(self):
        _, cases = self.run_report(host=lambda _: SuiteRun(cases={"M01a": ("FAIL", "bad")}, exit_code=0))
        self.assertEqual(cases["M01"]["status"], "FAIL")

    def test_g2_owned_failures_and_incomplete_smoke(self):
        for suite, expected in (("guest_execution_tests", ("G23a", "G23b", "G23c", "G24a", "G24b", "G21a", "G22a")),
                                ("guest_cpu_contract_tests", ("M21", "M22", "M23", "M24", "M25", "M26"))):
            self.assertTrue(set(expected) <= NS["sub_cases_owned_by"](suite))
        checks = {s: ("PASS", "") for s in NS["SUITE_MAP"]["M09"]}
        _, cases = self.run_report(device=lambda *a: SuiteRun(cases=checks, exit_code=0))
        self.assertEqual(cases["M09"]["status"], "NOT_RUN")
        self.assertEqual(cases["M09"]["auxiliary_status"], "PASS")
        for case, sub in (("M07", "G23c"), ("T03", "G24a"), ("M13", "M24")):
            _, cases = self.run_report(device=lambda *a: SuiteRun(cases={sub: ("FAIL", "injected")}, exit_code=0))
            self.assertEqual(cases[case]["status"], "FAIL")
        _, cases = self.run_report(device=lambda *a: SuiteRun(timed_out=True))
        self.assertEqual(cases["M09"]["status"], "FAIL")

    def test_closeout_cases_have_ownership_and_failures_propagate(self):
        for sub, case in (("G25a", "M09"), ("G25b", "M09"), ("G26a", "M11"), ("G26b", "M11"),
                          ("G27a", "M08"), ("M27", "M13"), ("M28", "M13"), ("M29", "M13"),
                          ("G28a", "M11"), ("G28b", "M11")):
            suite = "guest_execution_tests" if sub.startswith("G") else "guest_cpu_contract_tests"
            self.assertIn(sub, NS["sub_cases_owned_by"](suite))
            data, cases = self.run_report(device=lambda *a: SuiteRun(cases={sub: ("FAIL", "injected")}, exit_code=0))
            self.assertEqual(cases[case]["status"], "FAIL")
            self.assertEqual(data["round2"]["total"], 24)
        for sub, label, count, prefix in (("G25a", "G25", 100, "EPOCH "),
                                           ("G26a", "G26", 100, "EPOCH "),
                                           ("G27a", None, 10, "STORE ")):
            records = [prefix + json.dumps({"case": label, "epoch": i, "ok": True}) for i in range(count)]
            summary = f"[{sub}] case PASS"
            for lines in ([], records[:-1], records + [records[-1]],
                          records + [prefix + "null"], records + [prefix + "[]"],
                          records[:-1] + [prefix + '{"epoch":"bad"}'],
                          records[:-1] + [prefix + json.dumps({"case": label, "epoch": count-1})]):
                parsed, _, _ = NS["parse_sub_cases"]("\n".join(lines + [summary]))
                self.assertEqual(parsed[sub][0], "FAIL")
            parsed, _, _ = NS["parse_sub_cases"]("\n".join(records + [summary]))
            self.assertEqual(parsed[sub][0], "PASS")

    def test_device_deployment_hash_is_verified_before_launch(self):
        with tempfile.TemporaryDirectory() as td:
            binary = Path(td) / "probe"
            binary.write_bytes(b"binary identity")
            for matches in (False, True):
                calls = []
                def run(command, **kwargs):
                    calls.append(command)
                    output = ""
                    if "sha256sum" in command:
                        output = (NS["sha256_of"](binary) if matches else "wrong") + "  remote\n"
                    if any("__EXIT__" in arg for arg in command):
                        output = "[G01a] result PASS\n__EXIT__=0\n"
                    return subprocess.CompletedProcess(command, 0, output, "")
                with patch.object(G["subprocess"], "run", side_effect=run), patch.object(G["shutil"], "which", return_value="adb"):
                    result = NS["run_device_suite"](binary, "fake-device")
                self.assertEqual(result.usable, matches)
                self.assertEqual(any("__EXIT__" in arg for cmd in calls for arg in cmd), matches)
                self.assertIn('"device_sha256"', result.output)
                self.assertIn("rm", calls[-1])

    def test_partial_semantic_coverage_is_auxiliary(self):
        checks = {s: ("PASS", "") for s in NS["SUITE_MAP"]["C02"]}
        _, cases = self.run_report(device=lambda *a: SuiteRun(cases=checks, exit_code=0))
        self.assertEqual(cases["C02"]["status"], "NOT_RUN")
        self.assertEqual(cases["C02"]["auxiliary_status"], "PASS")
        self.assertIn("MXCSR", cases["C02"]["reason"])


if __name__ == "__main__":
    unittest.main()
