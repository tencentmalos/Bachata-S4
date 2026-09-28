#!/usr/bin/env bash
# AYN Thor: stop any running session through the app UI (Back -> Stop), launch Bloodborne from
# the Library, then drive notice -> title -> offline -> continue with the DebugBus pad.
# Usage: bash launch_bb.sh <outdir>
set -u
OUT="$1"; S=9c2841a4; SVC=com.shadps4.android/.service.FexSessionService; D=$(dirname "$0"); mkdir -p "$OUT"
w() { adb -s $S shell "read -t $1 </dev/zero" 2>/dev/null; }
stage() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/stage:/{print $2}' | tr -d '\r'; }
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
rate() { local a b; a=$(present); w 5; b=$(present); echo $(( (${b:-0}-${a:-0})/5 )); }
if [ "$(stage)" = "Running" ]; then
  adb -s $S shell input keyevent KEYCODE_BACK; w 2; ts=$(date +%s%N); adb -s $S shell input tap 1088 846
  for i in $(seq 1 40); do [ "$(stage)" != "Running" ] && break; w 1; done
  echo "stopped: $(stage) after $(( ($(date +%s%N) - ts) / 1000000 )) ms (poll granularity ~1 s)"; w 3
fi
# The session screen keeps the last frame after Stop; restart the (idle) app to get the Library.
if [ "$(stage)" != "Running" ]; then adb -s $S shell am force-stop com.shadps4.android; w 2; fi
adb -s $S shell am start -W -n com.shadps4.android/.MainActivity | grep -E 'Status'; w 4
adb -s $S shell input tap 960 470; w 3; adb -s $S shell input tap 1233 571
for i in $(seq 1 20); do w 2; [ "$(stage)" = "Running" ] && break; done
echo "session: stage=$(stage)" | tee "$OUT/session-start.txt"
adb -s $S shell dumpsys activity service $SVC | tr -d '\r' >> "$OUT/session-start.txt"
w 30
echo "notice: $(bash $D/pad.sh circle 200)"; w 28
echo "title/offline: $(bash $D/pad.sh circle 200)"; w 8
echo "continue: $(bash $D/pad.sh circle 200)"
for i in $(seq 1 16); do f=$(rate); echo "load-wait t=$((i*5)) fps~$f"; [ "$i" -gt 8 ] && [ "$f" -gt 8 ] && break; done
echo "settling 20 s"; w 20
adb -s $S exec-out screencap -p > "$OUT/arrived.png"
echo "arrived: fps~$(rate) stage=$(stage) pid=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r')"
