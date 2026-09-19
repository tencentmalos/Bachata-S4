#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Focused compiler admission checks. Pass --clang <NDK clang>."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--clang", required=True)
args, remaining = parser.parse_known_args()
ROOT = Path(__file__).resolve().parents[2]

class Admission(unittest.TestCase):
    def test_rejected_payloads_cannot_reuse_a_previous_pass(self):
        rejected = {
            "unresolved_host_call": 'extern "C" long missing(); extern "C" long guest_entry(){return missing();}',
            "writable_global": 'volatile long state; extern "C" long guest_entry(){return ++state;}',
            "tls": 'thread_local long state; extern "C" long guest_entry(){return ++state;}',
            "absolute_pointer": 'const char* const ptr="guest"; extern "C" const void* guest_entry(){return &ptr;}',
            "host_standard_library": '#include <vector>\nextern "C" long guest_entry(){std::vector<int> v(4);return v.size();}',
        }
        with tempfile.TemporaryDirectory(prefix="shad-guest-admission-") as tmp:
            root=Path(tmp); source=root/"entry.cpp"; out=root/"out"
            command=[sys.executable,str(ROOT/"scripts/android/build-guest-payload"),
                     "--clang",args.clang,"--source",str(source),"--output",str(out)]
            for name, code in rejected.items():
                with self.subTest(name=name):
                    source.write_text('extern "C" long guest_entry(long a){return a*7+3;}')
                    good=subprocess.run(command,capture_output=True,text=True)
                    self.assertEqual(good.returncode,0,good.stdout+good.stderr)
                    self.assertTrue((out/"guest_payload.h").exists())
                    source.write_text(code)
                    bad=subprocess.run(command,capture_output=True,text=True)
                    self.assertNotEqual(bad.returncode,0,name)
                    self.assertFalse((out/"guest_payload.h").exists(),name)
                    self.assertFalse((out/"manifest.json").exists(),name)

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0],*remaining])
