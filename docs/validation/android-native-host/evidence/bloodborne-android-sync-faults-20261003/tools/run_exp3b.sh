#!/usr/bin/env bash
# Round 4 on APK 1881c71d, session already in Central Yharnam: A/B watch_cross with decay off
# and decay on (hashing) against off (no hashing); stop when the battery runs low.
export MSYS_NO_PATHCONV=1
S=${SERIAL:-01108YHE01017563}
SP="$(cd "$(dirname "$0")" && pwd)"
cd "$SP"
export MIN_BATTERY=${MIN_BATTERY:-6}
bus() { adb -s $S shell "dumpsys activity service com.shadps4.android $*" | grep -vE "^SERVICE|Client:"; }
restore() {
    bus upload_diag watch_decay off | tail -1
    bus upload_diag fault_textures skip | tail -1
    bus guest_affinity on | tail -1
    bus upload_diag watch_cross off | tail -1
}
finish() {
    restore
    echo "== final state"
    bus guest_affinity status
    bus wake_proxy status
    bus upload_diag status | grep -E "watch_coalesce|write_faults|fault_textures"
    adb -s $S shell 'dumpsys battery | grep level; cat /sys/devices/system/cpu/cpu3/core_ctl/active_cpus /sys/devices/system/cpu/cpu7/core_ctl/active_cpus'
    exit 0
}
group() { "$@" || { [ $? -eq 3 ] && finish; }; }
sleep 30
echo "== state after settle"
bus guest_affinity status
bus wake_proxy status | grep -E "placement|full|max_depth"
bus upload_diag status | grep -E "watch_coalesce|write_faults|fault_textures"
echo "== baseline window (defaults)"
group env SETTLE=5 python ab_content.py 20 base "guest_affinity" on
echo "== watch_cross with decay off"
group env SETTLE=15 python ab_content.py 20 cx "upload_diag watch_cross" off on off on off on
bus upload_diag watch_cross off | tail -1
echo "== watch_decay on (with hashing) / off (no hashing)"
group env SETTLE=15 python ab_content.py 20 dk "upload_diag watch_decay" on off on off on off
finish
