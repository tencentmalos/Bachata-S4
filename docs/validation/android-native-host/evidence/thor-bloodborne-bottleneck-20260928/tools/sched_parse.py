"""Parse a tracefs text capture (gzip) for one process: per-thread CPU placement by cluster,
frequency-weighted run time, runnable (wake->run) latency and wakers.
Usage: python sched_parse.py <kgsl_trace.txt.gz> <pid> [tid ...]
Thor clusters: cpu0-2 little (A510), cpu3-6 mid (A715/A710), cpu7 prime (X3).
"""
import gzip, re, sys, collections

path, pid = sys.argv[1], sys.argv[2]
focus = set(sys.argv[3:])
CLUSTER = {0: 'little', 1: 'little', 2: 'little', 3: 'mid', 4: 'mid', 5: 'mid', 6: 'mid', 7: 'prime'}
line_re = re.compile(r'^\s*(.+?)-(\d+)\s+(?:\(\s*[\d-]+\)\s+)?\[(\d+)\]\s+\S+\s+([\d.]+):\s+(\w+):\s*(.*)$')
kv_re = re.compile(r'(\w+)=(\S+)')

names = {}          # tid -> comm (from events)
running = {}        # cpu -> (tid, since_ts)
wake_ts = {}        # tid -> (ts, waker_tid, waker_comm)
freq = collections.defaultdict(lambda: 0)   # cpu -> kHz
on_cpu = collections.defaultdict(lambda: collections.Counter())      # tid -> cpu -> seconds
cycles = collections.Counter()   # tid -> sum(seconds * GHz)
known = collections.Counter()    # tid -> seconds with known CPU frequency
runnable = collections.Counter() # tid -> seconds waiting after wakeup
wakers = collections.defaultdict(collections.Counter)                # tid -> waker comm -> count
wake_lat = collections.defaultdict(list)
switch_out_state = collections.defaultdict(collections.Counter)      # tid -> prev_state -> count
first_ts = last_ts = None

def account(cpu, tid, t0, t1):
    dt = t1 - t0
    if dt <= 0: return
    on_cpu[tid][cpu] += dt
    if freq[cpu] > 0:
        cycles[tid] += dt * freq[cpu] / 1e6   # seconds * GHz
        known[tid] += dt

with gzip.open(path, 'rt', errors='replace') as fh:
    for line in fh:
        m = line_re.match(line)
        if not m: continue
        comm, cur_tid, cpu, ts, ev, rest = m.group(1), m.group(2), int(m.group(3)), float(m.group(4)), m.group(5), m.group(6)
        if first_ts is None: first_ts = ts
        last_ts = ts
        if ev == 'cpu_frequency':
            kv = dict(kv_re.findall(rest)); freq[int(kv['cpu_id'])] = int(kv['state'])
        elif ev == 'sched_waking':
            kv = dict(kv_re.findall(rest)); t = kv.get('pid')
            if t: wake_ts[t] = (ts, cur_tid, comm); names.setdefault(t, kv.get('comm', '?'))
        elif ev == 'sched_switch':
            # prev_comm=X prev_pid=N prev_prio=P prev_state=S ==> next_comm=Y next_pid=M next_prio=Q
            left, _, right = rest.partition('==>')
            pk = dict(kv_re.findall(left)); nk = dict(kv_re.findall(right))
            prev, nxt = pk.get('prev_pid'), nk.get('next_pid')
            names.setdefault(prev, pk.get('prev_comm', '?')); names.setdefault(nxt, nk.get('next_comm', '?'))
            if cpu in running:
                rt, since = running[cpu]
                if rt == prev: account(cpu, prev, since, ts)
            switch_out_state[prev][pk.get('prev_state', '?')] += 1
            running[cpu] = (nxt, ts)
            if nxt in wake_ts:
                wts, wtid, wcomm = wake_ts.pop(nxt)
                lat = ts - wts
                if 0 <= lat < 1.0:
                    runnable[nxt] += lat; wake_lat[nxt].append(lat)
                    wakers[nxt][f'{wcomm}({wtid})'] += 1

span = last_ts - first_ts
print(f'trace span {span:.2f}s')
tids = [t for t in on_cpu if (not focus or t in focus)]
rows = sorted(tids, key=lambda t: -sum(on_cpu[t].values()))
print(f'{"thread":<22}{"tid":>7} {"on%":>6} {"little%":>8} {"mid%":>6} {"prime%":>7} {"avgGHz":>7} {"runq%":>6} {"wakes/s":>8} {"p50wake_us":>10}')
for t in rows[:25]:
    tot = sum(on_cpu[t].values())
    if tot <= 0: continue
    cl = collections.Counter()
    for c, v in on_cpu[t].items(): cl[CLUSTER.get(c, '?')] += v
    lat = sorted(wake_lat[t]); p50 = lat[len(lat)//2] * 1e6 if lat else 0
    print(f'{names.get(t, "?")[:21]:<22}{t:>7} {100*tot/span:6.1f} {100*cl["little"]/tot:8.1f} {100*cl["mid"]/tot:6.1f} {100*cl["prime"]/tot:7.1f} {(cycles[t]/known[t] if known[t] else 0):7.2f} {100*runnable[t]/span:6.1f} {len(lat)/span:8.1f} {p50:10.0f}')
for t in (focus or []):
    print(f'\n-- {names.get(t, "?")}({t}) top wakers:', wakers[t].most_common(8))
    print(f'   switch-out states:', dict(switch_out_state[t].most_common(6)))
    print(f'   per-cpu on seconds:', {c: round(v, 2) for c, v in sorted(on_cpu[t].items())})
