#!/usr/bin/env bash
# One A/B phase for HLE copy region merging: bash ab_hle.sh <outdir> <label> <on|off>
set -u
OUT="$1"; L="$2"; MODE="$3"; S=PB3110PGL6240001G; SVC=com.shadps4.android/.service.FexSessionService
mkdir -p "$OUT"
d() { adb -s $S shell dumpsys activity service $SVC "$@" 2>/dev/null | tr -d '\r' | grep -v "^SERVICE\|^  Client:"; }
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
d pm4_stats hle_merge $MODE > /dev/null
adb -s $S shell "read -t 3 </dev/zero" 2>/dev/null
a=$(present); t0=$(date +%s%N)
busy=$(adb -s $S shell 'for i in 1 2 3 4 5; do read -t 2 </dev/zero; cat /sys/class/kgsl/kgsl-3d0/gpubusy; done' | tr -d '\r' | awk '{s+=$1/$2} END {printf "%.1f", 100*s/NR}')
b=$(present); t1=$(date +%s%N)
echo "$L hle_merge=$MODE fps=$(python -c "print(round(($b-$a)*1e9/($t1-$t0),3))") gpubusy=${busy}% cpuset=$(adb -s $S shell 'cat /proc/$(pidof com.shadps4.android)/cpuset' | tr -d '\r')" | tee -a "$OUT/ab-fps.txt"
