#!/usr/bin/env bash
# With the new defaults running in Central Yharnam: one 10 s Perfetto sched trace, then one
# 10 s simpleperf recording (frame-pointer call graphs), both pulled and deleted on the device.
export MSYS_NO_PATHCONV=1
S=${SERIAL:-01108YHE01017563}
cd "$(dirname "$0")"
L=${1:-r3}
bus() { adb -s $S shell "dumpsys activity service com.shadps4.android $*" | grep -vE "^SERVICE|Client:"; }
f0=$(bus debug_status | sed -n 's/.*guest_flip: \([0-9]*\).*/\1/p' | tr -d '\r')
adb -s $S shell "perfetto --txt -c - -o /data/misc/perfetto-traces/bb-$L.pftrace" < perfetto-sched.cfg > /dev/null 2>&1
f1=$(bus debug_status | sed -n 's/.*guest_flip: \([0-9]*\).*/\1/p' | tr -d '\r')
adb -s $S pull /data/misc/perfetto-traces/bb-$L.pftrace "bb-$L.pftrace" > /dev/null && adb -s $S shell rm /data/misc/perfetto-traces/bb-$L.pftrace
echo "perfetto flips=$((f1 - f0))"
f0=$(bus debug_status | sed -n 's/.*guest_flip: \([0-9]*\).*/\1/p' | tr -d '\r')
adb -s $S shell "simpleperf record --app com.shadps4.android -f 2000 --call-graph fp --duration 10 -o /data/local/tmp/perf-$L.data" 2>&1 | tail -2
f1=$(bus debug_status | sed -n 's/.*guest_flip: \([0-9]*\).*/\1/p' | tr -d '\r')
adb -s $S pull /data/local/tmp/perf-$L.data "perf-$L.data" > /dev/null && adb -s $S shell rm /data/local/tmp/perf-$L.data
echo "simpleperf flips=$((f1 - f0))"
adb -s $S shell 'dumpsys battery | grep level; cat /sys/devices/system/cpu/cpu3/core_ctl/active_cpus /sys/devices/system/cpu/cpu7/core_ctl/active_cpus'
