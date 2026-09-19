import subprocess,time,json,re
from pathlib import Path
out=Path(__file__).resolve().parent/'phases';out.mkdir(exist_ok=True);adb=['adb','-s','9c2841a4'];service=['shell','dumpsys','activity','service','com.shadps4.android/.service.FexSessionService']
def call(args): return subprocess.check_output(adb+args,timeout=15)
def state(label):
 raw=call(service).decode();(out/(label+'.txt')).write_text(raw)
 def value(k):return re.search(r'^\s*'+k+r':\s*(\S+)',raw,re.M)[1]
 return dict(identity=[int(value('pid')),int(value('generation')),value('run_uuid')],ns=int(value('snapshot_ns')),flip=int(value('guest_flip')))
a=state('profile-before');pid=a['identity'][0]
assert pid==8463 and a['identity'][1]==1
(out/'scene-before.png').write_bytes(call(['exec-out','screencap','-p','-d','4630946441858561667']))
log=(out/'simpleperf-record.log').open('w')
proc=subprocess.Popen(adb+['shell','run-as','com.shadps4.android','/system/bin/simpleperf','record','-p',str(pid),'-e','cpu-clock:u','-f','100','--call-graph','fp','--duration','40','-o','files/rooftop-phases-perf.data'],stdout=log,stderr=subprocess.STDOUT)
try:
 (out/'capture-start.txt').write_bytes(call(service+['profiler_capture','file','128','35']))
 gpu=[]
 for _ in range(20):
  raw=call(['shell','cat /sys/class/kgsl/kgsl-3d0/gpubusy /sys/class/kgsl/kgsl-3d0/gpuclk']).decode().split()
  gpu.append(dict(time=time.time(),busy=int(raw[0]),total=int(raw[1]),hz=int(raw[2])));time.sleep(2)
 proc.wait(timeout=20)
 if proc.returncode: raise RuntimeError('simpleperf failed')
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
 log.close()
b=state('profile-after');assert a['identity']==b['identity']
(out/'gpu-util.json').write_text(json.dumps(gpu,indent=2))
finish=call(service+['profiler_capture','status']).decode();(out/'capture-finish.txt').write_text(finish)
assert 'capture_active=0' in finish
path=re.search(r'capture_path=(.+)',finish)[1]
(out/'scene.prof').write_bytes(call(['exec-out','run-as','com.shadps4.android','cat',path]))
(out/'perf.data').write_bytes(call(['exec-out','run-as','com.shadps4.android','cat','files/rooftop-phases-perf.data']))
(out/'maps.txt').write_bytes(call(['exec-out','run-as','com.shadps4.android','cat',f'/proc/{pid}/maps']))
(out/'scene-after.png').write_bytes(call(['exec-out','screencap','-p','-d','4630946441858561667']))
summary=dict(before=a,after=b,seconds=(b['ns']-a['ns'])/1e9,fps=(b['flip']-a['flip'])/((b['ns']-a['ns'])/1e9))
(out/'gameplay-counters.json').write_text(json.dumps(summary,indent=2));print(summary,flush=True)
