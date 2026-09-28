#!/usr/bin/env bash
# One Gnm fast path A/B phase on Swan: set the property, relaunch Bloodborne to the save point,
# settle, then sample FPS and per-thread on-CPU ms per presented frame.
# Usage: bash ab_gnm.sh <outdir> <label> <0|1>
set -u
OUT="$1"; L="$2"; FP="$3"; S=PB3110PGL6240001G; D=$(dirname "$0"); mkdir -p "$OUT"
adb -s $S shell setprop debug.shadps4.gnm_fastpath "$FP"
echo "== $L gnm_fastpath=$(adb -s $S shell getprop debug.shadps4.gnm_fastpath | tr -d '\r')" | tee -a "$OUT/phases.txt"
bash "$D/launch_bb_pad.sh" "$OUT/$L-launch" > "$OUT/$L-launch.log" 2>&1
tail -1 "$OUT/$L-launch.log" | tee -a "$OUT/phases.txt"
adb -s $S shell 'grep -a -E "Gnm fast path" /data/data/com.shadps4.android/files/host/log/android-host.log' | tr -d '\r' | tee -a "$OUT/phases.txt"
sleep 20
bash "$D/sample.sh" "$OUT" "$L" 10 | head -16
