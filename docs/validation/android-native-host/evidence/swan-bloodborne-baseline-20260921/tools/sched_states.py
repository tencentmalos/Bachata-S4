"""Per-thread on-CPU / runnable / sleep split from a sched_switch+sched_wakeup ftrace text trace.

usage: sched_states.py TRACE.gz TGID [tid,tid,...] [start_ts]
State machine per tid: RUNNING between switch-in and switch-out; after switch-out with prev_state R/R+
the thread is RUNNABLE (preempted) until next switch-in; with S it SLEEPS until sched_waking/wakeup,
then RUNNABLE until switch-in; D is uninterruptible. Also: per-CPU busy share by process, preemptors
of the watched threads, CPU residency, and the whole-process on-CPU total."""
import gzip, re, sys, collections
path, tgid = sys.argv[1], sys.argv[2]
watch = set(int(x) for x in sys.argv[3].split(",")) if len(sys.argv) > 3 else set()
start_ts = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0  # clip to the window every CPU covers (overrun)
hdr = re.compile(r'^\s*(.+?)-(\d+)\s+\(\s*([\d-]+)\)\s+\[(\d+)\]\s+\S+\s+(\d+\.\d+): (\w+): (.*)$')
swp = re.compile(r'prev_comm=(.*?) prev_pid=(\d+) prev_prio=(\d+) prev_state=(\S+) ==> next_comm=(.*?) next_pid=(\d+) next_prio=(\d+)')
wkp = re.compile(r'comm=(.*?) pid=(\d+) prio=(\d+) target_cpu=(\d+)')
tg = {}            # pid -> tgid (from header of lines emitted by the task itself)
names = {}
state = {}         # pid -> (state, since)
acc = collections.defaultdict(lambda: collections.Counter())
cpu_of = collections.defaultdict(collections.Counter)
cpu_busy = collections.defaultdict(collections.Counter)  # cpu -> tgid/comm -> seconds
running_on = {}    # cpu -> (pid, since)
preempted_by = collections.defaultdict(collections.Counter)
waiting_since = {}
t0 = t1 = None
pid_tgid_hint = {}
def close(pid, ts):
    st = state.get(pid)
    if st:
        acc[pid][st[0]] += ts - st[1]
for line in gzip.open(path, "rt", encoding="utf-8", errors="replace"):
    m = hdr.match(line)
    if not m:
        continue
    comm, pid, tgs, cpu, ts, ev, rest = m.groups()
    pid = int(pid); cpu = int(cpu); ts = float(ts)
    if ts < start_ts:
        continue
    if tgs.strip("-"):
        tg[pid] = tgs
    names.setdefault(pid, comm)
    if t0 is None:
        t0 = ts
    t1 = ts
    if ev == "sched_switch":
        s = swp.match(rest)
        if not s:
            continue
        pc, pp, _, pst, nc, npid, _ = s.groups()
        pp = int(pp); npid = int(npid)
        names[pp] = pc; names[npid] = nc
        if cpu in running_on:
            rp, since = running_on[cpu]
            cpu_busy[cpu][rp] += ts - since
        running_on[cpu] = (npid, ts)
        close(pp, ts)
        if pst.startswith("R"):
            state[pp] = ("runnable_preempted", ts)
            if pp in watch:
                preempted_by[pp][f"{nc}/{npid}"] += 1
        elif pst.startswith("D"):
            state[pp] = ("D", ts)
        else:
            state[pp] = ("sleep", ts)
        close(npid, ts)
        state[npid] = ("running", ts)
        cpu_of[npid][cpu] += 1
    elif ev in ("sched_waking", "sched_wakeup"):
        w = wkp.match(rest)
        if not w:
            continue
        wp = int(w.group(2))
        st = state.get(wp)
        if st and st[0] in ("sleep", "D"):
            close(wp, ts)
            state[wp] = ("runnable_woken", ts)
        elif st is None:
            state[wp] = ("runnable_woken", ts)
for pid in list(state):
    close(pid, t1)
for cpu, (rp, since) in running_on.items():
    cpu_busy[cpu][rp] += t1 - since
win = t1 - t0
print(f"window {win:.3f} s  ({t0:.3f} .. {t1:.3f})")
proc = [p for p in names if tg.get(p) == tgid]
tot = collections.Counter()
for p in proc:
    tot.update(acc[p])
print(f"process {tgid}: on-CPU {tot['running']:.2f} s = {tot['running']/win:.2f} cores; runnable {tot['runnable_preempted']+tot['runnable_woken']:.2f} s")
print("\n## top threads of the process by on-CPU")
print(f"{'tid':>7} {'name':16} {'run%':>6} {'rnbl-pre%':>9} {'rnbl-wake%':>10} {'sleep%':>7} {'D%':>5}  cpus")
for p in sorted(proc, key=lambda p: -acc[p]['running'])[:24]:
    a = acc[p]
    cs = " ".join(f"c{c}:{n}" for c, n in sorted(cpu_of[p].items()))
    print(f"{p:>7} {names[p][:16]:16} {100*a['running']/win:6.1f} {100*a['runnable_preempted']/win:9.1f} {100*a['runnable_woken']/win:10.1f} {100*a['sleep']/win:7.1f} {100*a['D']/win:5.1f}  {cs}")
for p in watch:
    print(f"\n## preempted {names.get(p)}/{p} by (switch-outs in state R):")
    for k, n in preempted_by[p].most_common(12):
        print(f"  {n:6d}  {k}")
print("\n## per-CPU busy share (top occupants)")
for cpu in sorted(cpu_busy):
    busy = cpu_busy[cpu]
    idle = busy.get(0, 0.0)
    top = [(p, s) for p, s in busy.most_common(8) if p != 0][:5]
    desc = ", ".join(f"{names.get(p,'?')[:15]}/{p}{'*' if tg.get(p)==tgid else ''} {100*s/win:.0f}%" for p, s in top)
    print(f"cpu{cpu}: idle {100*idle/win:4.0f}%  | {desc}")
others = collections.Counter()
for p, a in acc.items():
    if tg.get(p) != tgid and p != 0:
        others[tg.get(p, '?') + ':' + names.get(p, '?')] += a['running']
print("\n## other threads by on-CPU (top 15)")
for k, s in others.most_common(15):
    print(f"  {s/win:5.2f} cores  {k}")
