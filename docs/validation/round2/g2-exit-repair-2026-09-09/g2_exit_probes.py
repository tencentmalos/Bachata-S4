import subprocess,json,time,hashlib
from pathlib import Path
work=Path('/tmp/shadps4-g2-exit-repair-20260909'); build=Path('/tmp/shadps4-g2-close-review-20260909/build')
adb=['adb','-s','01108YHE01017563']; remote='/data/local/tmp/shadps4-g2-exit-repair-20260909'
runs=[]
for name,binary,args,expected in [('NX',build/'permission_probe',[],0),('rewrite',build/'permission_probe',['rewrite'],0),('store-disabled',build/'guest_store_negative',[],1),('page',work/'build/host_page_size_probe',[],0)]:
    path=remote+'/'+name
    subprocess.run(adb+['push',str(binary),path],check=True,capture_output=True)
    subprocess.run(adb+['shell','chmod','755',path],check=True,capture_output=True)
    deployed=subprocess.run(adb+['shell','sha256sum',path],check=True,capture_output=True,text=True).stdout.split()[0]
    local=hashlib.sha256(binary.read_bytes()).hexdigest();assert deployed==local
    cmd=adb+['shell','timeout','45',path,*args];start=time.monotonic()
    p=subprocess.run(cmd,capture_output=True,text=True,timeout=55)
    (work/(name+'.txt')).write_text(p.stdout+p.stderr)
    runs.append(dict(name=name,command=cmd,expected_exit=expected,exit_code=p.returncode,elapsed=time.monotonic()-start,local_sha256=local,deployed_sha256=deployed))
    print(name,p.returncode,(p.stdout+p.stderr)[-450:],flush=True)
    assert p.returncode==expected
(work/'probes-runs.json').write_text(json.dumps(runs,indent=2)+'\n')
