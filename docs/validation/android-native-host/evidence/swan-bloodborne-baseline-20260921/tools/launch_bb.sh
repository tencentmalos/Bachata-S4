#!/usr/bin/env bash
# Launch Bloodborne on Swan from the Library (card -> Launch), drive the intro with the
# confirm button (Circle in this build) until the present rate drops into the loading band,
# then wait for the clinic to settle. Usage: bash launch_bb.sh <outdir>
OUT="$1"; S=PB3110PGL6240001G; SVC="com.shadps4.android/.service.FexSessionService"; export MSYS_NO_PATHCONV=1
mkdir -p "$OUT"
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
stage() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/stage:/{print $2}' | tr -d '\r'; }
wait_s() { adb -s $S shell "read -t $1 </dev/zero" 2>/dev/null; }
rate() { local a b; a=$(present); wait_s 5; b=$(present); echo $(( (${b:-0}-${a:-0})/5 )); }
tap() { adb -s $S shell input swipe $1 $2 $1 $2 250; }
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
wait_s 25
for i in $(seq 1 12); do tap 1820 690; wait_s 12; f=$(rate); echo "press $i -> fps~$f"; [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break; done
for i in $(seq 1 40); do f=$(rate); echo "load-wait t=$((i*5)) fps~$f"; [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break; done
echo "settling 45 s"; wait_s 45
echo "arrived: fps~$(rate) stage=$(stage) pid=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r')"
