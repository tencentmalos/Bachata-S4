import sqlite3,json,sys,collections,statistics
from pathlib import Path
p=Path(sys.argv[1]);out=Path(sys.argv[2]);db=sqlite3.connect(p.resolve().as_uri()+'?mode=ro',uri=True)
meta=json.loads(db.execute('select data from metadata').fetchone()[0]);out.mkdir(exist_ok=True,parents=True)
(out/'index-metadata.json').write_text(json.dumps(meta,indent=2))
lo,hi=db.execute("select min(ts),max(ts) from events where kind not like 'Gpu%' and ts>0").fetchone()
def stats(v,div=1):
 v=sorted(v)
 return dict(count=len(v),mean=statistics.mean(v)/div,p50=v[len(v)//2]/div,p95=v[int((len(v)-1)*.95)]/div,max=v[-1]/div,total=sum(v)/div) if v else {}
counters=collections.defaultdict(list)
for ts,name,val,tid in db.execute("select ts,json_extract(data,'$.name'),value,tid from events where kind='Counter' order by ts,id"):
 counters[name].append((ts,int(val),tid))
cs={}
for name,rows in counters.items():
 values=[r[1] for r in rows]; times=[r[0] for r in rows]
 cs[name]={'value_stats':stats(values),'first':rows[0],'last':rows[-1]}
 if name.startswith('VideoOut.'):
  changed=[row for i,row in enumerate(rows) if i==0 or row[1]!=rows[i-1][1]]
  cs[name].update(period_ms=stats([b[0]-a[0] for a,b in zip(changed,changed[1:]) if b[1]==a[1]+1],1e6),rate=(changed[-1][1]-changed[0][1])*1e9/(changed[-1][0]-changed[0][0]) if len(changed)>1 else None,rows=changed)
(out/'counter-summary.json').write_text(json.dumps(cs,indent=2))
# Only already paired, same-name, non-negative intervals admitted by Litep's index.
inclusive=collections.defaultdict(list);exclusive=collections.Counter();coverage=collections.Counter();ranges=collections.defaultdict(list)
thread=None;stack=[]
def finish():
 if not stack:return
 start,end,name,child=stack.pop();exclusive[thread,name]+=max(0,end-start-child)
 if stack:stack[-1][3]+=end-start
 else:coverage[thread]+=end-start
q="""select e.tid,json_extract(e.data,'$.name'),i.start,i.end from intervals i join events e on e.id=i.begin_id where e.kind='SpanBegin' order by e.tid,i.start,i.end desc,i.begin_id"""
for tid,name,start,end in db.execute(q):
 if tid!=thread:
  while stack:finish()
  thread=tid
 while stack and start>=stack[-1][1]:finish()
 if stack and end>stack[-1][1]:raise RuntimeError('crossing CPU intervals')
 stack.append([start,end,name,0]);inclusive[tid,name].append(end-start)
 if name.startswith(('HLE.','GNM.','Vulkan.','VideoOut.','Liverpool.')) and end-start>=2_000_000:
  ranges[tid,name].append([start,end])
while stack:finish()
summary={'start_ns':lo,'end_ns':hi,'duration_s':(hi-lo)/1e9,'thread_scope_coverage_ms':{t:n/1e6 for t,n in coverage.items()},'scopes':[dict(tid=t,name=n,exclusive_ms=exclusive[t,n]/1e6,**stats(v,1e6)) for (t,n),v in inclusive.items()]}
summary['scopes'].sort(key=lambda x:-x['total']);(out/'scope-summary.json').write_text(json.dumps(summary,indent=2))
(out/'long-intervals.json').write_text(json.dumps([dict(tid=t,name=n,ranges=v) for (t,n),v in ranges.items()]))
print('duration_s',summary['duration_s'],'scopes',len(summary['scopes']))
for x in summary['scopes'][:32]:print(x)
for n in cs:
 if n.startswith('VideoOut.') or n.startswith('GPU.GuestFrame'):print(n,cs[n]['value_stats'],cs[n].get('rate'),cs[n].get('period_ms'))
