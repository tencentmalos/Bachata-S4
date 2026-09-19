import sys, collections, json, statistics
from pathlib import Path
sys.path.insert(0,str(Path('foundation/third_party/profiler_sdk/skills/spatial-trace-analyzer/scripts').resolve()))
import decoder
p=Path(__file__).resolve().parent
for name in sys.argv[1:]:
    r=decoder.read_file(p/(name+'.prof'))
    opens={}; gpu=collections.defaultdict(list); counters=collections.defaultdict(list)
    times=[]; contexts=collections.Counter(); bad=[]
    for e in r.events:
        if e.kind=='Counter' and e.name.startswith('GPU.'): counters[e.name].append(e.i64)
        if not e.kind.startswith('Gpu'):
            if e.ts_ns: times.append(e.ts_ns)
            continue
        contexts[e.gpu_ctx_name]+=1; key=(e.gpu_ctx,e.gpu_slot)
        if e.kind=='GpuZoneBegin': opens[key]=e
        elif e.kind=='GpuZoneEnd':
            b=opens.pop(key,None)
            if b is None: bad.append('orphan end'); continue
            gpu[b.name].append(e.ts_ns-b.ts_ns)
            if e.ts_ns<b.ts_ns: bad.append('negative')
    def stats(v):
        return {'n':len(v),'mean_ms':statistics.mean(v)/1e6,'p95_ms':sorted(v)[int((len(v)-1)*.95)]/1e6,'max_ms':max(v)/1e6}
    lo,hi=min(times)-1000000000,max(times)+1000000000
    outside=sum(1 for e in r.events if e.kind.startswith('Gpu') and not lo<=e.ts_ns<=hi)
    out={'pid':r.process_id,'events':len(r.events),'truncated':r.truncated,
         'diagnostics':r.diagnostics,'gpu_contexts':dict(contexts),
         'gpu_zones':{k:stats(v) for k,v in gpu.items()},'counters':{k:stats(v) for k,v in counters.items()},
         'gpu_bad':bad,'gpu_open':len(opens),'gpu_outside_cpu_range_1s':outside,
         'chunks_ok':r.chunks_ok,'chunks_skipped':r.chunks_skipped}
    (p/(name+'-analysis.json')).write_text(json.dumps(out,indent=2))
    print(name, json.dumps({k:out[k] for k in ['gpu_contexts','gpu_zones','gpu_bad','gpu_open','gpu_outside_cpu_range_1s']},indent=2),flush=True)
