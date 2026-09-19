import subprocess,time,json,re
from pathlib import Path
p=Path(__file__).resolve().parent; a=['adb','-s','9c2841a4']; d=['shell','dumpsys','activity','service','com.shadps4.android/.service.FexSessionService']
def call(x): return subprocess.check_output(a+x,timeout=15)
def state():
 raw=call(d).decode()
 def v(k): return re.search(r'^\s*'+k+r':\s*(\S+)',raw,re.M)[1]
 return dict(identity=[v('pid'),v('generation'),v('run_uuid')],ns=int(v('snapshot_ns')),flip=int(v('guest_flip')),queue=int(v('queue_submit')),overlay=int(v('overlay_redraw'))),raw
out=[]
try:
 for i,mode in enumerate(['show','hide','show']):
  print(call(d+['overlay',mode]).decode(),flush=True);time.sleep(3)
  before,raw=state();assert before['identity'][:2]==['10213','1'];(p/f'overlay-{i}-before.txt').write_text(raw)
  time.sleep(20)
  after,raw=state();assert before['identity']==after['identity'];(p/f'overlay-{i}-after.txt').write_text(raw)
  dt=(after['ns']-before['ns'])/1e9
  row=dict(mode=mode,before=before,after=after,seconds=dt,fps=(after['flip']-before['flip'])/dt,submits_per_s=(after['queue']-before['queue'])/dt,redraws_per_s=(after['overlay']-before['overlay'])/dt);out.append(row);print(row,flush=True)
  (p/'overlay-aba.json').write_text(json.dumps(out,indent=2))
finally: print(call(d+['overlay','show']).decode(),flush=True)
