#!/usr/bin/env bash
# Launch Bloodborne from the Library and drive notice/title/offline/continue with the DebugBus pad.
# Usage: bash launch_bb_pad.sh <outdir>
OUT="$1"; S=PB3110PGL6240001G; SVC="com.shadps4.android/.service.FexSessionService"; export MSYS_NO_PATHCONV=1
D=$(dirname "$0"); mkdir -p "$OUT"
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
stage() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/stage:/{print $2}' | tr -d '\r'; }
wait_s() { adb -s $S shell "read -t $1 </dev/zero" 2>/dev/null; }
rate() { local a b; a=$(present); wait_s 5; b=$(present); echo $(( (${b:-0}-${a:-0})/5 )); }
if [ "$(stage)" = "Running" ]; then
  adb -s $S shell am startservice -n $SVC -a com.shadps4.android.action.STOP_EMULATION >/dev/null 2>&1
  for i in $(seq 1 20); do wait_s 2; [ "$(stage)" = "Stopped" ] && break; done; echo "stopped: $(stage)"
  adb -s $S shell am force-stop com.shadps4.android; wait_s 3
fi
adb -s $S shell am start -W -n com.shadps4.android/.MainActivity -f 0x24000000 | grep -E 'Status'; wait_s 6
n=0; until [ "$(stage)" = "Running" ] || [ $n -ge 3 ]; do
  n=$((n+1)); adb -s $S shell input tap 727 553; wait_s 3; adb -s $S shell input tap 1271 618
  for i in $(seq 1 15); do wait_s 2; [ "$(stage)" = "Running" ] && break; done
done
echo "session: pid=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r') stage=$(stage)" | tee "$OUT/session-start.txt"
adb -s $S shell dumpsys activity service $SVC | tr -d '\r' >> "$OUT/session-start.txt"
wait_s 45
for step in notice title offline; do echo "$step: $(bash $D/pad.sh circle 200)"; wait_s 10; done
echo "continue: $(bash $D/pad.sh circle 200)"
for i in $(seq 1 24); do f=$(rate); echo "load-wait t=$((i*5)) fps~$f"; [ "$i" -gt 6 ] && [ "$f" -gt 8 ] && break; done
echo "settling 20 s"; wait_s 20
echo "arrived: fps~$(rate) stage=$(stage) pid=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r') cpuset=$(adb -s $S shell 'cat /proc/$(pidof com.shadps4.android)/cpuset' | tr -d '\r')"
