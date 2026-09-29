#!/usr/bin/env bash
# One Bloodborne soak run on Swan with the APK's bundled Turnip: set the host thread-priority
# switch, launch to the save point, sample the spawn view, turn to the clinic interior, sample
# again and stay SOAK seconds. Records the driver the session loaded, the thread_priority
# status, whether the process survived, KGSL faults of that pid (first fault type, IB1/IB2) and
# Turnip allocation errors.
# Usage: bash run_soak.sh <outdir> <label> <thread_priority 1|0> [soak_seconds=120]
set -u
OUT="$1"; L="$2"; TP="$3"; SOAK="${4:-120}"; S=PB3110PGL6240001G
SVC=com.shadps4.android/.service.FexSessionService; D=$(dirname "$0"); mkdir -p "$OUT"
LOG=/data/data/com.shadps4.android/files/host/log/android-host.log
adb -s $S shell setprop debug.shadps4.vulkan_driver '""'
if [ "$TP" = "0" ]; then adb -s $S shell setprop debug.shadps4.thread_priority 0
else adb -s $S shell setprop debug.shadps4.thread_priority '""'; fi
echo "== $L thread_priority=[$(adb -s $S shell getprop debug.shadps4.thread_priority | tr -d '\r')] $(date +%H:%M:%S)" | tee -a "$OUT/runs.txt"
bash "$D/launch_bb_pad.sh" "$OUT/$L-launch" > "$OUT/$L-launch.log" 2>&1
PID=$(sed -n 's/^session: pid=\([0-9]*\).*/\1/p' "$OUT/$L-launch/session-start.txt" | head -1)
echo "   pid=$PID $(tail -1 "$OUT/$L-launch.log")" | tee -a "$OUT/runs.txt"
adb -s $S shell "grep -a 'sha256=' $LOG | tail -1" | tr -d '\r' | grep -o 'info=[^ ]* [^ ]*\|sha256=[0-9a-f]*' | tr '\n' ' ' | sed 's/^/   loaded: /' | tee -a "$OUT/runs.txt"; echo | tee -a "$OUT/runs.txt"
alive() { [ -n "$PID" ] && [ "$(adb -s $S shell pidof com.shadps4.android | tr -d '\r')" = "$PID" ]; }
survived=-1
if alive; then
  adb -s $S shell dumpsys activity service $SVC thread_priority status 2>/dev/null | tr -d '\r' > "$OUT/$L-tpstatus.txt"
  bash "$D/sample_tp.sh" "$OUT" "$L-spawn" 10 > /dev/null
  grep "^== $L-spawn" "$OUT/summary.txt" | tail -1 | sed 's/^/   /' | tee -a "$OUT/runs.txt"
  PAD_STATE="$OUT/pad-action-id" bash "$D/pad.sh" none 1500 0 0 1 0 > /dev/null
  sleep 5
  if alive; then
    bash "$D/sample_tp.sh" "$OUT" "$L-inner" 10 > /dev/null
    grep "^== $L-inner" "$OUT/summary.txt" | tail -1 | sed 's/^/   /' | tee -a "$OUT/runs.txt"
  fi
  survived=0
  for i in $(seq 10 10 "$SOAK"); do
    adb -s $S shell "read -t 10 </dev/zero" 2>/dev/null
    if alive; then survived=$i; else break; fi
  done
fi
sleep 3
adb -s $S shell "logcat -d -b main,system,crash 2>/dev/null | grep -E '($PID|MESA|TU|turnip).*(ErrorDeviceLost|terminal outcome|GPUOBJ|IB address range|has died)|Process com.shadps4.android \\(pid $PID\\)'" \
  | tr -d '\r' > "$OUT/$L-logcat.txt"
adb -s $S shell "dmesg 2>/dev/null | grep -A6 'opcode error' | grep -B1 -A5 'shadps4.android\\[$PID\\]'" | tr -d '\r' > "$OUT/$L-kgsl.txt"
adb -s $S shell "dmesg 2>/dev/null | grep -B1 'shadps4.android\\[$PID\\]' | grep -o 'CP[A-Za-z ]*opcode error[a-z ]*' | head -1" | tr -d '\r' > "$OUT/$L-fault.txt"
fault=$(cat "$OUT/$L-fault.txt")
ibs=$(grep -m1 -o 'BR: rb [0-9a-f/]* ib1 [0-9A-F]*/[0-9a-f]* ib2 [0-9A-F]*/[0-9a-f]*' "$OUT/$L-kgsl.txt" 2>/dev/null)
lost=$(grep -c 'ErrorDeviceLost' "$OUT/$L-logcat.txt")
alloc=$(grep -c 'GPUOBJ\|IB address range' "$OUT/$L-logcat.txt")
end=$(alive && echo alive || echo gone)
echo "   result: $end survived_soak=${survived}s device_lost_lines=$lost alloc_errors=$alloc fault=[${fault:-none}] $ibs" | tee -a "$OUT/runs.txt"
