#!/usr/bin/env bash
# Repeat Bloodborne launch -> continue -> 150 s observation; record crashes and pm4_validate hits.
# Usage: bash repeat.sh <outdir> <runs> <gnm_fastpath 0|1>
set -u
OUT="$1"; N="$2"; FP="$3"; S=PB3110PGL6240001G; D=$(dirname "$0"); mkdir -p "$OUT"
adb -s $S shell setprop debug.shadps4.gnm_fastpath "$FP"
for r in $(seq 1 "$N"); do
  adb -s $S logcat -b crash -c
  bash "$D/launch_bb_pad.sh" "$OUT/r$r" > "$OUT/r$r.log" 2>&1
  pid=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r'); res=alive
  for i in $(seq 1 15); do sleep 10; p=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r'); [ -z "$p" ] || [ "$p" != "$pid" ] && { res="died_t$((i*10))"; break; }; done
  grep -q "stage=Running" "$OUT/r$r.log" || res="died_during_load"
  adb -s $S shell 'grep -a -E "pm4_validate|Bad PM4|Assertion|Device lost|Gnm fast path" /data/data/com.shadps4.android/files/host/log/android-host.log' | tr -d '\r' > "$OUT/r$r-host.txt"
  adb -s $S logcat -d -b crash | grep -E "Fatal|Abort|#0[4-9]" | head -8 > "$OUT/r$r-crash.txt"
  adb -s $S shell 'dmesg | grep -E "kgsl.*Fault" | tail -2' | tr -d '\r' >> "$OUT/r$r-crash.txt"
  echo "run $r fastpath=$FP: $res $(grep -o 'fps~[0-9]*' "$OUT/r$r.log" | tr '\n' ' ') validate=$(grep -c pm4_validate "$OUT/r$r-host.txt")" | tee -a "$OUT/summary.txt"
done
