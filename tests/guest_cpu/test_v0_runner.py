"""Acceptance reporting regressions. No device, compiler or network required."""
import contextlib
import io
import json
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

    def test_partial_semantic_coverage_is_auxiliary(self):
        checks = {s: ("PASS", "") for s in NS["SUITE_MAP"]["C02"]}
        _, cases = self.run_report(device=lambda *a: SuiteRun(cases=checks, exit_code=0))
        self.assertEqual(cases["C02"]["status"], "NOT_RUN")
        self.assertEqual(cases["C02"]["auxiliary_status"], "PASS")
        self.assertIn("MXCSR", cases["C02"]["reason"])


if __name__ == "__main__":
    unittest.main()
