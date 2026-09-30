from pathlib import Path
import gzip,re,collections,json,sys
p=Path(sys.argv[1]);first={};last={};events=[]
rx=re.compile(r'\[(\d+)\].*?\s([0-9]+\.[0-9]+): (\w+):')
with gzip.open(p/'trace/kgsl_trace.txt.gz','rt') as f:
 for l in f:
  m=rx.search(l)
  if not m:continue
  cpu,t=int(m[1]),float(m[2]);first[cpu]=min(first.get(cpu,t),t);last[cpu]=max(last.get(cpu,t),t)
  if m[3]=='adreno_preempt_done':events.append((t,l))
end=max(last.values());start=max(max(first.values()),end-60);hist=collections.Counter()
for t,l in events:
 if t<start:continue
 m=re.search(r'to id=(\d+) from id=(\d+) level=([0-9a-f]+)',l)
 if m:hist[f'{m[2]}->{m[1]} level={m[3]}']+=1
s={'first_by_cpu':first,'last_by_cpu':last,'window_start':start,'window_end':end,'histogram':dict(hist),'boundary':'Counts only retained events in the common-start final window, with same end cutoff for all CPUs; not uniquely per-process.'};(p/'preempt-final-window.json').write_text(json.dumps(s,indent=2));print(json.dumps(s))
