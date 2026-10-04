#!/usr/bin/env bash
# Stop the running session through the UI (Back -> Stop), then turn the screen off to charge.
export MSYS_NO_PATHCONV=1
S=${SERIAL:-01108YHE01017563}
adb -s $S shell input keyevent KEYCODE_BACK
sleep 2
adb -s $S shell uiautomator dump /sdcard/window_dump_bbstop.xml > /dev/null 2>&1
B=$(adb -s $S shell cat /sdcard/window_dump_bbstop.xml | tr '>' '\n' | grep 'text="Stop"' |
    sed -n 's/.*bounds="\[\([0-9]*\),\([0-9]*\)\]\[\([0-9]*\),\([0-9]*\)\]".*/\1 \2 \3 \4/p')
adb -s $S shell rm /sdcard/window_dump_bbstop.xml
if [ -n "$B" ]; then
    set -- $B
    adb -s $S shell input tap $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 ))
    sleep 4
fi
adb -s $S shell 'dumpsys activity service com.shadps4.android debug_status' | grep -E "session:|stop_reason"
adb -s $S shell input keyevent KEYCODE_SLEEP
