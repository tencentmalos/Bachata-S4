import subprocess,json,re,time,sys
from pathlib import Path
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
expected=int(sys.argv[2]);adb=['adb','-s','9c2841a4'];svc='com.shadps4.android/.service.FexSessionService'
def call(*args):return subprocess.check_output(adb+list(args),timeout=20).decode()
def state():return call('shell','dumpsys','activity','service',svc)
def identity(s):return tuple(re.search(r'^\s*'+k+r':\s*(.+)$',s,re.M)[1].strip() for k in ['pid','generation','run_uuid'])
original=identity(state());summary=[]
for n in range(4):
 before=state();assert identity(before)==original and 'session: active' in before
 request=call('shell','dumpsys','activity','service',svc,'gpu_memory','request')
 rid=int(re.search(r'request_id=(\d+)',request)[1])
 for attempt in range(40):
  report=call('shell','dumpsys','activity','service',svc,'gpu_memory','status')
  if f'completed_id={rid} ' in report and 'pending=false' in report:break
  time.sleep(.25)
 else:raise RuntimeError('renderer snapshot not completed')
 assert int(re.search(r'scale_percent=(\d+)',report)[1])==expected
 mem=call('shell','dumpsys','meminfo','com.shadps4.android')
 system=call('shell','cat','/proc/meminfo')
 after=state();assert identity(after)==original
 for suffix,content in [('gpu',report),('meminfo',mem),('system',system),('state',after)]:
  (out/f'{n}-{suffix}.txt').write_text(content)
 total=re.search(r'TOTAL PSS:\s*(\d+).*?TOTAL RSS:\s*(\d+)',mem)
 used={k:int(re.search(r'^'+k+r':\s*(\d+)',system,re.M)[1]) for k in ['MemTotal','MemAvailable']}
 values={k:int(v) for k,v in re.findall(r'\b(vma_\w+bytes|cached_image_allocation_bytes|guest_buffer_allocation_bytes|utility_buffer_allocation_bytes)=(\d+)',report)}
 entry={'identity':original,'scale_percent':expected,'pss_kib':int(total[1]),'rss_kib':int(total[2]),'system_memavailable_used_kib':used['MemTotal']-used['MemAvailable'],**values}
 summary.append(entry);print(json.dumps(entry),flush=True)
 (out/'measurements.json').write_text(json.dumps(summary,indent=2)+'\n')
 if n<3:time.sleep(3)
(out/'scene.png').write_bytes(subprocess.check_output(adb+['exec-out','screencap','-p','-d','4630946441858561667'],timeout=20))
