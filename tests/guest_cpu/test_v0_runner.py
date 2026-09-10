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
    def run_report(self, host=None, device=None, checks=None):
        """Run the real main() (real parser/ownership/aggregation/exit path; only external
        execution and device discovery are mocked) and return (data, cases_by_id, return_code).

        `checks` overrides the B02/B05/B06 direct checkers so a checker FAIL is observable."""
        absent = lambda *a: SuiteRun(error="not built", never_started=True)
        notrun = NS["NotRun"]("not inspected")
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "result.json"
            overrides = {
                "run_suite": host or absent,
                "run_device_suite": device or absent,
                "probe_device": lambda *_: {"attached": False},
                "git": lambda *a, **kw: "review-test",
                "CheckNativeElf": lambda *a: notrun,
                "CheckPublicApiOnlyConsumer": lambda: NS["NotRun"]("not compiled"),
                "CheckDesktopConfigure": lambda *a: NS["NotRun"]("not configured"),
            }
            if checks:
                overrides.update(checks)
            with patch.dict(G, overrides), patch.object(sys, "argv", ["runner", "--build-dir", td,
                  "--fex-build-dir", td, "--out", str(out)]), contextlib.redirect_stdout(io.StringIO()):
                code = NS["main"]()
            data = json.loads(out.read_text())
            self.assertTrue((Path(td) / "result-suites/api_contract_tests.txt").exists())
            return data, {case["id"]: case for case in data["cases"]}, code

    def test_direct_checker_failure_forces_nonzero_exit(self):
        # The P1 N1 regression: B02/B05/B06 are written straight into the case by their direct
        # checkers and never appear in any sub-check/suite set, so a FAIL there must still force
        # the overall decision and a non-zero exit (no other suite output present).
        for cid, key, mk in (
                ("B02", "CheckNativeElf", lambda: {"id": "B02", "status": "FAIL",
                                                   "reason": "verifier says artifacts are wrong"}),
                ("B05", "CheckPublicApiOnlyConsumer",
                 lambda: {"id": "B05", "status": "FAIL", "reason": "public API consumer compiled"}),
                ("B06", "CheckDesktopConfigure",
                 lambda: {"id": "B06", "status": "FAIL", "reason": "desktop build broke"})):
            with self.subTest(checker=cid):
                # Inject the failing checker through the run_report override, so it is not replaced
                # by the default never-run stub.
                if cid == "B02":
                    checks = {"CheckNativeElf": lambda *a, _m=mk: _m()}
                elif cid == "B05":
                    checks = {"CheckPublicApiOnlyConsumer": lambda *a, _m=mk: _m()}
                else:
                    checks = {"CheckDesktopConfigure": lambda *a, _m=mk: _m()}
                data, cases, code = self.run_report(checks=checks)
                self.assertEqual(cases[cid]["status"], "FAIL")
                self.assertTrue(data["overall"]["has_failures"])
                self.assertEqual(code, 1, f"{cid} FAIL must return non-zero")
                self.assertIn(cid, data["overall"]["v0_failed_case_ids"])

    def test_existing_but_unlaunchable_suite_is_a_failure_not_skip(self):
        # A suite binary that exists but the host cannot launch (corrupt/wrong format) is a failed
        # attempt, distinct from the file being absent (never built). No automatic ENOEXEC skip.
        launch_error = SuiteRun(error="cannot launch suite: [8] Exec format error")
        data, _cases, code = self.run_report(
            host=lambda binary: launch_error if binary.name == "guest_cpu_contract_tests"
            else SuiteRun(error="not built", never_started=True))
        self.assertEqual(code, 1)
        self.assertTrue(data["overall"]["has_failures"])
        self.assertEqual(data["overall"]["failed_suite_count"], 1)

    def test_missing_suite_is_not_a_failure(self):
        data, _cases, code = self.run_report()  # everything never-started
        self.assertEqual(code, 0)
        self.assertFalse(data["overall"]["has_failures"])
        self.assertEqual(data["overall"]["failed_suite_count"], 0)

    def test_never_started_and_scope(self):
        data, cases, _code = self.run_report()
        self.assertEqual(cases["M01"]["status"], "NOT_RUN")
        self.assertEqual(data["summary"]["failed"], 0)
        self.assertEqual(data["summary"]["deferred_by_scope"], 3)
        self.assertEqual(data["summary"]["in_scope_total"], 57)

    def test_no_output_failure_and_timeout(self):
        for run in (SuiteRun(exit_code=7), SuiteRun(timed_out=True)):
            with self.subTest(run=run):
                _, cases, _code = self.run_report(host=lambda binary: run if binary.name == "guest_cpu_contract_tests"
                                           else SuiteRun(error="not built", never_started=True))
                self.assertEqual(cases["M01"]["status"], "FAIL")

    def test_skip_cannot_complete_acceptance(self):
        checks = {s: ("PASS", "") for s in NS["SUITE_MAP"]["M13"]}
        checks["M13f"] = ("SKIP", "RWX refused")
        _, cases, _code = self.run_report(host=lambda _: SuiteRun(cases=checks, exit_code=0))
        self.assertEqual(cases["M13"]["status"], "NOT_RUN")

    def test_device_pass_completes_host_skip(self):
        checks = {s: ("PASS", "") for s in NS["SUITE_MAP"]["M13"]}
        skipped = dict(checks, M13f=("SKIP", "RWX refused"))
        _, cases, _code = self.run_report(host=lambda _: SuiteRun(cases=skipped, exit_code=0),
                                   device=lambda *a: SuiteRun(cases=checks, exit_code=0))
        self.assertEqual(cases["M13"]["status"], "PASS")

    def test_partial_failure_outranks_missing(self):
        _, cases, _code = self.run_report(host=lambda _: SuiteRun(cases={"M01a": ("FAIL", "bad")}, exit_code=0))
        self.assertEqual(cases["M01"]["status"], "FAIL")

    def test_g2_owned_failures_and_incomplete_smoke(self):
        for suite, expected in (("guest_execution_tests", ("G23a", "G23b", "G23c", "G24a", "G24b", "G21a", "G22a")),
                                ("guest_cpu_contract_tests", ("M21", "M22", "M23", "M24", "M25", "M26"))):
            self.assertTrue(set(expected) <= NS["sub_cases_owned_by"](suite))
        checks = {s: ("PASS", "") for s in NS["SUITE_MAP"]["M09"]}
        _, cases, _code = self.run_report(device=lambda *a: SuiteRun(cases=checks, exit_code=0))
        self.assertEqual(cases["M09"]["status"], "NOT_RUN")
        self.assertEqual(cases["M09"]["auxiliary_status"], "PASS")
        for case, sub in (("M07", "G23c"), ("T03", "G24a"), ("M13", "M24")):
            _, cases, _code = self.run_report(device=lambda *a: SuiteRun(cases={sub: ("FAIL", "injected")}, exit_code=0))
            self.assertEqual(cases[case]["status"], "FAIL")
        _, cases, _code = self.run_report(device=lambda *a: SuiteRun(timed_out=True))
        self.assertEqual(cases["M09"]["status"], "FAIL")

    def test_closeout_cases_have_ownership_and_failures_propagate(self):
        for sub, case in (("G25a", "M09"), ("G25b", "M09"), ("G26a", "M11"), ("G26b", "M11"),
                          ("G27a", "M08"), ("M27", "M13"), ("M28", "M13"), ("M29", "M13"),
                          ("G28a", "M11"), ("G28b", "M11")):
            suite = "guest_execution_tests" if sub.startswith("G") else "guest_cpu_contract_tests"
            self.assertIn(sub, NS["sub_cases_owned_by"](suite))
            data, cases, _code = self.run_report(device=lambda *a: SuiteRun(cases={sub: ("FAIL", "injected")}, exit_code=0))
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
        _, cases, _code = self.run_report(device=lambda *a: SuiteRun(cases=checks, exit_code=0))
        self.assertEqual(cases["C02"]["status"], "NOT_RUN")
        self.assertEqual(cases["C02"]["auxiliary_status"], "PASS")
        self.assertIn("MXCSR", cases["C02"]["reason"])

    # --- N1: every real failure (V0 / Round 2 / unmapped / crashed suite) drives the exit code ---

    G30_32 = ["G30a", "G30b", "G31a", "G31b", "G31c", "G32a", "G32b", "G32c"]
    G30_32_PARENT = {"G30a": "R2-C03", "G30b": "R2-C03",
                     "G31a": "R2-H01", "G31b": "R2-H01", "G31c": "R2-H01",
                     "G32a": "R2-H02", "G32b": "R2-H02", "G32c": "R2-H02"}

    def test_each_g30_g32_fail_exit0_forces_nonzero_and_round2_fail(self):
        # The P1 regression: a FAIL+exit0 recorded under an R2-only parent still returned 0.
        for sub in self.G30_32:
            with self.subTest(sub=sub):
                data, _cases, code = self.run_report(
                    device=lambda *a, _s=sub: SuiteRun(cases={_s: ("FAIL", "syn")}, exit_code=0))
                self.assertEqual(code, 1)
                self.assertTrue(data["overall"]["has_failures"])
                self.assertEqual(data["overall"]["failed_subchecks_unique"], [sub])
                self.assertEqual(data["overall"]["failed_subcheck_count"], 1)
                r2 = {c["id"]: c for c in data["round2"]["cases"]}
                self.assertEqual(r2[self.G30_32_PARENT[sub]]["status"], "FAIL")

    def test_g33_immediate_syscall_exit_owned_and_fail_exits_nonzero(self):
        # N4 formal G33/G34 (syscall-fault immediate exit + bounded self-loop) live in the device
        # guest suite, map to R2-H05, and a FAIL+exit0 on any sub-check drives a non-zero exit.
        for sub in ("G33a", "G33b", "G34", "G35a", "G35b", "G35c",
                    "G36a", "G36b", "G36c"):
            with self.subTest(sub=sub):
                self.assertIn(sub, NS["sub_cases_owned_by"]("guest_execution_tests"))
                data, _cases, code = self.run_report(
                    device=lambda *a, _s=sub: SuiteRun(cases={_s: ("FAIL", "syn")}, exit_code=0))
                self.assertEqual(code, 1)
                r2 = {c["id"]: c for c in data["round2"]["cases"]}
                self.assertEqual(r2["R2-H05"]["status"], "FAIL")
        # With all present and passing, H05 stays formal NOT_RUN with an auxiliary PASS (partial).
        h05_subs = ("G33a", "G33b", "G34", "G35a", "G35b", "G35c",
                    "G36a", "G36b", "G36c")
        data, _cases, code = self.run_report(device=lambda *a: SuiteRun(
            cases={s: ("PASS", "") for s in h05_subs}, exit_code=0))
        r2 = {c["id"]: c for c in data["round2"]["cases"]}
        self.assertEqual(r2["R2-H05"]["status"], "NOT_RUN")
        self.assertEqual(r2["R2-H05"]["auxiliary_status"], "PASS")

    def test_all_g30_g32_fail_exit0_counts_unique_subs_not_parents(self):
        checks = {s: ("FAIL", "syn") for s in self.G30_32}
        data, _cases, code = self.run_report(device=lambda *a: SuiteRun(cases=checks, exit_code=0))
        self.assertEqual(code, 1)
        # Three distinct parent cases fail, but there are seven distinct failed sub-checks; a sub
        # referenced by two matrices must not be counted twice as two failed sub-checks.
        self.assertEqual(sorted(data["overall"]["round2_failed_cases"]),
                         ["R2-C03", "R2-H01", "R2-H02"])
        self.assertEqual(data["overall"]["round2_failed_case_count"], 3)
        self.assertEqual(data["overall"]["failed_subcheck_count"], 7)

    def test_sub_in_both_v0_and_round2_counts_once_but_fails_both_parents(self):
        # G24a feeds V0 T03 (SUITE_MAP) and R2-M02 (ROUND2_MAP): two parent failures, one sub.
        data, cases, code = self.run_report(
            device=lambda *a: SuiteRun(cases={"G24a": ("FAIL", "syn")}, exit_code=0))
        self.assertEqual(code, 1)
        self.assertEqual(cases["T03"]["status"], "FAIL")
        r2 = {c["id"]: c for c in data["round2"]["cases"]}
        self.assertEqual(r2["R2-M02"]["status"], "FAIL")
        self.assertEqual(data["overall"]["failed_subcheck_count"], 1)

    def test_fail_then_pass_repeat_keeps_worst_and_is_nonzero(self):
        run = SuiteRun(cases={"G31a": ("PASS", "")}, exit_code=0)
        run.cases = {"G31a": ("PASS", "")}  # last printed verdict is PASS...
        # ...but the parser keeps worst verdict across repeats; emulate FAIL-then-PASS text.
        parsed, counts, conflicts = NS["parse_sub_cases"](
            "[G31a  ] earlier FAIL -- iteration 1\n[G31a  ] later PASS\n")
        self.assertEqual(parsed["G31a"][0], "FAIL")
        self.assertEqual(counts["G31a"], 2)
        self.assertTrue(conflicts)
        data, _cases, code = self.run_report(device=lambda *a: SuiteRun(
            cases=dict(parsed), exit_code=0, iteration_counts=counts, conflicts=conflicts))
        self.assertEqual(code, 1)
        r2 = {c["id"]: c for c in data["round2"]["cases"]}
        self.assertEqual(r2["R2-H01"]["status"], "FAIL")

    def test_missing_or_skip_g_sub_is_partial_not_pass_and_exit0(self):
        # One G sub present-and-PASS, the other sibling missing: the R2 parent is partial/NOT_RUN,
        # not a complete PASS, and with no other fault the runner exits 0.
        for present, missing in (("G31a", "G31b"), ("G32a", "G32b")):
            with self.subTest(present=present):
                data, _cases, code = self.run_report(
                    device=lambda *a, _p=present: SuiteRun(cases={_p: ("PASS", "")}, exit_code=0))
                self.assertEqual(code, 0)
                parent = "R2-H01" if present.startswith("G31") else "R2-H02"
                r2 = {c["id"]: c for c in data["round2"]["cases"]}
                self.assertEqual(r2[parent]["status"], "NOT_RUN")
                self.assertEqual(r2[parent]["auxiliary_status"], "NOT_RUN")
        # A SKIP is the same: not complete, not a failure.
        data, _cases, code = self.run_report(
            device=lambda *a: SuiteRun(cases={"G31a": ("SKIP", "x")}, exit_code=0))
        self.assertEqual(code, 0)

    def test_guest_suite_crash_and_timeout_taint_g30_g32_and_are_nonzero(self):
        for run in (SuiteRun(exit_code=7), SuiteRun(timed_out=True)):
            with self.subTest(run=run):
                data, _cases, code = self.run_report(device=lambda *a, _r=run: _r)
                self.assertEqual(code, 1)
                self.assertTrue(data["overall"]["failed_suite_count"] >= 1)
                # Zero-output crash taints the ids the suite owns, including the new H1/H2 prefixes.
                owners = NS["sub_cases_owned_by"]("guest_execution_tests")
                self.assertTrue({s for s in self.G30_32} <= owners)
                r2 = {c["id"]: c for c in data["round2"]["cases"]}
                for parent in ("R2-C03", "R2-H01", "R2-H02"):
                    self.assertEqual(r2[parent]["status"], "FAIL")

    def test_unknown_new_id_fail_is_accounting_failure_and_nonzero(self):
        for text in ("[Z99a  ] brand new FAIL\n",
                     "[Z99a  ] earlier FAIL -- i1\n[Z99a  ] later PASS\n"):
            with self.subTest(text=text):
                parsed, counts, conflicts = NS["parse_sub_cases"](text)
                data, _cases, code = self.run_report(
                    host=lambda *a, _p=parsed, _c=counts, _x=conflicts:
                    SuiteRun(cases=dict(_p), exit_code=0, iteration_counts=_c, conflicts=_x))
                self.assertEqual(code, 1)
                self.assertEqual(data["overall"]["unmapped_failed_subchecks"], ["Z99a"])

    def test_clean_partial_and_never_started_exit_zero_without_fabrication(self):
        # No device suite launched at all: nothing attempted, no fabricated failure, exit 0.
        data, _cases, code = self.run_report()
        self.assertEqual(code, 0)
        self.assertFalse(data["overall"]["has_failures"])
        # An unmapped PASS (extra diagnostic nobody maps) is harmless.
        parsed, _, _ = NS["parse_sub_cases"]("[Z98a  ] extra diag PASS\n")
        _d, _c, code = self.run_report(
            host=lambda *a, _p=parsed: SuiteRun(cases=dict(_p), exit_code=0))
        self.assertEqual(code, 0)

    def test_real_runner_subprocess_returncode_matches_failures(self):
        # At least one check through a real runner *process*: its OS exit code must agree with the
        # observed failure. Parser/aggregation/exit are all the shipped code; only the suite binary
        # is a synthesised host executable (no device needed).
        runner = ROOT / "scripts/android/run-v0-tests"
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            build = td / "build"; build.mkdir()
            suite = build / "guest_cpu_contract_tests"
            for verdict, expect_code in (("FAIL", 1), ("PASS", 0)):
                suite.write_text("#!/bin/sh\n"
                                 f'printf "[M01a  ] a PASS\\n[M01b  ] b PASS\\n[M01c  ] c {verdict}\\n"\n')
                suite.chmod(0o755)
                out = td / f"result-{verdict}.json"
                # Hide platform-tools so no real device interferes.
                env = dict(__import__("os").environ)
                env["PATH"] = __import__("os").pathsep.join(
                    p for p in env.get("PATH", "").split(__import__("os").pathsep)
                    if p and "platform-tools" not in p)
                proc = subprocess.run([sys.executable, str(runner), "--build-dir", str(build),
                                       "--out", str(out)], capture_output=True, text=True, env=env)
                self.assertEqual(proc.returncode, expect_code,
                                 f"M01c {verdict}: {proc.stdout}\n{proc.stderr}")


if __name__ == "__main__":
    unittest.main()
