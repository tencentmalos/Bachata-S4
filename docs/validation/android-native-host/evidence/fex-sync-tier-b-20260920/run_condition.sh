#!/usr/bin/env bash
# One A/B condition on the AYN Thor: set the fast-path property, launch the
# last game, wait for the title menu (~30 FPS), press Cross once (继续 =
# Continue), wait for the clinic to load, warm up, measure (FPS windows,
# hle_sync summary/detail, root sched capture), screenshot, stop.
# Usage: bash run_condition.sh <label> <fastpath 0|1> <outdir>
set -u
LABEL="$1"; FP="$2"; OUT="$3"
S=9c2841a4
SVC="com.shadps4.android/.service.FexSessionService"
SP="/c/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad"
export MSYS_NO_PATHCONV=1 ANDROID_SDK_ROOT=/c/Users/Admin/AppData/Local/Android/Sdk TMPDIR="C:/Users/Admin/AppData/Local/Temp"
export ROOTCTL=/c/workspace/devices/tools/android-root/rootctl
mkdir -p "$OUT"
if [ "$FP" = "0" ]; then adb -s $S shell setprop debug.shadps4.sync_fastpath 0; else adb -s $S shell setprop debug.shadps4.sync_fastpath '""'; fi
echo "property=$(adb -s $S shell getprop debug.shadps4.sync_fastpath | tr -d '\r')" | tee "$OUT/property.txt"
adb -s $S shell am start -W -n com.shadps4.android/.MainActivity -f 0x24000000 --ez open_last_game true 2>&1 | grep -E "Status" || true
present() { adb -s $S shell dumpsys activity service $SVC | awk '/host_present/{gsub(/[()]/,"",$2);print $2}'; }
rate() { local a b; a=$(present); sleep 5; b=$(present); echo $(( (${b:-0}-${a:-0})/5 )); }
press() { (cd /c/workspace/devices && bash tools/android-root/rootctl --serial $S exec -- "sendevent /dev/input/event9 1 304 1; sendevent /dev/input/event9 0 0 0; sleep 0.15; sendevent /dev/input/event9 1 304 0; sendevent /dev/input/event9 0 0 0" >/dev/null 2>&1); }
# Same press cadence as Tier A: confirm "session not exited" dialog, offline
# play, then Continue on the title menu; then wait for the loading screen.
for i in $(seq 1 40); do f=$(rate); echo "menu-wait t=$((i*5)) fps~$f"; [ "$f" -ge 25 ] && [ $i -ge 3 ] && break; done
adb -s $S exec-out screencap -p > "$OUT/scene-menu.png"
press; sleep 30; press; sleep 15
adb -s $S exec-out screencap -p > "$OUT/scene-menu2.png"
press
for i in $(seq 1 40); do f=$(rate); echo "load-wait t=$((i*5)) fps~$f"; [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break; done
sleep 60
adb -s $S exec-out screencap -p > "$OUT/scene-before.png"
p0=$(present); until p1=$(present) && [ $((p1-p0)) -ge 250 ]; do sleep 5; done
echo "warmup presents=$((p1-p0))" | tee "$OUT/warmup.txt"
adb -s $S shell dumpsys activity service $SVC hle_sync status | tr -d '\r' | grep -o '"fast_path":"[a-z_]*"' | tee "$OUT/fast_path.txt"
bash "$SP/device/measure.sh" $S "$LABEL" "$OUT" 3 10 > "$OUT/measure.log" 2>&1 || echo "measure exit=$?"
grep -E "fps_window|summary_window|done" "$OUT/measure.log"
adb -s $S exec-out screencap -p > "$OUT/scene-after.png"
adb -s $S logcat -d 2>/dev/null | grep -E "Guest sync fast path|GuestSyncObjects block" | tail -3 | tee "$OUT/logcat-fastpath.txt" || true
(cd /c/workspace/devices && bash tools/android-root/rootctl --serial $S exec -- "am startservice -n $SVC -a com.shadps4.android.action.STOP_EMULATION >/dev/null; sleep 6" >/dev/null 2>&1) || true
adb -s $S shell dumpsys activity service $SVC 2>&1 | tr -d '\r' | grep -E "stage|session:|No services" | tee "$OUT/stop.txt"
adb -s $S shell "cat /proc/\$(pidof com.shadps4.android)/status 2>/dev/null | grep TracerPid" || true
echo "done $LABEL"
