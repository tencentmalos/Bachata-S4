#!/usr/bin/env python3
"""G1 evidence regressions: ownership, app coverage, and unresolved fixture relocations."""
import runpy
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
runner = runpy.run_path(str(root / 'scripts/android/run-v0-tests'))
owned = runner['sub_cases_owned_by']('guest_execution_tests')
for case in ('T01', 'T02', 'T03', 'T05'):
    required = {sub for sub in runner['SUITE_MAP'][case] if sub.startswith('G')}
    assert required <= owned, (case, required - owned)
    assert runner['CASES'][case][1] is None, case
    assert 'APK' in runner['INCOMPLETE_COVERAGE'][case], case
print('PASS: G1 suite crash owns every mapped control case; CLI cannot earn APK coverage')

with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    src = tmp / 'bad.S'
    src.write_text('.text\n.globl fixture_bad\n.globl fixture_bad_end\n'
                   'fixture_bad:\njmp fixture_bad\nfixture_bad_end:\n')
    result = subprocess.run([sys.executable, str(root / 'scripts/android/generate-guest-fixtures'),
                             '--source', str(src), '--output', str(tmp / 'bad.h')],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode != 0 and 'unresolved fixture relocations' in result.stderr, result
    assert not (tmp / 'bad.h').exists()
print('PASS: raw .text containing an unresolved global jump is refused')
