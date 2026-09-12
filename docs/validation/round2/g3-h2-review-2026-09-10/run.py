import subprocess,json,hashlib,time
from pathlib import Path
p=Path(__file__).parent;adb=['adb','-s','9c2841a4'];remote='/data/local/tmp/shadps4-g3-h2-review-20260910'
subprocess.run(adb+['shell','mkdir','-p',remote],check=True)
runs=[]
for name,binary,args in [('guest',p/'build/backend/guest_execution_tests',[]),('contract',p/'build/backend/guest_cpu_contract_tests',[]),*[(n,p/'build/review_probe',[n]) for n in ['unknown','rejected','fp','exception']]]:
    dest=remote+'/'+name
    subprocess.run(adb+['push',str(binary),dest],check=True,capture_output=True)
    subprocess.run(adb+['shell','chmod','755',dest],check=True,capture_output=True)
    sha=hashlib.sha256(binary.read_bytes()).hexdigest();device=subprocess.run(adb+['shell','sha256sum',dest],check=True,capture_output=True,text=True).stdout.split()[0];assert sha==device
    cmd=adb+['shell','timeout','60',dest,*args];start=time.monotonic()
    r=subprocess.run(cmd,capture_output=True,text=True,timeout=70)
    (p/(name+'.txt')).write_text(r.stdout+r.stderr)
    runs.append(dict(name=name,command=cmd,exit_code=r.returncode,elapsed=time.monotonic()-start,sha256=sha,deployed_sha256=device))
    (p/'runs.json').write_text(json.dumps(runs,indent=2)+'\n')
    print(name,r.returncode,(r.stdout+r.stderr)[-650:],flush=True)
subprocess.run(adb+['shell','rm','-rf',remote],check=True)
