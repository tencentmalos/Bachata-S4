#!/usr/bin/env bash
# A/B the pending switches in the running Bloodborne session, most valuable first; stop when the
# battery runs low (ab_content.py exits 3).
export MSYS_NO_PATHCONV=1
S=${SERIAL:-01108YHE01017563}
SP="$(cd "$(dirname "$0")" && pwd)"
cd "$SP"
bus() { adb -s $S shell "dumpsys activity service com.shadps4.android $*" | grep -vE "^SERVICE|Client:"; }
restore() {
    bus upload_diag watch_decay on | tail -1
    bus upload_diag watch_cross off | tail -1
    bus upload_diag stream_bounce off | tail -1
    bus wake_proxy on | tail -1
    bus wake_proxy spin 200 | tail -1
    bash aff.sh ff
}
finish() {
    restore
    echo "== final state"
    bus wake_proxy status
    bus upload_diag status | grep -E "watch_coalesce|write_faults"
    adb -s $S shell 'dumpsys battery | grep level; cat /sys/devices/system/cpu/cpu3/core_ctl/active_cpus /sys/devices/system/cpu/cpu7/core_ctl/active_cpus'
    exit 0
}
group() { "$@" || { [ $? -eq 3 ] && finish; }; }
echo "== in-game proxy and upload state"
bus wake_proxy status
bus upload_diag status | grep -E "watch_coalesce|write_faults|gpu_watch"
echo "== baseline window (defaults)"
group env SETTLE=5 python ab_content.py 20 base "upload_diag watch_cross" off
echo "== watch_decay"
group env SETTLE=15 python ab_content.py 20 dk "upload_diag watch_decay" on off on off on off
bus upload_diag watch_decay on | tail -1
echo "== guest thread affinity (all Guest-* but Guest-1): ff = all cpus, f8 = cpus 3-7"
group env SETTLE=15 python ab_content.py 20 af @aff ff f8 ff f8 ff f8
bash aff.sh ff
echo "== stream_bounce"
group env SETTLE=15 python ab_content.py 20 sb "upload_diag stream_bounce" off on off on off on
bus upload_diag stream_bounce off | tail -1
echo "== watch_cross"
group env SETTLE=15 python ab_content.py 20 cx "upload_diag watch_cross" off on off on off on
bus upload_diag watch_cross off | tail -1
echo "== wake proxy on/off"
group env SETTLE=15 python ab_content.py 20 wp "wake_proxy" on off on off on off
bus wake_proxy on | tail -1
echo "== wake proxy spin window"
group env SETTLE=15 python ab_content.py 20 ws - "wake_proxy spin 200" "wake_proxy spin 50" "wake_proxy spin 200" "wake_proxy spin 50"
finish
