import sys,json,collections,statistics
from pathlib import Path
sys.path.insert(0,str(Path.cwd()/"foundation/third_party/profiler_sdk/skills/spatial-trace-analyzer/scripts"))
import decoder
root=Path(__file__).resolve().parent
r=decoder.read_file(root/"overlap.prof")
stacks=collections.defaultdict(list);data=collections.defaultdict(list);ends=[];times=[]
for e in r.events:
 if e.ts_ns:times.append(e.ts_ns)
 if e.kind=="SpanBegin":stacks[e.tid].append((e.name,e.ts_ns))
 elif e.kind=="SpanEnd" and stacks[e.tid]:
  n,t=stacks[e.tid].pop()
  if e.ts_ns>=t:
   data[e.tid,n].append(e.ts_ns-t)
   if e.tid==8285 and n=="HLE.sceGnmSubmitAndFlipCommandBuffers":ends.append(e.ts_ns)
def stats(v):
 s=sorted(v)
 return dict(count=len(v),sum_s=sum(v)/1e9,mean_ms=statistics.mean(v)/1e6,p50_ms=s[len(s)//2]/1e6,p95_ms=s[int((len(s)-1)*.95)]/1e6,max_ms=max(v)/1e6)
summary=dict(pid=r.process_id,events=len(r.events),chunks=r.chunks_ok,skipped=r.chunks_skipped,diagnostics=r.diagnostics,time_range=[min(times),max(times)],duration_s=(max(times)-min(times))/1e9,game_frames=len(ends),frame_intervals=stats([b-a for a,b in zip(ends,ends[1:])]))
summary["spans"]=[dict(tid=k[0],name=k[1],**stats(v)) for k,v in data.items()]
(root/"overlap-summary.json").write_text(json.dumps(summary,indent=2))
print({k:v for k,v in summary.items() if k not in ["spans","diagnostics"]})
for s in summary["spans"]:
 if s['name'] in ['VideoOut.Prepare','Present.FrameFenceWait','Present.FreeFrameWait','Vulkan.Submit','HLE.pthread_mutex_lock','HLE.scePthreadMutexLock','HLE.sem_wait']:print(s)
