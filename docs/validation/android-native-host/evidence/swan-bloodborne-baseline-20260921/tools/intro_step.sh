#!/usr/bin/env bash
# One intro step: optional Circle press, wait, present rate, screencap -> rotated panel crop.
# usage: intro_step.sh <outdir> <tag> <press 0|1> [wait_s=15]
OUT="$1"; TAG="$2"; PRESS="$3"; WAIT="${4:-15}"
S=PB3110PGL6240001G; SVC="com.shadps4.android/.service.FexSessionService"; export MSYS_NO_PATHCONV=1
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
[ "$PRESS" = "1" ] && adb -s $S shell input swipe 1820 690 1820 690 250
adb -s $S shell "read -t $WAIT </dev/zero" 2>/dev/null
a=$(present); adb -s $S shell 'read -t 5 </dev/zero' 2>/dev/null; b=$(present)
echo "$TAG press=$PRESS presents=$b rate~$(( (${b:-0}-${a:-0})/5 )) stage=$(adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/stage:/{print $2}' | tr -d '\r') pid=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r')"
adb -s $S shell screencap -p /data/local/tmp/is.png && adb -s $S pull /data/local/tmp/is.png "$(cygpath -w "$OUT/$TAG.png")" >/dev/null
python - "$(cygpath -w "$OUT/$TAG.png")" "$(cygpath -w "$OUT/$TAG-panel.png")" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]); w, h = im.size
# left eye panel region (upper middle), rotate so the panel reads upright
c = im.crop((int(w*0.17), 0, int(w*0.48), int(h*0.52))).rotate(-80, expand=True, fillcolor=(0,0,0))
c.thumbnail((1100, 800)); c.save(sys.argv[2]); print("panel", c.size)
PY
rm -f "$OUT/$TAG.png"
