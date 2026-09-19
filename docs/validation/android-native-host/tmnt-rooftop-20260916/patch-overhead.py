import subprocess,time,re,json
from pathlib import Path
p=Path(__file__).resolve().parent
adb=['adb','-s','9c2841a4','shell','dumpsys','activity','service','com.shadps4.android/.service.FexSessionService']
def snap():
 s=subprocess.check_output(adb).decode();g=subprocess.check_output(adb+['guest_patch','status']).decode()
 def f(k):return re.search(r'^\s*'+k+r':\s*(\S+)',s,re.M)[1]
 assert f('pid')=='8463' and f('generation')=='1'
 return dict(ns=int(f('snapshot_ns')),flip=int(f('guest_flip')),sdk_calls=int(re.search('sdk_calls: (\d+)',g)[1]))
rows=[]
try:
 for state in ['enable','disable','enable']:
  output=subprocess.check_output(['scripts/android/guest-patch','9c2841a4',state]).decode();assert 'failed: 1' not in output
  time.sleep(1)
  a=snap();time.sleep(15);b=snap()
  rows.append(dict(state=state,before=a,after=b,fps=(b['flip']-a['flip'])*1e9/(b['ns']-a['ns'])))
  (p/'patch-overhead.json').write_text(json.dumps(rows,indent=2));print(rows[-1],flush=True)
finally:subprocess.run(['scripts/android/guest-patch','9c2841a4','enable'],check=True,stdout=subprocess.DEVNULL)
