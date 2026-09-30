import collections,gzip,json,re,sys
from pathlib import Path
p=Path(sys.argv[1]);hist=collections.Counter();ticks=[];faults=[];creates=[]
rx=re.compile(r'\s([0-9]+\.[0-9]+): (\w+):')
with gzip.open(p/'trace/kgsl_trace.txt.gz','rt') as f:
 for line in f:
  m=rx.search(line)
  if not m: continue
  if m[2]=='adreno_preempt_done':
   v=re.search(r'to id=(\d+) from id=(\d+) level=([0-9a-f]+)',line)
   if v:hist[f'{v[2]}->{v[1]} level={v[3]}']+=1
  if m[2]=='adreno_gpu_fault':faults.append(line.strip())
  if m[2]=='kgsl_context_create':creates.append(line.strip())
v={'histogram_all_retained_events':dict(sorted(hist.items())),'faults':faults,'context_create':creates,'limitation':'Ringbuffer event counts are not uniquely attributable to one process; level is the raw GMU event field, not a proven hardware preemption-level decode.'}
(p/'preempt-summary.json').write_text(json.dumps(v,indent=2));print(json.dumps(v,indent=2))
