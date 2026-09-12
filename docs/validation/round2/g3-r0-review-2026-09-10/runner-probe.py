import contextlib
import io
import json
from pathlib import Path
import runpy
import sys
import tempfile
from unittest.mock import patch

root = Path(sys.argv[1]).resolve()
outdir = Path(sys.argv[2]).resolve()
outdir.mkdir(parents=True, exist_ok=True)
ns = runpy.run_path(str(root / 'scripts/android/run-v0-tests'))
g = ns['main'].__globals__
SuiteRun = ns['SuiteRun']
rows = []
scenarios = [(s + '-fail-exit0', f'[{s}] injected result FAIL -- independent audit\n')
             for s in ['G30a', 'G30b', 'G31a', 'G31b', 'G31c', 'G32a', 'G32b', 'Z99a']]
scenarios += [('g31-conflict', '[G31a] injected result FAIL\n[G31a] injected result PASS\n'),
              ('g31-skip', '[G31a] injected result SKIP\n'),
              ('g31-pass', '[G31a] injected result PASS\n'),
              ('guest-crash', ''), ('guest-timeout', ''), ('missing', '')]
for name, output in scenarios:
    with tempfile.TemporaryDirectory() as td:
        destination = outdir / (name + '.json')
        parsed, counts, conflicts = ns['parse_sub_cases'](output)
        injected = SuiteRun(cases=parsed, output=output, exit_code=7 if name == 'guest-crash' else 0,
                            timed_out=name == 'guest-timeout', never_started=name == 'missing',
                            iteration_counts=counts, conflicts=conflicts)
        absent = lambda *a: SuiteRun(error='not built', never_started=True)
        def device(binary, *args):
            return injected if binary.name == 'guest_execution_tests' else absent()
        captured = io.StringIO()
        with patch.dict(g, {
            'run_suite': absent, 'run_device_suite': device,
            'probe_device': lambda *_: {'attached': False, 'synthetic': True},
            'CheckNativeElf': lambda *a: ns['NotRun']('not inspected: synthetic test'),
            'CheckPublicApiOnlyConsumer': lambda: ns['NotRun']('not compiled: synthetic test'),
            'CheckDesktopConfigure': lambda *a: ns['NotRun']('not configured: synthetic test'),
        }), patch.object(sys, 'argv', ['runner', '--build-dir', td, '--fex-build-dir', td,
                                        '--out', str(destination), '--run-id', name]), \
             contextlib.redirect_stdout(captured):
            code = ns['main']()
        data = json.loads(destination.read_text())
        (outdir / (name + '.txt')).write_text(captured.getvalue() + f'\nmain_return_code={code}\n')
        rows.append({'scenario': name, 'main_return_code': code,
                     'v0_failed': data['summary']['failed'],
                     'r2_failed': [c['id'] for c in data['round2']['cases'] if c['status'] == 'FAIL'],
                     'unmapped': data['summary']['unmapped_sub_case_failures'],
                     'release_status': data['release_status']})
(outdir / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')
print(json.dumps(rows, indent=2))
