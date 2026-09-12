import runpy
from pathlib import Path
root=Path('/Users/bytedance/workspace/emulations/ps4/shadps4')
n=runpy.run_path(str(root/'tests/guest_cpu/test_v0_runner.py'))
t=n['RunnerTests']()
SuiteRun=n['SuiteRun']
r=n['NS']
for sub in ('G25a','G26a','G27a','M27','M28','M29'):
    suite='guest_execution_tests' if sub.startswith('G') else 'guest_cpu_contract_tests'
    data,cases=t.run_report(device=lambda *a:SuiteRun(cases={sub:('FAIL','review injected failure')},exit_code=0))
    mapped=[key for key,ids in r['SUITE_MAP'].items() if sub in ids]
    print(sub, 'owned=',sub in r['sub_cases_owned_by'](suite),'mapped=',mapped,'report_failed=',data['summary']['failed'])
