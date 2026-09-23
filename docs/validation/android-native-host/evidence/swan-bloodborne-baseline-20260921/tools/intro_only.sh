#!/usr/bin/env bash
# Drive the Bloodborne intro of an already running session (save present): confirm (Circle)
# every ~17 s until the present rate drops into the loading band, wait for the load, settle.
S=PB3110PGL6240001G; SVC="com.shadps4.android/.service.FexSessionService"; export MSYS_NO_PATHCONV=1
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
stage() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/stage:/{print $2}' | tr -d '\r'; }
wait_s() { adb -s $S shell "read -t $1 </dev/zero" 2>/dev/null; }
rate() { local a b; a=$(present); wait_s 5; b=$(present); echo $(( (${b:-0}-${a:-0})/5 )); }
for i in $(seq 1 12); do adb -s $S shell input swipe 1820 690 1820 690 250; wait_s 12; f=$(rate); echo "press $i -> fps~$f stage=$(stage)"; [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break; done
for i in $(seq 1 40); do f=$(rate); echo "load-wait t=$((i*5)) fps~$f"; [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break; done
echo "settling 45 s"; wait_s 45
echo "arrived: fps~$(rate) stage=$(stage) pid=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r')"
