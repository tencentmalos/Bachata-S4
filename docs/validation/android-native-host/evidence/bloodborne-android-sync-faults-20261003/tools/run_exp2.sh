#!/usr/bin/env bash
# New build (defaults: watch_decay off, fault_textures skip, guest_affinity on): launch Bloodborne,
# reach Central Yharnam, A/B each new default; stop when the battery runs low.
export MSYS_NO_PATHCONV=1
S=${SERIAL:-01108YHE01017563}
SP="$(cd "$(dirname "$0")" && pwd)"
cd "$SP"
export MIN_BATTERY=${MIN_BATTERY:-8}
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
adb -s $S shell 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard; am start -W -n com.shadps4.android/.MainActivity -f 0x24000000 --es game_id CUSA03023' | tail -1
python android_nav.py 300 | tail -2
sleep 60
echo "== state after settle"
bus guest_affinity status
bus wake_proxy status | grep -E "placement|full|max_depth"
bus upload_diag status | grep -E "watch_coalesce|write_faults|fault_textures"
echo "== baseline window (defaults)"
group env SETTLE=5 python ab_content.py 20 base "guest_affinity" on
echo "== guest_affinity"
group env SETTLE=15 python ab_content.py 20 ga "guest_affinity" off on off on off on
bus guest_affinity on | tail -1
echo "== fault_textures"
group env SETTLE=15 python ab_content.py 20 ft "upload_diag fault_textures" always skip always skip always skip
bus upload_diag fault_textures skip | tail -1
echo "== watch_cross with decay off"
group env SETTLE=15 python ab_content.py 20 cx "upload_diag watch_cross" off on off on off on
bus upload_diag watch_cross off | tail -1
echo "== watch_decay (now off by default)"
group env SETTLE=15 python ab_content.py 20 dk "upload_diag watch_decay" on off on off on off
finish
