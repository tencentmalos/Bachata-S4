#!/usr/bin/env bash
# One A/B phase for pass hoisting (recorder on): bash ab_hoist.sh <outdir> <label> <on|off>
set -u
OUT="$1"; L="$2"; MODE="$3"; S=PB3110PGL6240001G; SVC=com.shadps4.android/.service.FexSessionService
mkdir -p "$OUT"
d() { adb -s $S shell dumpsys activity service $SVC "$@" 2>/dev/null | tr -d '\r' | grep -v "^SERVICE\|^  Client:"; }
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
snap() { d gpu_memory request >/dev/null; adb -s $S shell "read -t 2 </dev/zero" 2>/dev/null; d gpu_memory status | grep -E "tile_traffic|pass_hoist|attachment_draws=" > "$1"; }
d vk_recorder hoist $MODE > /dev/null
adb -s $S shell "read -t 3 </dev/zero" 2>/dev/null
snap "$OUT/$L-s0.txt"; p0=$(present)
a=$(present); t0=$(date +%s%N)
busy=$(adb -s $S shell 'for i in 1 2 3 4 5; do read -t 2 </dev/zero; cat /sys/class/kgsl/kgsl-3d0/gpubusy; done' | tr -d '\r' | awk '{s+=$1/$2} END {printf "%.1f", 100*s/NR}')
b=$(present); t1=$(date +%s%N)
snap "$OUT/$L-s1.txt"; p1=$(present)
python - "$OUT/$L-s0.txt" "$OUT/$L-s1.txt" $((p1-p0)) "$L" "$MODE" "$(python -c "print(round(($b-$a)*1e9/($t1-$t0),2))")" "$busy" "$(adb -s $S shell 'cat /proc/$(pidof com.shadps4.android)/cpuset' | tr -d '\r')" <<'PY' | tee -a "$OUT/ab.txt"
import re,sys
def kv(p): return {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',open(p).read())}
a,b=kv(sys.argv[1]),kv(sys.argv[2]); n=max(int(sys.argv[3]),1)
f=lambda k:(b[k]-a[k])/n
print(f"{sys.argv[4]} hoist={sys.argv[5]} fps={sys.argv[6]} gpubusy={sys.argv[7]}% passes={f('passes'):.0f} loads={f('loads'):.0f} loadMPix={f('load_pixels')/1e6:.1f} hoisted={f('hoisted'):.1f} conflicts={f('conflicts'):.1f} unavail={f('unavailable'):.1f} interrupted={f('interrupted'):.2f} cpuset={sys.argv[8]}")
PY
