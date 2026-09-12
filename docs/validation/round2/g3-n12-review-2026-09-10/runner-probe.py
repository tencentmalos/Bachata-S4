import contextlib
import io
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from unittest.mock import patch

root = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
ns = runpy.run_path(str(root / 'scripts/android/run-v0-tests'))
g = ns['main'].__globals__
rows = []
for case, check in [('B02', 'CheckNativeElf'), ('B05', 'CheckPublicApiOnlyConsumer'), ('B06', 'CheckDesktopConfigure')]:
    with tempfile.TemporaryDirectory() as td:
        dest = out / (case + '.json')
        replacements = {
            'run_suite': lambda *_: ns['SuiteRun'](never_started=True, error='not built'),
            'probe_device': lambda *_: {'attached': False},
            'CheckNativeElf': lambda *_: ns['NotRun']('not inspected'),
            'CheckPublicApiOnlyConsumer': lambda: ns['NotRun']('not compiled'),
            'CheckDesktopConfigure': lambda *_: ns['NotRun']('not configured'),
        }
        replacements[check] = lambda *a: {'status': 'FAIL', 'reason': 'independent injected checker failure'}
        capture = io.StringIO()
        with patch.dict(g, replacements), patch.object(sys, 'argv', ['runner', '--build-dir', td, '--out', str(dest)]), contextlib.redirect_stdout(capture):
            code = ns['main']()
        data = json.loads(dest.read_text())
        (out / (case + '.txt')).write_text(capture.getvalue() + f'\nmain_return_code={code}\n')
        rows.append({'case': case, 'main_return_code': code, 'v0_failed': data['summary']['failed'], 'overall': data['overall']})

# Exercise the actual OS launch and actual runner process: ENOEXEC does not prove wrong architecture.
with tempfile.TemporaryDirectory() as td:
    p = Path(td) / 'guest_cpu_contract_tests'
    p.write_bytes(b'corrupted non-executable artifact\n')
    p.chmod(0o755)
    suite = ns['run_suite'](p)
    dest = out / 'corrupt-artifact.json'
    command = [sys.executable, str(root / 'scripts/android/run-v0-tests'), '--build-dir', td, '--out', str(dest)]
    r = subprocess.run(command, env=dict(os.environ, PATH='/usr/bin:/bin:/usr/sbin:/sbin'), capture_output=True, text=True, timeout=60)
    (out / 'corrupt-artifact.txt').write_text(r.stdout + r.stderr)
    rows.append({'case': 'corrupt-artifact', 'command': command, 'runner_process_returncode': r.returncode,
                 'suite_never_started': suite.never_started, 'suite_error': suite.error,
                 'overall': json.loads(dest.read_text())['overall']})
(out / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')
print(json.dumps(rows, indent=2))
