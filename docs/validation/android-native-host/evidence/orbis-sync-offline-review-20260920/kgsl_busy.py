import gzip,re,json,sys,collections,statistics
root=sys.argv[1]
d=json.load(open(f"samples/{root}/analysis/cpu-gpu.json",encoding="utf-8"))
# PROF window in mono ns (cpu-gpu.json start/end are mono ns after sidecar alignment)
w0,w1=d["start_ns"]/1e9,d["end_ns"]/1e9
p=f"samples/{root}/kgsl/kgsl_trace.txt.gz"
ts_re=re.compile(r'\s(\d+\.\d+):\s+(\S+?):\s+(.*)$')
pw=[];busy_sum=total_sum=0;pw_in=0
retired=[];sub={};inflight=collections.Counter();freq=[];gpubusy=[]
for line in gzip.open(p,"rt",encoding="utf-8",errors="replace"):
    m=ts_re.search(line)
    if not m: continue
    t=float(m.group(1));ev=m.group(2);rest=m.group(3)
    if ev=="kgsl_pwrstats":
        mm=re.search(r'total=(\d+) busy=(\d+)',rest)
        if mm and w0<=t<=w1:
            tot,b=int(mm.group(1)),int(mm.group(2));busy_sum+=b;total_sum+=tot;pw_in+=1
    elif ev=="adreno_cmdbatch_retired":
        mm=re.search(r'ctx=(\d+) .*?ts=(\d+) inflight=(\d+).*?start=(\d+) retire=(\d+)',rest)
        if mm and w0<=t<=w1:
            st,rt=int(mm.group(4)),int(mm.group(5))
            retired.append((t,int(mm.group(1)),int(mm.group(2)),int(mm.group(3)),(rt-st)))
    elif ev=="adreno_cmdbatch_submitted":
        mm=re.search(r'inflight=(\d+)',rest)
        if mm and w0<=t<=w1: inflight[int(mm.group(1))]+=1
    elif ev=="gpu_frequency":
        mm=re.search(r'gpu_freq=(\d+)Khz',rest)
        if mm: freq.append((t,int(mm.group(1))))
    elif ev=="kgsl_gpubusy":
        mm=re.search(r'busy=(\d+) elapsed=(\d+)',rest)
        if mm and w0<=t<=w1: gpubusy.append((int(mm.group(1)),int(mm.group(2))))
win=w1-w0
print(f"##### {root}: PROF window {win:.3f}s")
print(f"kgsl_pwrstats samples in window={pw_in}: busy/total = {busy_sum}/{total_sum} = {100*busy_sum/total_sum if total_sum else float('nan'):.1f}% GPU busy (driver power stats; units are GPU clock/us ticks, ratio only)")
if gpubusy:
    b=sum(x for x,_ in gpubusy);e=sum(y for _,y in gpubusy);print(f"kgsl_gpubusy samples={len(gpubusy)} busy/elapsed={100*b/e:.1f}%")
# per-batch GPU execution from start/retire ticks (19.2MHz assumed -> verify by comparing to pwrstats)
exec_ticks=sum(r[4] for r in retired)
print(f"retired batches in window={len(retired)}; sum(retire-start) ticks={exec_ticks} -> at 19.2MHz = {exec_ticks/19.2e6:.3f}s = {100*exec_ticks/19.2e6/win:.1f}% of window (batch-exec sum, overlapping batches possible)")
durs=sorted(r[4]/19.2e3 for r in retired)  # ms
if durs: print(f"per-batch exec ms: p50={durs[len(durs)//2]:.3f} p90={durs[int(len(durs)*.9)]:.3f} p99={durs[int(len(durs)*.99)]:.3f} max={durs[-1]:.3f}")
byctx=collections.Counter(r[1] for r in retired);print("retired per ctx:",dict(byctx))
print("inflight at submit (depth histogram):",dict(sorted(inflight.items())))
# GPU frequency time-weighted in window
if freq:
    freq.sort();cur=None;acc=collections.Counter();last=w0
    for t,f in freq:
        if t<w0: cur=f;continue
        if t>w1: break
        if cur is not None: acc[cur]+=t-last
        last=t;cur=f
    if cur is not None: acc[cur]+=w1-last
    tw=sum(acc.values());print("GPU freq time share:",{k:f"{100*v/tw:.0f}%" for k,v in sorted(acc.items())})
# longest exec batches
for r in sorted(retired,key=lambda r:-r[4])[:5]: print(f"  long batch ctx={r[1]} ts={r[2]} exec={r[4]/19.2e3:.2f}ms inflight_at_retire={r[3]}")
