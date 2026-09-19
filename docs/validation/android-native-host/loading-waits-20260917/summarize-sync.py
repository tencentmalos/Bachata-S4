import json,collections,re
from pathlib import Path
root=Path(__file__).resolve().parent;r=json.loads((root/'sync.json').read_text())
pending={};completed=[];bad=collections.Counter()
for e in r['phases']:
 k=e['CondObject'],e['WaitOwner'];p=e['CondPhase']
 if p==0:
  if k in pending:bad['replaced_enqueued']+=1
  pending[k]={0:e}
 elif k not in pending:bad['no_enqueue']+=1
 elif p in pending[k]:bad['duplicate_phase']+=1;pending.pop(k)
 else:
  pending[k][p]=e
  if p==3:
   q=pending.pop(k)
   if 2 not in q:bad['no_resume']+=1;continue
   completed.append(q)
def snap(i):return int(re.search(r'snapshot_ns: (\d+)',(root/f'diagnostic-run/frame-{i:04}.txt').read_text())[1])
b={v['name']:v['t'] for v in r['bookmarks']}
windows={'startup':(b['Session.Generation.1'],b['Startup.FirstGuestPresent']), 'loading_observation':(snap(6),snap(10)), 'whole':(0,2**63-1)}
main_owner=r['owners']['14271']['OwnerHandle']
def duration(a,b,window):return max(0,min(b,window[1])-max(a,window[0]))
out={'sha256':r['sha256'],'events':r['events'],'bad_pairs':dict(bad),'open_waits':len(pending),'owners':r['owners'],'windows':{},'main_hle_top':sorted([v for v in r['rows'] if v['tid']==14271],key=lambda v:v['elapsed_ns'],reverse=True)[:12]}
for label,w in windows.items():
 groups={};notifiers={}
 for q in completed:
  a,n,z=q[0],q.get(1),q[2];end=q[3]
  if a['WaitOwner']!=main_owner:continue
  t=duration(a['t'],end['t'],w)
  if not t:continue
  k=a['CondObject'];g=groups.setdefault(k,dict(condition=hex(k),count=0,total_ms=0,sleep_ms=0,reacquire_ms=0,pre_notify_ms=0,post_notify_ms=0,notified=0,unnotified=0,max_ms=0))
  g['count']+=1;g['total_ms']+=t/1e6;g['sleep_ms']+=duration(a['t'],z['t'],w)/1e6;g['reacquire_ms']+=duration(z['t'],end['t'],w)/1e6;g['max_ms']=max(g['max_ms'],t/1e6)
  if n:
   g['notified']+=1;g['pre_notify_ms']+=duration(a['t'],n['t'],w)/1e6;g['post_notify_ms']+=duration(n['t'],z['t'],w)/1e6
   key=(n['tid'],n.get('Caller'),n.get('Parent0'),n.get('Parent1'))
   ng=notifiers.setdefault(key,dict(tid=n['tid'],caller=hex(n.get('Caller',0)),parent0=hex(n.get('Parent0',0)),parent1=hex(n.get('Parent1',0)),count=0,covered_wait_ms=0));ng['count']+=1;ng['covered_wait_ms']+=t/1e6
  else:g['unnotified']+=1
 out['windows'][label]=dict(start_ns=w[0],end_ns=w[1],conditions=list(groups.values()),notifiers=sorted(notifiers.values(),key=lambda x:x['covered_wait_ms'],reverse=True))
# Fixed stride mutex records locate callers only. Bucket by checked caller parent.
mutex={}
for v in r['rows']:
 if v['tid']!=14271 or 'mutex' not in v['name'].lower():continue
 f=v['fields'];key=v['name'],f.get('Caller'),f.get('Parent0'),f.get('Arg0')
 g=mutex.setdefault(key,dict(name=v['name'],caller=hex(f.get('Caller',0)),parent=hex(f.get('Parent0',0)),object_slot=hex(f.get('Arg0',0)),samples=0));g['samples']+=v['count']
out['main_mutex_locators']=sorted(mutex.values(),key=lambda g:g['samples'],reverse=True)[:20]
(root/'sync-summary.json').write_text(json.dumps(out,indent=2))
print(json.dumps({k:v for k,v in out.items() if k not in ('owners','main_hle_top')},indent=2))
