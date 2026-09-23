#!/usr/bin/env bash
# On-CPU sampling of the whole emulator process (all threads), fp call graphs.
# Usage: bash perf_record.sh <outdir> [seconds=20] [freq=2000]
set -u
OUT="$1"; SECS="${2:-20}"; FREQ="${3:-2000}"; S=PB3110PGL6240001G; export MSYS_NO_PATHCONV=1
mkdir -p "$OUT"
PID=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r')
adb -s $S shell "for t in /proc/$PID/task/*; do echo \"\${t##*/} \$(cat \$t/comm)\"; done" | tr -d '\r' > "$OUT/threads.txt"
date +%s%N > "$OUT/perf-start-host-ns.txt"
adb -s $S shell "cat /proc/uptime" | tr -d '\r' > "$OUT/perf-start-uptime.txt"
adb -s $S shell "simpleperf record -p $PID -e cpu-clock -f $FREQ --call-graph fp --duration $SECS -o /data/local/tmp/shad-perf.data" 2>&1 | tr -d '\r' | tee "$OUT/perf-record.log"
adb -s $S shell "cat /proc/uptime" | tr -d '\r' > "$OUT/perf-end-uptime.txt"
adb -s $S pull /data/local/tmp/shad-perf.data "$OUT/perf.data" >/dev/null && adb -s $S shell rm /data/local/tmp/shad-perf.data
sha256sum "$OUT/perf.data" | tee "$OUT/perf.data.sha256"
echo "pid=$PID"
