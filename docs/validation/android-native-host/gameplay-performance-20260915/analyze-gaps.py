"""Analysis-only decoder adapter; production SDK is unchanged.

A writer (0,1) marker means dropped events for current ThreadContext. Insert
an analysis-only gap record, preserve wire order and discard that thread's
open spans before pairing new events. No span may cross a marked gap.
"""
import sys,types,json,collections,statistics,hashlib
from pathlib import Path
root=Path(__file__).resolve().parent
sdk=Path.cwd()/'foundation/third_party/profiler_sdk/skills/spatial-trace-analyzer/scripts/decoder.py'
source=sdk.read_text()
a='''            result.diagnostics.append("invalid zero-sized chunk")
            continue'''
b='''            result.diagnostics.append("invalid zero-sized chunk")
            if compressed_size != 0 or uncompressed_size != 1:
                raise ValueError("Unsupported malformed chunk; refusing resync")
            chunks.append(0xEF)  # analysis-only marker; never written to the PROF
            continue'''
assert source.count(a)==1
source=source.replace(a,b)
a='''            if tag == TAG_STRING_DEF:'''
b='''            if tag == 0xEF:
                open_spans.pop(current_tid, None)
                result.events.append(DecodedEvent(kind="Gap", tid=current_tid))
            elif tag == TAG_STRING_DEF:'''
assert source.count(a)==1
source=source.replace(a,b).replace('    result.events.sort(key=lambda event: event.ts_ns)','    # Preserve per-thread wire order, including gaps.')
module=types.ModuleType('gap_decoder');sys.modules[module.__name__]=module
exec(compile(source,str(sdk), 'exec'),module.__dict__)
for name in sys.argv[1:] or ['overlap','scene']:
 r=module.read_file(root/(name+'.prof'))
 stacks=collections.defaultdict(list);values=collections.defaultdict(list);gaps=[];last={};ends=[];times=[];orphan=collections.Counter();crossed=collections.Counter()
 for e in r.events:
  if e.kind=='Gap':
   gaps.append(dict(tid=e.tid,previous_ns=last.get(e.tid),discarded=[s[0] for s in stacks[e.tid]]))
   crossed[e.tid]+=len(stacks[e.tid]);stacks[e.tid].clear();continue
  if e.ts_ns: times.append(e.ts_ns);last[e.tid]=e.ts_ns
  if e.kind=='SpanBegin':stacks[e.tid].append([e.name,e.ts_ns,0])
  elif e.kind=='SpanEnd':
   if not stacks[e.tid]:orphan[e.tid]+=1;continue
   n,t,child=stacks[e.tid].pop();d=e.ts_ns-t
   if d<0:raise ValueError('nonmonotonic span')
   values[e.tid,n].append((d,max(0,d-child)))
   if stacks[e.tid]:stacks[e.tid][-1][2]+=d
   if e.tid==8285 and n=='HLE.sceGnmSubmitAndFlipCommandBuffers':ends.append(e.ts_ns)
 def stats(v):
  s=sorted(v);return dict(count=len(v),sum_s=sum(v)/1e9,mean_ms=statistics.mean(v)/1e6,p50_ms=s[len(s)//2]/1e6,p95_ms=s[int((len(s)-1)*.95)]/1e6,max_ms=max(v)/1e6)
 spans=[dict(tid=k[0],name=k[1],**stats([d for d,x in v]),exclusive_s=sum(x for d,x in v)/1e9) for k,v in values.items()]
 out=dict(decoder_sha256=hashlib.sha256(sdk.read_bytes()).hexdigest(),input_sha256=hashlib.sha256((root/(name+'.prof')).read_bytes()).hexdigest(),pid=r.process_id,event_count=len(r.events),chunks_ok=r.chunks_ok,gaps=gaps,orphan_ends=dict(orphan),discarded_open_spans=dict(crossed),duration_s=(max(times)-min(times))/1e9,spans=spans)
 (root/(name+'-gap-aware.json')).write_text(json.dumps(out,indent=2))
 print(name, 'gaps',gaps)
 for s in spans:
  if s['name']=='VideoOut.Prepare' or (s['tid']==8300 and s['name'] in ['Present.FreeFrameWait','Present.FrameFenceWait','Vulkan.Submit']) or s['name'] in ['Audio.QueueWait','HLE.sceAudioOutOutputs']:print(s)
