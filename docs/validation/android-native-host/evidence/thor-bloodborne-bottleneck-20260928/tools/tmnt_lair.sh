#!/usr/bin/env bash
# From the TMNT adventure menu (Continue highlighted): continue into the lair, make the same moves,
# screenshot, sample. Usage: bash tmnt_lair.sh <outdir> <label>
set -u
OUT="$1"; L="$2"; S=9c2841a4; D=$(dirname "$0")
bash "$D/pad.sh" cross 200 >/dev/null
adb -s $S shell "read -t 30 </dev/zero"
bash "$D/pad.sh" none 1500 0 1 0 0 >/dev/null; adb -s $S shell "read -t 1.6 </dev/zero"
bash "$D/pad.sh" none 1200 -1 0 0 0 >/dev/null; adb -s $S shell "read -t 5 </dev/zero"
adb -s $S exec-out screencap -p > "$OUT/$L.png"
bash "$D/sample.sh" "$OUT" "$L" 15 | head -6
