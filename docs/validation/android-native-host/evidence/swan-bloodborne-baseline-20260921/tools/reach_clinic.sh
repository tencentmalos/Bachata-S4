#!/usr/bin/env bash
# Advance Bloodborne from the intro/title to the clinic on Swan: press on-screen Circle
# (Asian-region confirm) every 20 s until host_present rate drops into the loading band,
# then wait for the scene to settle. Polling only; no properties changed.
S=PB3110PGL6240001G
SVC="com.shadps4.android/.service.FexSessionService"
export MSYS_NO_PATHCONV=1
present() { adb -s $S shell dumpsys activity service $SVC | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
rate() { local a b; a=$(present); sleep 5; b=$(present); echo $(( (${b:-0}-${a:-0})/5 )); }
circle() { adb -s $S shell input swipe 1820 690 1820 690 250; }
for i in $(seq 1 12); do
  circle; sleep 15; f=$(rate); echo "press $i -> fps~$f"
  [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break
done
for i in $(seq 1 40); do f=$(rate); echo "load-wait t=$((i*5)) fps~$f"; [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break; done
echo "settling 60s"; sleep 60
f=$(rate); echo "final fps~$f presents=$(present)"
adb -s $S shell screencap -p /data/local/tmp/swan-clinic.png
adb -s $S pull /data/local/tmp/swan-clinic.png "$(cygpath -w /c/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad/swan/swan-clinic.png)" | tail -1
