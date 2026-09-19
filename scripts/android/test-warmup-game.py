#!/usr/bin/env python3
"""Focused accounting negatives; never touches ADB or the user's game."""
import copy
import importlib.machinery
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import types
import unittest
from unittest.mock import patch

warmup = importlib.machinery.SourceFileLoader(
    "warmup", str(Path(__file__).with_name("warmup-game"))).load_module()


class WarmupTests(unittest.TestCase):
    def setUp(self):
        identity = [410, 2, "session-uuid"]
        self.manifest = {"run_id": "run", "identity": identity,
            "frames": [{"elapsed": 1, "state": {"identity": identity, "guest_flip": 10}},
                       {"elapsed": 4, "state": {"identity": identity, "guest_flip": 20}}],
            "presses": [{"elapsed": 2, "button": "stick-right"}]}
        self.review = {"run_id": "run", "scene_frame": 0, "response_frame": 1,
                       "note": "Observer saw the avatar move on the roof"}

    def test_valid_review(self):
        warmup.validate_review(self.review, self.manifest)

    def test_other_run_and_missing_note_rejected(self):
        for changed in ({"run_id": "old"}, {"note": ""}, {"note": False}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                warmup.validate_review(self.review | changed, self.manifest)

    def test_no_input_or_no_frame_progress_rejected(self):
        for what in ("input", "progress", "identity"):
            m = copy.deepcopy(self.manifest)
            if what == "input": m["presses"] = []
            if what == "progress": m["frames"][1]["state"]["guest_flip"] = 10
            if what == "identity": m["frames"][1]["state"]["identity"] = [410, 3, "session-uuid"]
            with self.subTest(what=what), self.assertRaises(ValueError):
                warmup.validate_review(self.review, m)

    def test_invalid_frame_selectors_rejected(self):
        for a, b in [(0, 0), (1, 0), (-1, 1), (0, 2), (False, 1), (0, "1")]:
            with self.subTest(a=a, b=b), self.assertRaises(ValueError):
                warmup.validate_review(self.review | {"scene_frame": a, "response_frame": b}, self.manifest)

    def exercise(self, failure=False, changed_identity=False):
        with tempfile.TemporaryDirectory() as directory:
            args = types.SimpleNamespace(out=Path(directory) / "run", serial="fixture",
                display_id="fixture", input_display="0", button="circle", seconds=1,
                open_last_game=False, interval=.5, capture_interval=1, hold_ms=100)
            now = [0.0]
            calls = []
            state_reads = [0]
            def command(argv, **kwargs):
                calls.append(argv)
                if failure: raise subprocess.CalledProcessError(1, argv)
                if "dumpsys" in argv:
                    state_reads[0] += 1
                    generation = 3 if changed_identity and state_reads[0] > 1 else 2
                    return (f"session: active\npid: 410\ngeneration: {generation}\n"
                        "run_uuid: session-uuid\nstage: Running\n"
                        f"guest_flip: {state_reads[0]} (age_ns=100)\n").encode()
                if "screencap" in argv:
                    return b"\x89PNG\r\n\x1a\n" + bytes(8) + struct.pack(">II", 1920, 1080)
                return b""
            def sleep(seconds): now[0] += seconds
            with patch.object(warmup.subprocess, "check_output", command), \
                 patch.object(warmup.time, "monotonic", lambda: now[0]), \
                 patch.object(warmup.time, "sleep", sleep):
                result = warmup.run(args)
            return result, json.loads((args.out / "manifest.json").read_text()), calls

    def test_live_menu_and_presses_never_auto_pass(self):
        code, manifest, _ = self.exercise()
        self.assertEqual(code, 2)
        self.assertEqual(manifest["status"], "TIMEOUT_UNVERIFIED")
        self.assertTrue(manifest["presses"])

    def test_adb_failure_is_nonzero_and_persisted(self):
        code, manifest, _ = self.exercise(failure=True)
        self.assertEqual(code, 1)
        self.assertEqual(manifest["status"], "ERROR_UNVERIFIED")

    def test_generation_change_during_capture_stops_before_touch(self):
        code, manifest, calls = self.exercise(changed_identity=True)
        self.assertEqual(code, 1)
        self.assertEqual(manifest["status"], "ERROR_UNVERIFIED")
        self.assertFalse(any("input" in call for call in calls))


if __name__ == "__main__":
    unittest.main()
