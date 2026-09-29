#!/usr/bin/env bash
# One measurement window on Swan for the thread-priority A/B: FPS (host_present, also per second),
# GPU busy/clock, CPU cluster frequencies, and for every app thread that used >=2% CPU its on-CPU
# and runqueue-wait ms per presented frame (/proc/tid/schedstat fields 1 and 2) plus its nice value.
# Usage: bash sample_tp.sh <outdir> <label> [seconds=10]
set -u
OUT="$1"; L="$2"; SEC="${3:-10}"; S=PB3110PGL6240001G; SVC=com.shadps4.android/.service.FexSessionService
mkdir -p "$OUT"
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
# tid|comm|on-cpu ns|runqueue ns|nice (stat field 19, counted after the closing paren of comm)
st() { adb -s $S shell 'p=$(pidof com.shadps4.android); for t in /proc/$p/task/*; do s=$(cat $t/stat); r=${s##*) }; set -- $r; echo "$(basename $t)|$(cat $t/comm)|$(cut -d" " -f1,2 $t/schedstat | tr " " "|")|${17}"; done' | tr -d '\r'; }
gpu() { adb -s $S shell 'cat /sys/class/kgsl/kgsl-3d0/gpubusy /sys/class/kgsl/kgsl-3d0/gpuclk; for p in 0 6; do cat /sys/devices/system/cpu/cpufreq/policy$p/scaling_cur_freq; done' | tr -d '\r' | tr '\n' ' '; }
ctx() { adb -s $S shell 'p=$(pidof com.shadps4.android); echo "pid=$p cpuset=$(cat /proc/$p/cpuset) prop=[$(getprop debug.shadps4.thread_priority)] focus=$(dumpsys window | grep -m1 mCurrentFocus)"; cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | sort -n | tail -1' | tr -d '\r' | tr '\n' ' '; }
echo "$(ctx)" > "$OUT/$L-ctx.txt"
st > "$OUT/$L-st0.txt"; p0=$(present); t0=$(date +%s%N)
: > "$OUT/$L-gpu.txt"; : > "$OUT/$L-present.txt"
for i in $(seq 1 "$SEC"); do
  echo "$(date +%s%N) $(present)" >> "$OUT/$L-present.txt"
  gpu >> "$OUT/$L-gpu.txt"; echo >> "$OUT/$L-gpu.txt"
  adb -s $S shell "read -t 1 </dev/zero" 2>/dev/null
done
st > "$OUT/$L-st1.txt"; p1=$(present); t1=$(date +%s%N)
python - "$OUT/$L-st0.txt" "$OUT/$L-st1.txt" $((p1-p0)) $t0 $t1 "$L" "$OUT/$L-gpu.txt" "$OUT/$L-ctx.txt" "$OUT/$L-present.txt" <<'PY' | tee -a "$OUT/summary.txt"
import sys
def load(p):
    d = {}
    for l in open(p, encoding='utf-8', errors='replace'):
        f = l.strip().split('|')
        if len(f) == 5 and f[2].isdigit() and f[3].isdigit():
            d[f[0]] = (f[1], int(f[2]), int(f[3]), f[4])
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
n = int(sys.argv[3]); secs = (int(sys.argv[5]) - int(sys.argv[4])) / 1e9
fps = n / secs if secs > 0 else 0
rows = []
for tid, (comm, run, wait, nice) in b.items():
    _, run0, wait0, _ = a.get(tid, (comm, 0, 0, nice))
    d = run - run0
    if d > 0.02 * secs * 1e9:
        rows.append((d, wait - wait0, comm, tid, nice))
rows.sort(reverse=True)
busy = []; clk = []; c0 = []; c6 = []
for l in open(sys.argv[7]):
    f = l.split()
    if len(f) >= 5 and f[1].isdigit() and int(f[1]) > 0:
        busy.append(100.0 * int(f[0]) / int(f[1])); clk.append(int(f[2]) / 1e6)
        c0.append(int(f[3]) / 1e3); c6.append(int(f[4]) / 1e3)
per_sec = []
pts = [tuple(map(int, l.split())) for l in open(sys.argv[9]) if len(l.split()) == 2]
for (ta, pa), (tb, pb) in zip(pts, pts[1:]):
    if tb > ta: per_sec.append((pb - pa) / ((tb - ta) / 1e9))
avg = lambda v: sum(v) / len(v) if v else float('nan')
print(f"== {sys.argv[6]}: {secs:.1f}s presents={n} fps={fps:.2f} (1s min {min(per_sec, default=0):.1f} max {max(per_sec, default=0):.1f}) "
      f"gpubusy={avg(busy):.1f}% gpuclk={avg(clk):.0f}MHz cpu0-5={avg(c0):.0f} cpu6-7={avg(c6):.0f}MHz")
print("   ctx: " + open(sys.argv[8]).read().strip())
tot_run = sum(r[0] for r in rows); tot_wait = sum(r[1] for r in rows)
print(f"   total (threads >=2%): on-cpu {tot_run/1e6/max(n,1):6.1f} ms/frame  runq {tot_wait/1e6/max(n,1):6.1f} ms/frame")
for d, w, comm, tid, nice in rows[:16]:
    print(f"   {comm:<18} tid={tid:<6} nice={nice:>3} {d/1e6/secs/10:5.1f}% cpu  on-cpu {d/1e6/max(n,1):6.1f}  runq {w/1e6/max(n,1):5.1f} ms/frame")
PY
