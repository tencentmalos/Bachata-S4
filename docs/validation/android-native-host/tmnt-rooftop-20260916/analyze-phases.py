import sys,types,json,collections,statistics
from pathlib import Path
sdk=Path('foundation/third_party/profiler_sdk/skills/spatial-trace-analyzer/scripts/decoder.py')
s=sdk.read_text()
a='''            result.diagnostics.append("invalid zero-sized chunk")
            continue'''
b='''            result.diagnostics.append("invalid zero-sized chunk")
            if compressed_size != 0 or uncompressed_size != 1:
                raise ValueError("unsupported zero chunk")
            chunks.append(0xEF)
            continue'''
assert s.count(a)==1;s=s.replace(a,b)
a='''            if tag == TAG_STRING_DEF:'''
b='''            if tag == 0xEF:
                open_spans.pop(current_tid, None)
                result.events.append(DecodedEvent(kind="Gap", tid=current_tid))
            elif tag == TAG_STRING_DEF:'''
assert s.count(a)==1;s=s.replace(a,b).replace('    result.events.sort(key=lambda event: event.ts_ns)','    # retain wire order')
m=types.ModuleType('gap_decoder');sys.modules[m.__name__]=m;exec(compile(s,str(sdk),'exec'),m.__dict__)
p=Path(sys.argv[1]);r=m.read_file(p)
starts={};stacks=collections.defaultdict(list);cpu=collections.defaultdict(list);phases=collections.defaultdict(list);gpu=collections.defaultdict(list);go={};gaps=[];orphan=collections.Counter();counters=collections.Counter(); gpu_counters=collections.defaultdict(list)
for e in r.events:
 if e.kind=='Gap':
  gaps.append(dict(tid=e.tid,discarded=len(stacks[e.tid])));stacks[e.tid].clear();continue
 if e.kind=='SpanBegin':stacks[e.tid].append(e)
 elif e.kind=='SpanEnd':
  if stacks[e.tid]:
   b=stacks[e.tid].pop()
   if e.ts_ns>=b.ts_ns: cpu[e.tid,b.name].append((b.ts_ns,e.ts_ns))
  else:orphan[e.tid]+=1
 elif e.kind=='Counter' and e.name.startswith('GuestPatch.'):
  counters[e.name]+=1
  if e.name.endswith('_begin_ns'):starts[e.tid,e.name[:-9]]=e.i64
  elif e.name.endswith('_elapsed_ns'):
   key=e.tid,e.name[:-11];start=starts.pop(key,None)
   if start is not None and e.i64>=0:phases[key].append((start,start+e.i64))
 elif e.kind=='Counter' and e.name.startswith('GPU.'):
  gpu_counters[e.name].append(e.i64)
 elif e.kind=='GpuZoneBegin':go[e.gpu_ctx,e.gpu_slot]=e
 elif e.kind=='GpuZoneEnd':
  b=go.pop((e.gpu_ctx,e.gpu_slot),None)
  if b and e.ts_ns>=b.ts_ns:gpu[b.name].append((b.ts_ns,e.ts_ns))
def stats(v):
 v=sorted(v);return dict(n=len(v),mean_ms=statistics.mean(v)/1e6,p50_ms=v[len(v)//2]/1e6,p95_ms=v[int((len(v)-1)*.95)]/1e6,max_ms=max(v)/1e6,sum_s=sum(v)/1e9) if v else {}
def union(a):
 end=0;total=0
 for lo,hi in sorted(a):
  total+=max(0,hi-max(lo,end));end=max(end,hi)
 return total
def merged(a):
 out=[]
 for lo,hi in sorted(a):
  if out and lo<=out[-1][1]:out[-1]=(out[-1][0],max(hi,out[-1][1]))
  else:out.append((lo,hi))
 return out
def intersect(a,b):
 a=merged(a);b=merged(b);i=j=0;out=[]
 while i<len(a) and j<len(b):
  lo,hi=a[i];s,e=b[j]
  if max(lo,s)<min(hi,e):out.append((max(lo,s),min(hi,e)))
  if hi<e:i+=1
  else:j+=1
 return out
phase_summary=[]
for (tid,name),v in phases.items():
 overlaps=[]
 for (ctid,cn),cv in cpu.items():
  if ctid!=tid:continue
  ns=union(intersect(v,cv))
  if ns:overlaps.append(dict(name=cn,ms_per_phase=ns/len(v)/1e6))
 hle=[item for (ctid,cn),cv in cpu.items() if ctid==tid and cn.startswith('HLE.') for item in intersect(v,cv)]
 phase_summary.append(dict(tid=tid,name=name,**stats([b-a for a,b in v]),hle_union_ms_per_phase=union(hle)/len(v)/1e6,host_overlap=sorted(overlaps,key=lambda x:-x['ms_per_phase'])[:16]))
out=dict(pid=r.process_id,chunks=r.chunks_ok,skipped=r.chunks_skipped,truncated=r.truncated,diagnostics=r.diagnostics,gaps=gaps,orphan_ends=dict(orphan),counters=dict(counters),phases=phase_summary,gpu={k:stats([b-a for a,b in v]) for k,v in gpu.items()},gpu_counters={k:stats(v) for k,v in gpu_counters.items()},cpu=[dict(tid=t,name=n,**stats([b-a for a,b in v])) for (t,n),v in cpu.items()])
p.with_suffix('.analysis.json').write_text(json.dumps(out,indent=2))
p.with_suffix('.intervals.json').write_text(json.dumps(dict(phases=[dict(tid=t,name=n,ranges=v) for (t,n),v in phases.items()],gpu=gpu)))
print(json.dumps({k:v for k,v in out.items() if k not in ['diagnostics','cpu']},indent=2))
