#!/usr/bin/env bash
# One A/B phase for upload_diag: bash ab_phase.sh <outdir> <label> <on|off>
set -u
OUT="$1"; L="$2"; IGN="$3"; S=PB3110PGL6240001G; SVC=com.shadps4.android/.service.FexSessionService
mkdir -p "$OUT"
d() { adb -s $S shell dumpsys activity service $SVC "$@" 2>/dev/null | tr -d '\r' | grep -v "^SERVICE\|^  Client:"; }
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
d upload_diag ignore_storage_dirty $IGN
adb -s $S shell "read -t 3 </dev/zero" 2>/dev/null
d gpu_memory request >/dev/null; adb -s $S shell "read -t 2 </dev/zero" 2>/dev/null; d gpu_memory status > "$OUT/$L-snap0.txt"
d upload_diag start 200 >/dev/null
a=$(present); t0=$(date +%s%N); adb -s $S shell "read -t 10 </dev/zero" 2>/dev/null; b=$(present); t1=$(date +%s%N)
d upload_diag status > "$OUT/$L-upload-diag.txt"
d gpu_memory request >/dev/null; adb -s $S shell "read -t 2 </dev/zero" 2>/dev/null; d gpu_memory status > "$OUT/$L-snap1.txt"; c=$(present)
echo "$L ignore=$IGN fps=$(python -c "print(round(($b-$a)*1e9/($t1-$t0),3))") presents=$((b-a)) snap_presents=$((c-a)) cpuset=$(adb -s $S shell 'cat /proc/$(pidof com.shadps4.android)/cpuset' | tr -d '\r')" | tee -a "$OUT/ab-fps.txt"
head -3 "$OUT/$L-upload-diag.txt"
