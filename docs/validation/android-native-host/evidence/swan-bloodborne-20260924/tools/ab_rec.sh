#!/usr/bin/env bash
# One A/B phase for the Vulkan recording thread: bash ab_rec.sh <outdir> <label> <on|off>
set -u
OUT="$1"; L="$2"; MODE="$3"; S=PB3110PGL6240001G; SVC=com.shadps4.android/.service.FexSessionService
mkdir -p "$OUT"
d() { adb -s $S shell dumpsys activity service $SVC "$@" 2>/dev/null | tr -d '\r' | grep -v "^SERVICE\|^  Client:"; }
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
snap() { d gpu_memory request >/dev/null; adb -s $S shell "read -t 2 </dev/zero" 2>/dev/null; d gpu_memory status > "$1"; }
num() { grep -o " $2=[0-9]*" "$1" | head -1 | cut -d= -f2; }
d vk_recorder $MODE > "$OUT/$L-mode.txt"
adb -s $S shell "read -t 3 </dev/zero" 2>/dev/null
d vk_recorder status > "$OUT/$L-rec0.txt"; snap "$OUT/$L-snap0.txt"; p0=$(present)
a=$(present); t0=$(date +%s%N); adb -s $S shell "read -t 10 </dev/zero" 2>/dev/null; b=$(present); t1=$(date +%s%N)
snap "$OUT/$L-snap1.txt"; p1=$(present); d vk_recorder status > "$OUT/$L-rec1.txt"
n=$((p1-p0)); [ $n -lt 1 ] && n=1
per() { python -c "print(round(($(num "$OUT/$L-snap1.txt" $1)-$(num "$OUT/$L-snap0.txt" $1))/$n,1))"; }
rper() { python -c "print(round(($(grep -oE "(^| )$1=[0-9]+" "$OUT/$L-rec1.txt" | tr -d " " | cut -d= -f2)-$(grep -oE "(^| )$1=[0-9]+" "$OUT/$L-rec0.txt" | tr -d " " | cut -d= -f2))/$n,1))"; }
echo "$L rec=$MODE fps=$(python -c "print(round(($b-$a)*1e9/($t1-$t0),3))") draws/frame=$(per attachment_draws) chunks=$(rper chunks) cmds=$(rper commands) syncs=$(rper syncs) waits=$(rper sync_waits) raw=$(rper raw_syncs) cpuset=$(adb -s $S shell 'cat /proc/$(pidof com.shadps4.android)/cpuset' | tr -d '\r')" | tee -a "$OUT/ab-fps.txt"
