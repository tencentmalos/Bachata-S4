#!/usr/bin/env bash
# Device measurement bundle for one APK/scene: FPS windows (host_present deltas),
# hle_sync summary + detail windows, and a root sched/KGSL ftrace capture.
# Usage: bash measure.sh <serial> <label> <outdir> [fps_windows=3] [window_s=10]
set -euo pipefail
S="$1"; LABEL="$2"; OUT="$3"; NWIN="${4:-3}"; WIN="${5:-10}"
SVC="com.shadps4.android/.service.FexSessionService"
export MSYS_NO_PATHCONV=1
mkdir -p "$OUT"
snap() { adb -s "$S" shell dumpsys activity service "$SVC" | tr -d '\r' | grep -E "snapshot_ns|guest_flip|host_present|pid:|generation|stage"; }
present() { snap | awk '/host_present/{gsub(/[()]/,"",$2); print $2}'; }
ns() { snap | awk '/snapshot_ns/{print $2}'; }
CTX=$(adb -s "$S" shell dumpsys activity service "$SVC" hle_sync status | tr -d '\r' | grep -o '"context":[0-9]*' | cut -d: -f2)
PID=$(adb -s "$S" shell pidof com.shadps4.android | tr -d '\r')
echo "label=$LABEL context=$CTX pid=$PID" | tee "$OUT/identity.txt"
adb -s "$S" shell dumpsys package com.shadps4.android | tr -d '\r' | grep -E "versionName|lastUpdateTime" >> "$OUT/identity.txt"
snap > "$OUT/snap-0.txt"
# 1) FPS windows, metrics OFF
for i in $(seq 1 "$NWIN"); do
  p0=$(present); t0=$(ns); sleep "$WIN"; p1=$(present); t1=$(ns)
  echo "fps_window $i presents=$((p1-p0)) ns=$((t1-t0)) fps=$(python -c "print(round(($p1-$p0)/(($t1-$t0)/1e9),3))")" | tee -a "$OUT/fps.txt"
done
# 2) hle_sync summary window
adb -s "$S" shell dumpsys activity service "$SVC" hle_sync start "$CTX" > "$OUT/sync-start.json"
sleep 3
adb -s "$S" shell dumpsys activity service "$SVC" hle_sync status > "$OUT/sync-A.json"; pA=$(present)
sleep 20
adb -s "$S" shell dumpsys activity service "$SVC" hle_sync status > "$OUT/sync-B.json"; pB=$(present)
echo "summary_window presents=$((pB-pA))" | tee -a "$OUT/fps.txt"
# 3) detail window
adb -s "$S" shell dumpsys activity service "$SVC" hle_sync detail "$CTX" > "$OUT/sync-detail-start.json"
sleep 10
adb -s "$S" shell dumpsys activity service "$SVC" hle_sync status > "$OUT/sync-C.json"
adb -s "$S" shell dumpsys activity service "$SVC" hle_sync stop "$CTX" > "$OUT/sync-stop.json"
# 4) root sched capture (12 s), metrics OFF
if [ -n "${ROOTCTL:-}" ]; then
  bash "$ROOTCTL" --serial "$S" exec -- "sh /data/local/tmp/sync-review/sched-capture.sh $PID 12 /data/local/tmp/sync-review/$LABEL-sched; cat /data/local/tmp/sync-review/$LABEL-sched/status" | tail -1
  adb -s "$S" pull "/data/local/tmp/sync-review/$LABEL-sched" "$(cygpath -w "$OUT")" | tail -1
fi
snap > "$OUT/snap-end.txt"
echo "done $LABEL -> $OUT"
