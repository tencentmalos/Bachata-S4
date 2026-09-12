#!/usr/bin/env python3
"""Failure-path tests for the build wrapper; no real compiler/device required."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/android/build-host-deps"


class BuildFailures(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.ndk = self.base / "ndk"
        (self.ndk / "meta").mkdir(parents=True)
        (self.ndk / "meta/platforms.json").write_text('{"min":21,"max":35}')
        (self.ndk / "source.properties").write_text("Pkg.Revision = TEST\n")
        toolchain = self.ndk / "build/cmake/android.toolchain.cmake"
        toolchain.parent.mkdir(parents=True)
        toolchain.write_text("# fixture\n")
        compiler = self.ndk / "toolchains/llvm/prebuilt/fixture/bin/clang"
        compiler.parent.mkdir(parents=True)
        compiler.write_text("#!/bin/sh\necho fixture-compiler\n")
        compiler.chmod(0o755)
        self.out = self.base / "output"
        self.bin = self.base / "bin"
        self.bin.mkdir()
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"])

    def invoke(self, *args):
        return subprocess.run([str(SCRIPT), "--ndk", str(self.ndk), "--out", str(self.out), *args],
                              env=self.env, text=True, capture_output=True, timeout=45)

    def fake_cmake(self, body):
        p = self.bin / "cmake"
        p.write_text("#!/bin/sh\n" + body + "\n")
        p.chmod(0o755)

    def test_rejects_unsupported_api_without_fallback(self):
        p = self.invoke("--api", "36")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("no silent API fallback", p.stderr)
        self.assertFalse(self.out.exists())

    def test_rejects_incompatible_profile(self):
        self.out.mkdir()
        (self.out / "profile.json").write_text('{"api":35}')
        p = self.invoke()
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("another NDK/API/STL/source profile", p.stderr)

    def test_configure_failure_replaces_stale_success(self):
        self.out.mkdir()
        (self.out / "result.json").write_text('{"status":"BUILD_PASS"}')
        self.fake_cmake("exit 23")
        p = self.invoke()
        self.assertNotEqual(p.returncode, 0)
        result = json.loads((self.out / "result.json").read_text())
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["commands"][0]["exit_code"], 23)
        self.assertNotIn("artifacts", result)

    def test_build_failure_is_not_success(self):
        self.fake_cmake('if [ "$1" = "--build" ]; then exit 24; fi\nexit 0')
        p = self.invoke()
        self.assertNotEqual(p.returncode, 0)
        result = json.loads((self.out / "result.json").read_text())
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual([c["exit_code"] for c in result["commands"]], [0, 24])
        self.assertEqual(result["device_status"], "NOT_RUN")


if __name__ == "__main__":
    unittest.main()
