#!/usr/bin/env bash
# Per-thread on-CPU / runnable (runqueue wait) / sleep per presented frame on AYN Thor, from
# /proc/<pid>/task/<tid>/schedstat (on_cpu_ns run_delay_ns timeslices) and ctxt switch counts,
# plus the CPU each thread last ran on. No root needed.
# Usage: bash sample2.sh <outdir> <label> [seconds=10]
set -u
OUT="$1"; L="$2"; SEC="${3:-10}"; S=9c2841a4; SVC=com.shadps4.android/.service.FexSessionService
mkdir -p "$OUT"
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
st() { adb -s $S shell 'p=$(pidof com.shadps4.android); for t in /proc/$p/task/*; do v=$(grep -E "^(voluntary|nonvoluntary)_ctxt" $t/status | awk "{print \$2}" | tr "\n" " "); c=$(cut -d" " -f39 $t/stat); echo "$(basename $t)|$(cat $t/comm)|$(cat $t/schedstat)|$v|$c"; done' | tr -d '\r'; }
st > "$OUT/$L-s0.txt"; p0=$(present); t0=$(date +%s%N)
adb -s $S shell "read -t $SEC </dev/zero" 2>/dev/null
st > "$OUT/$L-s1.txt"; p1=$(present); t1=$(date +%s%N)
python - "$OUT/$L-s0.txt" "$OUT/$L-s1.txt" $((p1-p0)) $t0 $t1 "$L" <<'PY' | tee -a "$OUT/summary2.txt"
import sys
def load(p):
    d = {}
    for l in open(p, encoding='utf-8', errors='replace'):
        f = l.strip().split('|')
        if len(f) != 5: continue
        ss = f[2].split(); cs = f[3].split()
        if len(ss) < 3 or len(cs) < 2: continue
        d[f[0]] = (f[1], int(ss[0]), int(ss[1]), int(cs[0]), int(cs[1]), f[4])
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
n = int(sys.argv[3]); secs = (int(sys.argv[5]) - int(sys.argv[4])) / 1e9
rows = []
for tid, (comm, on, rq, vol, inv, cpu) in b.items():
    if tid not in a: continue
    _, on0, rq0, vol0, inv0, _ = a[tid]
    don, drq = on - on0, rq - rq0
    if don > 0.03 * secs * 1e9:
        rows.append((don, comm, tid, drq, vol - vol0, inv - inv0, cpu))
rows.sort(reverse=True)
fr = secs * 1000 / max(n, 1)
print(f"== {sys.argv[6]}: {secs:.1f}s presents={n} fps={n/secs:.2f} frame={fr:.1f}ms  (per frame: on-CPU / runnable / sleep; ctx switches vol/invol per frame; last cpu)")
for don, comm, tid, drq, vol, inv, cpu in rows[:16]:
    on_f = don / 1e6 / max(n, 1); rq_f = drq / 1e6 / max(n, 1); sl_f = fr - on_f - rq_f
    print(f"   {comm:<20} {tid:<6} on {on_f:5.1f}  runnable {rq_f:5.1f}  sleep {sl_f:5.1f} ms   vol {vol/max(n,1):6.1f} invol {inv/max(n,1):5.1f}  cpu{cpu}")
PY
