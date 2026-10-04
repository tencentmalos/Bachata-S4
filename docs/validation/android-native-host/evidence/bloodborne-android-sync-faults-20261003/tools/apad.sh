#!/usr/bin/env bash
# apad.sh <mask> [hold_ms] : press a button mask on the current Android session via DebugBus pad.
S=${SERIAL:-01108YHE01017563}
SP="$(dirname "$0")"
st=$(adb -s $S shell 'dumpsys activity service com.shadps4.android debug_status')
pid=$(echo "$st" | sed -n 's/^ *pid: \([0-9]*\).*/\1/p' | head -1)
gen=$(echo "$st" | sed -n 's/^ *generation: \([0-9]*\).*/\1/p' | head -1)
uuid=$(echo "$st" | sed -n 's/^ *run_uuid: \([0-9a-f]*\).*/\1/p' | head -1)
idf="$SP/apad_id.txt"; id=$(( $(cat "$idf" 2>/dev/null || echo 0) + 1 )); echo $id > "$idf"
adb -s $S shell "dumpsys activity service com.shadps4.android pad state $pid $gen $uuid claude $id 0 ${2:-200} $1 0 0 0 0 0 0 0 0 0" | grep -o '"status"[^,]*' | head -1
