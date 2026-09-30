from pathlib import Path
import sys,json,collections,statistics,hashlib
root=Path('/Users/bytedance/workspace/emulations/ps4/shadps4')
sys.path.insert(0,str(root/'foundation/third_party/profiler_sdk/skills/spatial-trace-analyzer/scripts'))
import decoder

def stats(v):
    v=sorted(v)
    return dict(count=len(v),total_ms=sum(v),mean_ms=statistics.mean(v),p50_ms=v[int((len(v)-1)*.5)],p95_ms=v[int((len(v)-1)*.95)],max_ms=max(v)) if v else {}
for arg in sys.argv[1:]:
    path=Path(arg); data=decoder.read_file(path)
    stack=collections.defaultdict(list); scopes=collections.defaultdict(list); frames=[]; names={}; counters=collections.defaultdict(list); unmatched=0
    for e in data.events:
        names[e.tid]=e.thread_name
        if e.kind=='SpanBegin': stack[e.tid].append(e)
        elif e.kind=='SpanEnd':
            if not stack[e.tid]: unmatched+=1;continue
            b=stack[e.tid].pop()
            if b.name != e.name: raise RuntimeError('span mismatch')
            if e.ts_ns < b.ts_ns: raise RuntimeError('negative duration')
            scopes[(b.name,b.tid)].append((e.ts_ns-b.ts_ns)/1e6)
        elif e.kind=='FrameMark': frames.append(e.ts_ns)
        elif e.kind=='Counter' and any(n in e.name.lower() for n in ['flip','frame','fps']): counters[e.name].append(e.i64)
    frames=sorted(set(frames))
    selected={f'{n} [tid={tid} {names.get(tid)}]':stats(v) for (n,tid),v in scopes.items() if n.startswith(('XR.', 'OpenXR ImGui','OpenXR Scene','Present.')) or 'Voxel' in n}
    result=dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),sdk_source='0b467a861569345a64fd88cb2e5daa8ff542c14f',pid=data.process_id,events=len(data.events),chunks_ok=data.chunks_ok,chunks_skipped=data.chunks_skipped,truncated=data.truncated,diagnostics=data.diagnostics,unmatched_end=unmatched,unclosed_begin=sum(map(len,stack.values())),frames=stats([(b-a)/1e6 for a,b in zip(frames,frames[1:])]),frame_cadence_fps=(len(frames)-1)*1e9/(frames[-1]-frames[0]) if len(frames)>1 else None,scopes=selected,counters={n:dict(samples=len(v),first=v[0],last=v[-1]) for n,v in counters.items()},limits='Elapsed scopes, not on-CPU or GPU execution time. Capture-boundary scopes excluded. SDK frame marks are not headset refreshes.')
    path.with_suffix('.summary.json').write_text(json.dumps(result,indent=2))
    print(path.name,result['pid'],result['frame_cadence_fps'],result['frames'],selected)
