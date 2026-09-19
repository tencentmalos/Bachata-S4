import json,statistics
from pathlib import Path
p=Path(__file__).resolve().parent/'phases';r=json.loads((p/'scene.intervals.json').read_text())
d={x['name'].rsplit('.',1)[-1]:x['ranges'] for x in r['phases'] if x['tid']==9112}
rows=[]
def overlap(a,b):return max(0,min(a[1],b[1])-max(a[0],b[0]))
for frame in d['frame'][1:-1]:
 begin=[x for x in d['begin'] if frame[0]<=x[0] and x[1]<=frame[1]]
 submit=[x for x in d['submit'] if frame[0]<=x[0] and x[1]<=frame[1]]
 fmod=[x for x in d['fmod'] if frame[0]<=x[0] and x[1]<=frame[1]]
 if len(begin)!=1 or len(submit)!=1 or len(fmod)!=1:continue
 b=begin[0];s=submit[0]
 row=dict(start_ns=frame[0],end_ns=frame[1],frame=frame[1]-frame[0],pre_render=b[0]-frame[0],begin=b[1]-b[0],draw_build=s[0]-b[1],submit=s[1]-s[0],tail=frame[1]-s[1],fmod=fmod[0][1]-fmod[0][0],jobs=sum(overlap(frame,x) for x in d['jobs']),jobs_pre=sum(overlap((frame[0],b[0]),x) for x in d['jobs']),jobs_draw=sum(overlap((b[1],s[0]),x) for x in d['jobs']))
 assert sum(row[k] for k in ['pre_render','begin','draw_build','submit','tail'])==row['frame']
 rows.append(row)
summary={}
for k in rows[0]:
 if k.endswith('_ns'):continue
 v=sorted(x[k] for x in rows);summary[k]=dict(n=len(v),mean_ms=statistics.mean(v)/1e6,p95_ms=v[int((len(v)-1)*.95)]/1e6,max_ms=max(v)/1e6)
(p/'frame-breakdown.json').write_text(json.dumps(dict(summary=summary,frames=rows),indent=2));print(json.dumps(summary,indent=2))
