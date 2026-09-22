#!/usr/bin/env bash
# Poll host_present rate every 5 s for N iterations; print rate; optional screenshot label.
S=PB3110PGL6240001G
SVC="com.shadps4.android/.service.FexSessionService"
N="${1:-12}"; SHOT="${2:-}"
present() { adb -s $S shell dumpsys activity service $SVC | awk '/host_present/{gsub(/[()]/,"",$2);print $2}'; }
for i in $(seq 1 $N); do a=$(present); sleep 5; b=$(present); echo "t=$((i*5)) fps~$(( (${b:-0}-${a:-0})/5 )) presents=$b"; done
if [ -n "$SHOT" ]; then
  export MSYS_NO_PATHCONV=1
  adb -s $S shell screencap -p /data/local/tmp/$SHOT.png
  adb -s $S pull /data/local/tmp/$SHOT.png "$(cygpath -w /c/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad/swan/$SHOT.png)" | tail -1
fi
