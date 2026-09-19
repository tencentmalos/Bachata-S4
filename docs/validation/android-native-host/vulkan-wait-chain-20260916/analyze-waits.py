import json,re,collections,statistics
from pathlib import Path
p=Path(__file__).resolve().parent
wanted={13992,14012,14014,14016,14018,14043}
line_re=re.compile(r'^\s*(.*)-(\d+)\s+\[(\d+)\]\s+\S+\s+(\d+\.\d+):\s+(sched_\w+):\s+(.*)$')
switch_re=re.compile(r'prev_comm=(.*?) prev_pid=(\d+) prev_prio=\d+ prev_state=(\S+) ==> next_comm=(.*?) next_pid=(\d+)')
wake_re=re.compile(r'comm=(.*?) pid=(\d+) prio=\d+ target_cpu=\d+')
running={};sleeping={};oncpu=collections.defaultdict(list);offcpu=collections.defaultdict(list);names={};blocked=[];ts_all=[]
for line in (p/'sched2.txt').read_text().splitlines():
 m=line_re.match(line)
 if not m:continue
 name,waker,core,ts,kind,body=m.groups();waker,core=int(waker),int(core);ns=round(float(ts)*1e9);ts_all.append(ns);names[waker]=name
 if kind=='sched_switch':
  m=switch_re.match(body)
  if not m:continue
  pn,prev,state,nn,nxt=m.groups();prev,nxt=int(prev),int(nxt);names[prev]=pn;names[nxt]=nn
  old=running.get(core)
  if old and old[0]==prev and prev in wanted:oncpu[prev].append([old[1],ns])
  running[core]=(nxt,ns)
  if prev in wanted:sleeping[prev]=dict(start=ns,state=state,wakes=[])
  if nxt in wanted and nxt in sleeping:
   r=sleeping.pop(nxt);r['end']=ns;offcpu[nxt].append(r)
 elif kind=='sched_wakeup':
  m=wake_re.match(body)
  if m:
   wn,tid=m.groups();tid=int(tid);names[tid]=wn
   if tid in sleeping:sleeping[tid]['wakes'].append(dict(time=ns,tid=waker,name=name))
 elif kind=='sched_blocked_reason':
  if any('pid='+str(t)+' ' in body for t in wanted):blocked.append(dict(ns=ns,event=body))
j=json.loads((p/'handoff2.json').read_text());opened={};spans=[]
for time,tid,obj,token,value,kind,name,wait in j['events']:
 if kind=='begin':opened[token]=(time,tid,obj,value,name)
 elif kind=='end' and token in opened:
  a,t,o,v,n=opened.pop(token);spans.append(dict(start=a,end=time,tid=t,obj=o,related=v,name=n))
def overlap(a,b,c,d):return max(0,min(b,d)-max(a,c))
def merge(seq):
 out=[]
 for a,b in sorted(seq):
  if out and a<=out[-1][1]:out[-1][1]=max(b,out[-1][1])
  else:out.append([a,b])
 return out
out={'trace_bounds_ns':[min(ts_all),max(ts_all)],'trace_header':(p/'sched2.txt').read_text()[:160], 'blocked_reasons':collections.Counter(r['event'] for r in blocked),'threads':[],'scope_scheduling':[]}
lo,hi=out['trace_bounds_ns']
for tid in sorted(wanted):
 v=offcpu[tid];by=collections.Counter();states=collections.Counter()
 for r in v:
  states[r['state']]+=(r['end']-r['start'])/1e6
  if r['wakes']:by[(r['wakes'][0]['tid'],r['wakes'][0]['name'])]+=(r['wakes'][0]['time']-r['start'])/1e6
 out['threads'].append(dict(tid=tid,name=names.get(tid),on_cpu_ms=sum(b-a for a,b in oncpu[tid])/1e6,off_cpu_ms=dict(states),sleep_until_first_waker_ms=[dict(tid=t,name=n,ms=v) for (t,n),v in by.most_common()]))
for tid in [14012,14043]:
 for name in ['Vulkan.SubmitLock','Vulkan.Submit','Present.DriverCall']:
  sv=[r for r in spans if r['tid']==tid and r['name']==name and lo<=r['start']<r['end']<=hi]
  ons=offs=0;wakers=collections.Counter();holders=0
  for r in sv:
   a,b=r['start'],r['end'];ons+=sum(overlap(a,b,c,d) for c,d in oncpu[tid]);offs+=sum(overlap(a,b,w['start'],w['end']) for w in offcpu[tid])
   for w in offcpu[tid]:
    if w['wakes']:
     w0=w['wakes'][0];wakers[(w0['tid'],w0['name'])]+=overlap(a,b,w['start'],w0['time'])/1e6
   hs=merge([[max(a,v['start']),min(b,v['end'])] for v in spans if v['tid']!=tid and v['name'] in ['Vulkan.Submit','Present.DriverCall'] and overlap(a,b,v['start'],v['end'])])
   holders+=sum(d-c for c,d in hs)
  out['scope_scheduling'].append(dict(tid=tid,name=name,calls=len(sv),wall_ms=sum(r['end']-r['start'] for r in sv)/1e6,on_cpu_ms=ons/1e6,off_cpu_ms=offs/1e6,overlap_other_queue_holder_ms=holders/1e6,wakers=[dict(tid=t,name=n,sleep_ms=v) for (t,n),v in wakers.most_common()]))
(p/'scheduling-analysis.json').write_text(json.dumps(out,indent=2))
(p/'sched-target-intervals.json').write_text(json.dumps(dict(on_cpu=oncpu,off_cpu=offcpu,handoff=spans)))
print(json.dumps(out,indent=2))
