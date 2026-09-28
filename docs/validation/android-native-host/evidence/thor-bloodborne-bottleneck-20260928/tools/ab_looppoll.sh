#!/usr/bin/env bash
# One session on AYN Thor: set multiblock / loop-poll properties, stop the running session (timed),
# relaunch Bloodborne to the save point, record FexCore's multiblock line, then sample twice in the
# same session: upload_diag stream_barriers off (new default) and on (old behaviour).
# Usage: bash ab_looppoll.sh <outdir> <label> <multiblock 0|1> <loop_poll empty|0>
set -u
OUT="$1"; L="$2"; MB="$3"; LP="${4:-}"; S=9c2841a4; D=$(dirname "$0"); mkdir -p "$OUT"
SVC=com.shadps4.android/.service.FexSessionService
adb -s $S shell "setprop debug.shadps4.fex_multiblock '$MB'; setprop debug.shadps4.fex_loop_poll '$LP'"
echo "== $L multiblock=$(adb -s $S shell getprop debug.shadps4.fex_multiblock | tr -d '\r') loop_poll=$(adb -s $S shell getprop debug.shadps4.fex_loop_poll | tr -d '\r')" | tee -a "$OUT/phases.txt"
adb -s $S logcat -c
T0=$(date +%s)
bash "$D/launch_bb.sh" "$OUT/$L-launch" > "$OUT/$L-launch.log" 2>&1
grep -E "stopped:|arrived" "$OUT/$L-launch.log" | tee -a "$OUT/phases.txt"
echo "launch took $(( $(date +%s) - T0 ))s" | tee -a "$OUT/phases.txt"
adb -s $S logcat -d -s FexCore 2>/dev/null | tr -d '\r' | grep -i multiblock | tail -1 | tee -a "$OUT/phases.txt"
i=0
for sb in off on off; do
  i=$((i+1))
  adb -s $S shell dumpsys activity service $SVC upload_diag stream_barriers $sb 2>/dev/null | tr -d '\r' | tail -1 | tee -a "$OUT/phases.txt"
  adb -s $S shell "read -t 5 </dev/zero" 2>/dev/null
  bash "$D/sample.sh" "$OUT" "$L-$i-sb$sb" 15 | head -4
done
