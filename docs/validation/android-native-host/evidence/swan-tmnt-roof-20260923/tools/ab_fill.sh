#!/usr/bin/env bash
# One A/B phase for the compute-fill image clear: bash ab_fill.sh <outdir> <label> <on|off>
# FPS window runs with upload_diag disarmed (the compute_fill counters are always on);
# a short armed window afterwards counts re-uploads per frame.
set -u
OUT="$1"; L="$2"; MODE="$3"; S=PB3110PGL6240001G; SVC=com.shadps4.android/.service.FexSessionService
mkdir -p "$OUT"
d() { adb -s $S shell dumpsys activity service $SVC "$@" 2>/dev/null | tr -d '\r' | grep -v "^SERVICE\|^  Client:"; }
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
d upload_diag fill_clear $MODE
adb -s $S shell "read -t 3 </dev/zero" 2>/dev/null
d upload_diag status | grep compute_fill > "$OUT/$L-fill-before.txt"
a=$(present); t0=$(date +%s%N); adb -s $S shell "read -t 10 </dev/zero" 2>/dev/null; b=$(present); t1=$(date +%s%N)
d upload_diag status | grep compute_fill > "$OUT/$L-fill-after.txt"
d gpu_memory request >/dev/null; adb -s $S shell "read -t 2 </dev/zero" 2>/dev/null; d gpu_memory status > "$OUT/$L-gpu-memory.txt"
d upload_diag start 0 >/dev/null; adb -s $S shell "read -t 3 </dev/zero" 2>/dev/null; d upload_diag stop > "$OUT/$L-upload-diag.txt"
echo "$L fill_clear=$MODE fps=$(python -c "print(round(($b-$a)*1e9/($t1-$t0),3))") presents=$((b-a)) cpuset=$(adb -s $S shell 'cat /proc/$(pidof com.shadps4.android)/cpuset' | tr -d '\r') focus=$(adb -s $S shell dumpsys window | grep -m1 mFocusedApp | tr -d '\r' | awk '{print $3}')" | tee -a "$OUT/ab-fps.txt"
cat "$OUT/$L-fill-before.txt" "$OUT/$L-fill-after.txt"; grep -E "uploads=" "$OUT/$L-upload-diag.txt"
