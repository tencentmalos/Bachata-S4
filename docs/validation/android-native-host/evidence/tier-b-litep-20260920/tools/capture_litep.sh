#!/usr/bin/env bash
# Combined Litep evidence capture in the clinic: profile_sync=1 (condition
# observer), Litep tools/capture_kgsl.py armed window, PROF file capture inside it.
# Usage: bash capture_litep.sh <label> <fastpath 0|1> <outdir> [prof_seconds=20]
set -u
LABEL="$1"; FP="$2"; OUT="$3"; SECS="${4:-20}"
S=9c2841a4
SVC="com.shadps4.android/.service.FexSessionService"
LITEP=/c/workspace/spatial_mcp_publish/dev_tools/mcp/litep
export MSYS_NO_PATHCONV=1 ANDROID_SDK_ROOT=/c/Users/Admin/AppData/Local/Android/Sdk TMPDIR="C:/Users/Admin/AppData/Local/Temp"
ROOTCTL=/c/workspace/devices/tools/android-root/rootctl
mkdir -p "$OUT"
if [ "$FP" = "0" ]; then adb -s $S shell setprop debug.shadps4.sync_fastpath 0; else adb -s $S shell setprop debug.shadps4.sync_fastpath '""'; fi
adb -s $S shell setprop debug.shadps4.profile_sync 1
echo "sync_fastpath=$(adb -s $S shell getprop debug.shadps4.sync_fastpath | tr -d '\r') profile_sync=$(adb -s $S shell getprop debug.shadps4.profile_sync | tr -d '\r')" | tee "$OUT/property.txt"
adb -s $S shell am start -W -n com.shadps4.android/.MainActivity -f 0x24000000 --ez open_last_game true 2>&1 | grep -E "Status" || true
present() { adb -s $S shell dumpsys activity service $SVC | awk '/host_present/{gsub(/[()]/,"",$2);print $2}'; }
rate() { local a b; a=$(present); sleep 5; b=$(present); echo $(( (${b:-0}-${a:-0})/5 )); }
press() { (cd /c/workspace/devices && bash $ROOTCTL --serial $S exec -- "sendevent /dev/input/event9 1 304 1; sendevent /dev/input/event9 0 0 0; sleep 0.15; sendevent /dev/input/event9 1 304 0; sendevent /dev/input/event9 0 0 0" >/dev/null 2>&1); }
for i in $(seq 1 40); do f=$(rate); echo "menu-wait t=$((i*5)) fps~$f"; [ "$f" -ge 25 ] && [ $i -ge 6 ] && break; done
# Adaptive: Cross every 20 s (dialog confirm / offline / continue) until the
# rate drops into the loading/clinic band; a fresh process reaches the dialog later.
for i in $(seq 1 10); do press; sleep 20; f=$(rate); echo "press $i -> fps~$f"; [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break; done
for i in $(seq 1 40); do f=$(rate); echo "load-wait t=$((i*5)) fps~$f"; [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break; done
sleep 60
p0=$(present); until p1=$(present) && [ $((p1-p0)) -ge 250 ]; do sleep 5; done
echo "warmup presents=$((p1-p0))" | tee "$OUT/warmup.txt"
adb -s $S shell dumpsys activity service $SVC | tr -d '\r' > "$OUT/session-before.txt"
adb -s $S shell dumpsys activity service $SVC hle_sync status | tr -d '\r' | grep -o '"fast_path":"[a-z_]*"' | tee "$OUT/fast_path.txt"
adb -s $S exec-out screencap -p > "$OUT/scene-before.png"
# KGSL first (armed window = PROF seconds + 6), PROF inside it.
PYTHONIOENCODING=utf-8 python "C:/workspace/spatial_mcp_publish/dev_tools/mcp/litep/tools/capture_kgsl.py" --serial $S --package com.shadps4.android \
  --rootctl "C:/PROGRA~1/Git/bin/bash.exe C:/workspace/devices/tools/android-root/rootctl-win" \
  --clock-helper "C:/workspace/emulations/shadps4/build/validation/litep-kgsl-clock" \
  --seconds $((SECS+6)) --total-mib 384 --output "$(cygpath -w "$OUT")\kgsl-capture" > "$OUT/kgsl-capture.log" 2>&1 &
KG=$!
sleep 4
adb -s $S shell dumpsys activity service $SVC profiler_capture file 512 $SECS | tr -d '\r' > "$OUT/capture-start.txt"
p2=$(present); sleep $SECS; p3=$(present)
echo "capture_window presents=$((p3-p2)) seconds=$SECS" | tee "$OUT/capture-window.txt"
for i in $(seq 1 30); do st=$(adb -s $S shell dumpsys activity service $SVC profiler_capture status | tr -d '\r'); echo "$st" | grep -q "capture_active=0" && break; sleep 2; done
echo "$st" > "$OUT/capture-final.txt"
wait $KG; echo "kgsl exit=$?" | tee -a "$OUT/capture-window.txt"; tail -2 "$OUT/kgsl-capture.log"
adb -s $S exec-out screencap -p > "$OUT/scene-after.png"
PROF=$(grep -o "capture_path=.*" "$OUT/capture-final.txt" | cut -d= -f2)
(cd /c/workspace/devices && bash $ROOTCTL --serial $S exec -- "cp $PROF /data/local/tmp/sync-review/$LABEL.prof; chmod 644 /data/local/tmp/sync-review/$LABEL.prof; sha256sum /data/local/tmp/sync-review/$LABEL.prof" 2>&1 | tail -1 | tee "$OUT/prof-device.sha256")
adb -s $S pull "/data/local/tmp/sync-review/$LABEL.prof" "$(cygpath -w "$OUT/capture.prof")" | tail -1
adb -s $S shell dumpsys activity service $SVC | tr -d '\r' > "$OUT/session-after.txt"
adb -s $S shell dumpsys activity service $SVC hle_sync status | tr -d '\r' > "$OUT/hle-sync-after.json"
adb -s $S shell setprop debug.shadps4.profile_sync '""'
(cd /c/workspace/devices && bash $ROOTCTL --serial $S exec -- "am startservice -n $SVC -a com.shadps4.android.action.STOP_EMULATION >/dev/null; sleep 6; ls /sys/kernel/tracing/instances/" 2>&1 | tail -1)
adb -s $S shell dumpsys activity service $SVC 2>&1 | tr -d '\r' | grep -E "stage|session:|No services" | tee "$OUT/stop.txt"
adb -s $S shell "cat /proc/\$(pidof com.shadps4.android)/status 2>/dev/null | grep TracerPid" || true
echo "done $LABEL"
