import argparse,json,re,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('run');p.add_argument('action',choices=['status','pad','observe']);p.add_argument('--mask',type=int,default=0);p.add_argument('--id',type=int,default=1);p.add_argument('--hold',type=int,default=150);p.add_argument('--seconds',type=int,default=240);p.add_argument('--lx',type=float,default=0);p.add_argument('--ly',type=float,default=0);p.add_argument('--rx',type=float,default=0);p.add_argument('--ry',type=float,default=0);a=p.parse_args();out=Path(a.run);out.mkdir(parents=True,exist_ok=True)
adb=['adb','-s','PB3110PGL6240001G']
def call(args):return subprocess.check_output(adb+args,text=True,errors="replace",timeout=20)
def status():
 raw=call(['shell','dumpsys','activity','com.shadps4.android/.MainActivity','debugbus','debug_status']);d={}
 for k in ['pid','generation','run_uuid','stage','session','driver','guest_flip','queue_submit','host_present','snapshot_ns','stop_reason']:
  m=re.search(r'^'+k+r': (.*)$',raw,re.M)
  if m:d[k]=m.group(1)
 d['host_time_ns']=time.time_ns();d['faultcount']=call(['shell','cat','/sys/class/kgsl/kgsl-3d0/snapshot/faultcount']).strip();(out/'status-latest.txt').write_text(raw)
 with (out/'observations.jsonl').open('a') as f:f.write(json.dumps(d)+'\n')
 return d
if a.action=='status':print(json.dumps(status()))
elif a.action=='pad':
 d=status();assert d.get('stage')=='Running',d
 args=['shell','dumpsys','activity','com.shadps4.android/.MainActivity','debugbus','pad','state',d['pid'],d['generation'],d['run_uuid'],'cp-ab-'+out.name,str(a.id),'0',str(a.hold),str(a.mask),str(a.lx),str(a.ly),str(a.rx),str(a.ry),'0','0','0','0','0'];r=call(args)
 with (out/'actions.jsonl').open('a') as f:f.write(json.dumps({'host_time_ns':time.time_ns(),'args':args,'result':r})+'\n')
 print(r)
else:
 first=status();base=first['faultcount'];end=time.monotonic()+a.seconds
 print(json.dumps(first),flush=True)
 while time.monotonic()<end:
  time.sleep(min(10,max(0,end-time.monotonic())));d=status();print(json.dumps(d),flush=True)
  if d['faultcount']!=base or d.get('stage') in ['Failed','Stopped']:
   failures=[]
   captures=[('dmesg-fault.txt',['shell','dmesg']),('snapshot-files.txt',['shell','ls','-l','/data/vendor/gpu_snapshot']),('logcat-fault.txt',['shell','logcat','-d','-b','all','-t','4000'])]
   for name,args in captures:
    try:
     with (out/name).open('wb') as f:
      result=subprocess.run(adb+args,stdout=f,stderr=subprocess.PIPE,timeout=30)
     if result.returncode:failures.append({'file':name,'returncode':result.returncode,'stderr':result.stderr.decode(errors='replace')})
    except Exception as e:failures.append({'file':name,'error':repr(e)})
   (out/'fault-collection-errors.json').write_text(json.dumps(failures,indent=2))
   break
