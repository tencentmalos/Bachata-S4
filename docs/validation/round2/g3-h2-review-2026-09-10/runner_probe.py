import runpy,json
from pathlib import Path
root=Path.cwd();ns=runpy.run_path(str(root/'tests/guest_cpu/test_v0_runner.py'));t=ns['RunnerTests']();SuiteRun=ns['SuiteRun'];rows=[]
for sub in ['G30a','G30b','G31a','G31b','G31c','G32a','G32b']:
    data,cases=t.run_report(device=lambda *a:SuiteRun(cases={sub:('FAIL','review injected')},exit_code=0))
    rows.append(dict(sub=sub,owned=sub in ns['NS']['sub_cases_owned_by']('guest_execution_tests'),v0_failed=data['summary']['failed'],r2_failed=sum(x['status']=='FAIL' for x in data['round2']['cases'])))
print(json.dumps(rows,indent=2))
