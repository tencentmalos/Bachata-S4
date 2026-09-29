#!/usr/bin/env bash
# One thread-priority A/B phase on Swan: set debug.shadps4.thread_priority (1 = default on, 0 = off),
# relaunch Bloodborne to the save point, record the DebugBus thread_priority status and the host-log
# priority lines, then sample the spawn view and (after turning the camera) the clinic interior.
# Usage: bash ab_tp.sh <outdir> <label> <0|1>
set -u
OUT="$1"; L="$2"; ON="$3"; S=PB3110PGL6240001G; SVC=com.shadps4.android/.service.FexSessionService
D=$(dirname "$0"); mkdir -p "$OUT"
if [ "$ON" = "0" ]; then adb -s $S shell setprop debug.shadps4.thread_priority 0; else adb -s $S shell setprop debug.shadps4.thread_priority '""'; fi
echo "== $L thread_priority=[$(adb -s $S shell getprop debug.shadps4.thread_priority | tr -d '\r')]" | tee -a "$OUT/phases.txt"
bash "$D/launch_bb_pad.sh" "$OUT/$L-launch" > "$OUT/$L-launch.log" 2>&1
tail -1 "$OUT/$L-launch.log" | tee -a "$OUT/phases.txt"
adb -s $S shell dumpsys activity service $SVC thread_priority status 2>/dev/null | tr -d '\r' > "$OUT/$L-status.txt"
adb -s $S shell 'grep -a "Thread priority:" /data/data/com.shadps4.android/files/host/log/android-host.log' | tr -d '\r' > "$OUT/$L-hostlog.txt"
head -2 "$OUT/$L-status.txt" | tail -1 | tee -a "$OUT/phases.txt"
sleep 15
bash "$D/sample_tp.sh" "$OUT" "$L-spawn" 10 | head -22
# Turn to the clinic interior (right stick +1 for 1.5 s): the heavier, CPU-side fingerprint.
PAD_STATE="$OUT/pad-action-id" bash "$D/pad.sh" none 1500 0 0 1 0
sleep 8
bash "$D/sample_tp.sh" "$OUT" "$L-inner" 10 | head -22
