import gzip,re,sys,collections
p=sys.argv[1]; names_file=sys.argv[2] if len(sys.argv)>2 else None
threads={}
if names_file:
    lines=open(names_file,encoding="utf-8",errors="replace").read().split("\n")
    for i in range(0,len(lines)-1,2):
        m=re.search(r"task/(\d+)",lines[i]); 
        if m: threads[int(m.group(1))]=lines[i+1].strip()
sw=re.compile(r'\[(\d+)\]\s+\S+\s+(\d+\.\d+): sched_switch: prev_comm=(.*?) prev_pid=(\d+) prev_prio=\d+ prev_state=(\S+) ==> next_comm=(.*?) next_pid=(\d+)')
wk=re.compile(r'(\d+\.\d+): sched_wakeup: comm=(.*?) pid=(\d+) prio=\d+ target_cpu=(\d+)')
running={}   # pid -> (start_ts)
last_wake={} # pid -> ts
stats=collections.defaultdict(lambda: {"wakeups":0,"short_reblock":0,"short_reblock_us":0.0,"run_after_wake":[], "wake_latency":[], "oncpu":0.0,"switches":0})
t0=None;t1=None
for line in gzip.open(p,"rt",encoding="utf-8",errors="replace"):
    m=sw.search(line)
    if m:
        ts=float(m.group(2)); t0=t0 or ts; t1=ts
        prev=int(m.group(4)); state=m.group(5); nxt=int(m.group(7))
        if prev in running:
            st=running.pop(prev); dur=(ts-st)*1e6
            s=stats[prev]; s["oncpu"]+=dur; s["switches"]+=1
            if prev in last_wake and last_wake[prev] is not None:
                s["run_after_wake"].append(dur)
                if dur<20 and state[0] in "SD":
                    s["short_reblock"]+=1; s["short_reblock_us"]+=dur
                last_wake[prev]=None
        running[nxt]=ts
        if nxt in last_wake and last_wake[nxt] is not None and isinstance(last_wake[nxt],float):
            stats[nxt]["wake_latency"].append((ts-last_wake[nxt])*1e6)
            # keep marker that this run follows a wakeup
            last_wake[nxt]=True
        continue
    m=wk.search(line)
    if m:
        ts=float(m.group(1)); pid=int(m.group(3))
        stats[pid]["wakeups"]+=1; last_wake[pid]=ts
window=(t1-t0) if t0 and t1 else 0
print(f"window {window:.3f}s")
rows=[]
for pid,s in stats.items():
    name=threads.get(pid,"?")
    if not (name.startswith("Guest") or "Gpu" in name or "Vk" in name or "shadPS4" in name): continue
    raw=s["run_after_wake"]; raw.sort(); wl=sorted(s["wake_latency"])
    rows.append((name,pid,s["wakeups"],s["short_reblock"],100*s["short_reblock"]/max(1,s["wakeups"]),s["oncpu"]/1000, (wl[len(wl)//2] if wl else 0),(wl[int(len(wl)*.9)] if wl else 0),(raw[len(raw)//2] if raw else 0)))
rows.sort(key=lambda r:-r[2])
print(f"{'thread':<18}{'pid':>6}{'wakeups':>8}{'reblock<20us':>13}{'%':>6}{'oncpu_ms':>10}{'wake->run p50us':>16}{'p90us':>8}{'run p50us':>10}")
tot_w=tot_r=0
for r in rows[:28]:
    print(f"{r[0]:<18}{r[1]:>6}{r[2]:>8}{r[3]:>13}{r[4]:>6.1f}{r[5]:>10.0f}{r[6]:>16.1f}{r[7]:>8.1f}{r[8]:>10.1f}")
    tot_w+=r[2]; tot_r+=r[3]
print(f"guest+gpu threads total wakeups={tot_w} short-reblock={tot_r} ({100*tot_r/max(1,tot_w):.1f}%)")
