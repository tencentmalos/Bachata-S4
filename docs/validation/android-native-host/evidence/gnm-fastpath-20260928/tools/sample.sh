#!/usr/bin/env bash
# One measurement window on AYN Thor: FPS (host_present), GPU busy/clock, CPU cluster
# frequencies, and on-CPU ms per presented frame for every app thread that used >=2% CPU.
# Usage: bash sample.sh <outdir> <label> [seconds=10]
set -u
OUT="$1"; L="$2"; SEC="${3:-10}"; S=PB3110PGL6240001G; SVC=com.shadps4.android/.service.FexSessionService
mkdir -p "$OUT"
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
st() { adb -s $S shell 'p=$(pidof com.shadps4.android); for t in /proc/$p/task/*; do echo "$(basename $t)|$(cat $t/comm)|$(cut -d" " -f1 $t/schedstat)"; done' | tr -d '\r'; }
gpu() { adb -s $S shell 'cat /sys/class/kgsl/kgsl-3d0/gpubusy /sys/class/kgsl/kgsl-3d0/gpuclk; for p in 0 6 6; do cat /sys/devices/system/cpu/cpufreq/policy$p/scaling_cur_freq; done' | tr -d '\r' | tr '\n' ' '; }
ctx() { adb -s $S shell 'p=$(pidof com.shadps4.android); echo "pid=$p cpuset=$(cat /proc/$p/cpuset) focus=$(dumpsys window | grep -m1 mCurrentFocus)"; cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | sort -n | tail -1' | tr -d '\r' | tr '\n' ' '; }
echo "$(ctx)" > "$OUT/$L-ctx.txt"
st > "$OUT/$L-st0.txt"; p0=$(present); t0=$(date +%s%N)
: > "$OUT/$L-gpu.txt"
for i in $(seq 1 "$SEC"); do gpu >> "$OUT/$L-gpu.txt"; echo >> "$OUT/$L-gpu.txt"; adb -s $S shell "read -t 1 </dev/zero" 2>/dev/null; done
st > "$OUT/$L-st1.txt"; p1=$(present); t1=$(date +%s%N)
python - "$OUT/$L-st0.txt" "$OUT/$L-st1.txt" $((p1-p0)) $t0 $t1 "$L" "$OUT/$L-gpu.txt" "$OUT/$L-ctx.txt" <<'PY' | tee -a "$OUT/summary.txt"
import sys, collections
def load(p):
    d = {}
    for l in open(p, encoding='utf-8', errors='replace'):
        parts = l.strip().split('|')
        if len(parts) == 3 and parts[2].isdigit():
            d[parts[0]] = (parts[1], int(parts[2]))
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
n = int(sys.argv[3]); secs = (int(sys.argv[5]) - int(sys.argv[4])) / 1e9
fps = n / secs if secs > 0 else 0
rows = []
for tid, (comm, ns) in b.items():
    d = ns - a.get(tid, (comm, 0))[1]
    if d > 0.02 * secs * 1e9:
        rows.append((d, comm, tid))
rows.sort(reverse=True)
busy = []; clk = []; c0 = []; c3 = []; c7 = []
for l in open(sys.argv[7]):
    f = l.split()
    if len(f) >= 6 and f[1].isdigit() and int(f[1]) > 0:
        busy.append(100.0 * int(f[0]) / int(f[1])); clk.append(int(f[2]) / 1e6)
        c0.append(int(f[3]) / 1e3); c3.append(int(f[4]) / 1e3); c7.append(int(f[5]) / 1e3)
avg = lambda v: sum(v) / len(v) if v else float('nan')
print(f"== {sys.argv[6]}: {secs:.1f}s presents={n} fps={fps:.2f} gpubusy={avg(busy):.1f}% gpuclk={avg(clk):.0f}MHz "
      f"cpu0-5={avg(c0):.0f} cpu6-7={avg(c3):.0f} cpu7={avg(c7):.0f}MHz")
print("   ctx: " + open(sys.argv[8]).read().strip())
for d, comm, tid in rows[:14]:
    print(f"   {comm:<24} tid={tid:<6} {d/1e6/secs/10:5.1f}% cpu  {d/1e6/max(n,1):6.1f} ms/frame")
PY
