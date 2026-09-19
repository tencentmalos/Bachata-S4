import sys,json,collections
from pathlib import Path
sys.path.insert(0,'/Users/bytedance/workspace/spatial_mcp_publish/dev_tools/mcp/guest_profiling')
import profile_batch as b
root=Path(__file__).resolve().parent
m,t,p,spans,frames,invalid=b.load_capture(Path('build/frame-recompile-20260916/capture-v2/capture.json'))
r=json.loads(Path('build/frame-intercepts-20260916/installed-mcp-frames/frame-workset.json').read_text())
print('keys',r.keys(), 'integrity',invalid,'threads',getattr(t,'threads',None))
print('spans',len(spans),'frames',len(frames))
# Screenshot rounds the visible parent and child duration to three decimals.
matched=[]
for f in r['frames']:
 for c in f['calls']:
  if round(c['inclusive_ns']/1e6,3)==19.524:
   matched.append((f,c))
print('matches',len(matched))
report={'capture':m,'integrity':invalid,'matches':[]}
for f,c in matched:
 print('FRAME',f['index'],f['elapsed_ns']/1e6,'CALL',c)
 a,z=c['start_ns'],c['end_ns']
 nearby=[s for s in spans if s['kind']!='guest' and s['end_ns']>a and s['start_ns']<z]
 groups=collections.defaultdict(list)
 for s in nearby:groups[(s['host_tid'],s['name'])].append((max(a,s['start_ns']),min(z,s['end_ns'])))
 summary=sorted([dict(tid=tid,name=n,overlap_ms=b.union(v)/1e6,count=len(v)) for (tid,n),v in groups.items()],key=lambda x:-x['overlap_ms'])
 print('overlap',json.dumps(summary[:40],indent=2))
 report['matches'].append(dict(index=f['index'],frame_start_ns=f['start_ns'],call=c,overlap=summary,spans=nearby,calls=f['calls'][-12:]))
# Submit wrapper from the canonical binding, one per frame.
retained={(f['host_tid'],f['start_ns'],f['end_ns']) for f in r['frames']}
lo=min(f['start_ns'] for f in r['frames']);hi=max(f['end_ns'] for f in r['frames'])
submits=[s for s in spans if s['kind']=='guest' and s['name']=='submit' and s['start_ns']>=lo and s['end_ns']<=hi]
hosts=[s for s in spans if s['kind']=='host']
critical=[s for s in hosts if s['name'] in ['GNM.SubmissionGate','Vulkan.SubmitLock','Vulkan.Submit','VideoOut.Prepare','Vulkan.CompletionWait','Present.DriverCall','Vulkan.NextCommandBuffer','Vulkan.RefreshTimeline']]
stats=[]
for c in submits:
 a,z=c['start_ns'],c['end_ns'];groups=collections.defaultdict(list)
 for s in critical:
  if s['kind']!='host' or s['end_ns']<=a or s['start_ns']>=z:continue
  if s['name'] in ['GNM.SubmissionGate','Vulkan.SubmitLock','Vulkan.Submit','VideoOut.Prepare','Vulkan.CompletionWait','Present.DriverCall','Vulkan.NextCommandBuffer','Vulkan.RefreshTimeline']:
   groups[s['host_tid'],s['name']].append((max(a,s['start_ns']),min(z,s['end_ns'])))
 stats.append(dict(duration_ms=(z-a)/1e6,tid=c['host_tid'],start_ns=a,end_ns=z,overlaps=[dict(tid=k[0],name=k[1],ms=b.union(v)/1e6) for k,v in groups.items()]))
report['submit_windows']=stats
report['selected_guest_details']=[]
selected=matched[0][0]
for call in selected['calls']:
 if call.get('entry_offset') not in (22341264,252544,103984):continue
 a,z=call['start_ns'],call['end_ns'];group=collections.defaultdict(list)
 for h in hosts:
  if h['host_tid']==call['host_tid'] and h['start_ns']<z and h['end_ns']>a:group[h['name']].append((max(a,h['start_ns']),min(z,h['end_ns'])))
 detail=dict(name=call.get('semantic_name',call['name']),entry_offset=call['entry_offset'],elapsed_ms=(z-a)/1e6,host_intersections=sorted([dict(name=n,count=len(v),union_ms=b.union(v)/1e6) for n,v in group.items()],key=lambda d:-d['union_ms']))
 report['selected_guest_details'].append(detail)
 print('guest_detail',detail)
(root/'analysis.json').write_text(json.dumps(report,indent=2)+'\n')
print('stats windows',len(stats))
print('host names',collections.Counter(s['name'] for s in spans if s['kind']=='host').most_common(60))
