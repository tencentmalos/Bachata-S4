#!/usr/bin/env bash
# One multiblock A/B phase on AYN Thor: set the property, (re)launch Bloodborne to the save point,
# record whether FEX logged MULTIBLOCK, then sample FPS/GPU and per-thread on-CPU/runnable/sleep.
# Usage: bash ab_multiblock.sh <outdir> <label> <0|1>
set -u
OUT="$1"; L="$2"; MB="$3"; S=9c2841a4; D=$(dirname "$0"); mkdir -p "$OUT"
adb -s $S shell setprop debug.shadps4.fex_multiblock "$MB"
echo "== $L multiblock=$(adb -s $S shell getprop debug.shadps4.fex_multiblock | tr -d '\r')" | tee -a "$OUT/phases.txt"
adb -s $S logcat -c
T0=$(date +%s)
bash "$D/launch_bb.sh" "$OUT/$L-launch" 2>&1 | tee "$OUT/$L-launch.log" | tail -3
echo "launch took $(( $(date +%s) - T0 ))s" | tee -a "$OUT/phases.txt"
adb -s $S logcat -d -s FexCore 2>/dev/null | tr -d '\r' | grep -i multiblock | tail -2 | tee -a "$OUT/phases.txt"
bash "$D/sample.sh" "$OUT" "$L" 10 | head -12
bash "$D/sample2.sh" "$OUT" "$L-sched" 10 | head -8
