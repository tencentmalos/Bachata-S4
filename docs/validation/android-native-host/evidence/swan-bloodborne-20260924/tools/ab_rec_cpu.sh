#!/usr/bin/env bash
# Recorder A/B by per-thread on-CPU time: bash ab_rec_cpu.sh <outdir> <label> <on|off>
set -u
OUT="$1"; L="$2"; MODE="$3"; S=PB3110PGL6240001G; SVC=com.shadps4.android/.service.FexSessionService
mkdir -p "$OUT"
d() { adb -s $S shell dumpsys activity service $SVC "$@" 2>/dev/null | tr -d '\r' | grep -v "^SERVICE\|^  Client:"; }
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
st() { adb -s $S shell 'p=$(pidof com.shadps4.android); for t in /proc/$p/task/*; do c=$(cat $t/comm); case "$c" in shadPS4:GpuComm*|shadPS4:VkRecor*|Guest-1|shadPS4:Present*) echo "$c $(cut -d" " -f1 $t/schedstat)";; esac; done' | tr -d '\r'; }
d vk_recorder $MODE > /dev/null
adb -s $S shell "read -t 3 </dev/zero" 2>/dev/null
st > "$OUT/$L-st0.txt"; p0=$(present); t0=$(date +%s%N)
adb -s $S shell "read -t 10 </dev/zero" 2>/dev/null
st > "$OUT/$L-st1.txt"; p1=$(present); t1=$(date +%s%N)
python - "$OUT/$L-st0.txt" "$OUT/$L-st1.txt" $((p1-p0)) $t0 $t1 "$L" "$MODE" <<'PY' | tee -a "$OUT/ab-cpu.txt"
import sys
a={l.rsplit(' ',1)[0]:int(l.rsplit(' ',1)[1]) for l in open(sys.argv[1]) if l.strip()}
b={l.rsplit(' ',1)[0]:int(l.rsplit(' ',1)[1]) for l in open(sys.argv[2]) if l.strip()}
n=int(sys.argv[3]); secs=(int(sys.argv[5])-int(sys.argv[4]))/1e9
parts=[f"{k}={(b[k]-a.get(k,0))/1e6/max(n,1):.1f}ms/f" for k in sorted(b)]
print(f"{sys.argv[6]} rec={sys.argv[7]} fps={n/secs:.2f} "+" ".join(parts))
PY
