import importlib.util, sys, json, collections, statistics
from pathlib import Path
out = Path(__file__).resolve().parent
sdk = Path('foundation/third_party/profiler_sdk/skills/spatial-trace-analyzer/scripts/decoder.py')
spec = importlib.util.spec_from_file_location('sdk_decoder', sdk)
d = importlib.util.module_from_spec(spec); sys.modules[spec.name] = d; spec.loader.exec_module(d)
r = d.read_file(out / 'tmnt-gpu-ring.prof')
pending, zones = {}, []
for e in r.events:
    key = e.gpu_ctx, e.gpu_slot
    if e.kind == 'GpuZoneBegin': pending[key] = e
    elif e.kind == 'GpuZoneEnd':
        b = pending.pop(key, None)
        if b and e.ts_ns >= b.ts_ns:
            zones.append(dict(ctx=b.gpu_ctx, name=b.name, start=b.ts_ns, end=e.ts_ns, ms=(e.ts_ns-b.ts_ns)/1e6))
def stats(values):
    v = sorted(values)
    return dict(n=len(v), mean=statistics.mean(v), p50=v[len(v)//2], p95=v[int((len(v)-1)*.95)], max=max(v)) if v else {}
groups = collections.defaultdict(list)
for z in zones: groups[z['name']].append(z['ms'])
assert 'GPU.GuestRenderPass' in groups
full = []
for z in zones:
    if z['name'] != 'GPU.GuestCommands' or z['ms'] < 10: continue
    inside = sorted((t for t in zones if t['name']=='GPU.GuestRenderPass' and t['ctx']==z['ctx'] and t['start']>=z['start'] and t['end']<=z['end']), key=lambda t:t['start'])
    end, covered, gaps = z['start'], 0, []
    for i, t in enumerate(inside):
        if t['start'] > end: gaps.append(dict(before_pass=i, ms=(t['start']-end)/1e6))
        covered += max(0, t['end']-max(end,t['start'])); end=max(end,t['end'])
    if end < z['end']: gaps.append(dict(before_pass='tail', ms=(z['end']-end)/1e6))
    full.append(dict(guest_ms=z['ms'], pass_count=len(inside), rendering_ms=covered/1e6, outside_rendering_ms=z['ms']-covered/1e6, largest_gaps=sorted(gaps,key=lambda g:-g['ms'])[:3]))
result = dict(gpu_only=True, same_context_interval_coverage=True, not_same_run_as_rdc=True,
    all_groups={k:stats(v) for k,v in groups.items()},
    guest_duration_buckets={'under_1ms':sum(v<1 for v in groups['GPU.GuestCommands']), '1_to_10ms':sum(1<=v<10 for v in groups['GPU.GuestCommands']), 'at_least_10ms':len(full)},
    full_batch_selection='GPU.GuestCommands >= 10ms; explicit bimodal split, not CPU frame identities',
    full_batches_summary={k:stats(x[k] for x in full) for k in ['guest_ms','pass_count','rendering_ms','outside_rendering_ms']}, full_batches=full)
(out/'gpu-batch-coverage.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result['full_batches_summary'],indent=2))
print('first_three:',json.dumps(full[:3],indent=2))
