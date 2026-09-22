#!/usr/bin/env bash
# Log MemAvailable, shadps4 RSS/VmSwap and host_present every 5 s until the process dies or N samples.
S=PB3110PGL6240001G
SVC="com.shadps4.android/.service.FexSessionService"
N="${1:-120}"; OUT="${2:-/c/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad/swan/memwatch.log}"
for i in $(seq 1 $N); do
  adb -s $S shell 'p=$(pidof com.shadps4.android); if [ -z "$p" ]; then echo "t=$(date +%T) no-process"; exit 0; fi; echo "t=$(date +%T) pid=$p avail_kb=$(awk "/MemAvailable/{print \$2}" /proc/meminfo) rss_kb=$(awk "/VmRSS/{print \$2}" /proc/$p/status) swap_kb=$(awk "/VmSwap/{print \$2}" /proc/$p/status) presents=$(dumpsys activity service com.shadps4.android/.service.FexSessionService 2>/dev/null | awk "/host_present/{gsub(/[()]/,\"\",\$2);print \$2}") stage=$(dumpsys activity service com.shadps4.android/.service.FexSessionService 2>/dev/null | awk "/stage:/{print \$2}")' | tr -d '\r' | tee -a "$OUT"
  sleep 5
done
