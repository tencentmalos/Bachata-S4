from pathlib import Path
import subprocess,json,hashlib,time
work=Path('/tmp/shadps4-g2-exit-repair-20260909'); root=Path.cwd()
commands=[('runner-final',['python3','scripts/android/run-v0-tests','--build-dir',str(work/'host-build'),'--fex-build-dir',str(work/'build'),'--serial','01108YHE01017563','--expected-page-size','4096','--run-id','g2-exit-repair-final-20260909','--out',str(work/'results.json')]),('probes-final',['python3','/tmp/g2_exit_probes.py']),('runner-unit',['python3','tests/guest_cpu/test_v0_runner.py']),('runner-accounting',['python3','tests/runner/test-runner-accounting.py']),('runner-g1',['python3','tests/runner/test-g1-evidence.py'])]
runs=[]
for name,cmd in commands:
    start=time.monotonic()
    p=subprocess.run(cmd,capture_output=True,text=True,timeout=180)
    (work/(name+'.txt')).write_text(p.stdout+p.stderr)
    runs.append(dict(name=name,command=cmd,exit_code=p.returncode,elapsed=time.monotonic()-start))
    print(name,p.returncode,(p.stdout+p.stderr)[-250:],flush=True)
    assert p.returncode==0
(work/'final-runs.json').write_text(json.dumps(runs,indent=2)+'\n')
