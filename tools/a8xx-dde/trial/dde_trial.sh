#!/usr/bin/env bash
# dde_trial.sh TAG ARM STREAM WATCH_S
#   cold start GoW3 for ARM (A|B|C), enable stream_submit STREAM early (0 = leave off), load the newest save,
#   then watch WATCH_S seconds for a KGSL fault. A kgsl ftrace instance runs for the whole trial.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
DEV="$S/../device"
SERIAL="${ANDROID_SERIAL:-6edb0593}"
ROOT="${DDE_OUT:-$(cd "$S/../../.." && pwd)/build/validation/a8xx-dde}"
mkdir -p "$ROOT/trials"
# Windows paths for adb push / pull (Git Bash); identity elsewhere.
winpath() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf %s "$1"; fi; }
W="$(winpath "$ROOT")"
export GOW3_OUT="$W"
TAG=$1; ARM=$2; STREAM=$3; WATCH=$4
D=$ROOT/trials/$TAG; WD=$W/trials/$TAG; mkdir -p $D
A() { MSYS_NO_PATHCONV=1 adb -s "$SERIAL" "$@"; }
R() { MSYS_NO_PATHCONV=1 bash $S/rootsh-odin3.sh "$1" 2>&1 | grep -v pushed; }
up() { A shell cat /proc/uptime | cut -d' ' -f1 | tr -d '\r'; }
log() { echo "$(date +%H:%M:%S) up=$(up) $*" | tee -a $D/timeline.txt; }
fc0=$(A shell cat /sys/class/kgsl/kgsl-3d0/snapshot/faultcount | tr -d '\r')
log "trial $TAG arm=$ARM stream=$STREAM watch=$WATCH faultcount=$fc0"
R "$(winpath "$DEV/trace_start.sh")" | tee -a $D/timeline.txt
sz=$(R "$(winpath "$DEV/stale_drain.sh")" | tee -a $D/timeline.txt | grep -o "stale_snapshot_bytes: [0-9]*" | grep -o "[0-9]*$")
if [ -n "$sz" ] && [ "$sz" -gt 0 ]; then A pull /data/local/tmp/stale-snapshot.bin "$WD/kgsl-snapshot-stale-before-trial.bin" > /dev/null 2>&1; fi
A shell rm -f /data/local/tmp/stale-snapshot.bin
rm -f $ROOT/pad-action-id.txt
python -I $S/gow3_ab.py prep "$ARM" | tee -a $D/timeline.txt
log "launched"
if [ "$STREAM" != 0 ]; then
  for i in $(seq 1 20); do
    sleep 2
    out=$(python -I $S/gow3.py bus upload_diag stream_submit $STREAM 2>&1 | tr -d '\r' | grep -o 'stream_submit=[0-9]*')
    [ "$out" = "stream_submit=$STREAM" ] && break
  done
  log "stream_submit -> $out"
fi
sleep 20
PID=$(A shell pidof com.shadps4.android | tr -d '\r')
A shell run-as com.shadps4.android cat files/host/log/android-host.log | grep -a -o 'Turnip TU_DEBUG=[^ ]*' | tail -1 | tee -a $D/timeline.txt
A logcat -d -v time | grep -a -o -E 'Turnip: cs_dsb issues[^,]*|Turnip: cs_recycle keeps[^(]*|Turnip: cs_skip_head leaves|Turnip: dde_touch reads|Turnip: dde_check waits|native-drivers/[0-9a-f]{16}' | sort | uniq -c | tee -a $D/timeline.txt
for m in 0x8 0x8 0x4000; do python -I $S/gow3.py pad $m 150 > /dev/null; sleep 3; done
sleep 3
python -I $S/gow3.py pad 0x40 150 > /dev/null; sleep 1.5
python -I $S/gow3.py pad 0x2000 150 > /dev/null; sleep 4
python -I $S/gow3.py shot "trials/$TAG/load-dialog" > /dev/null
A shell 'sendevent /dev/input/event8 1 304 1; sendevent /dev/input/event8 0 0 0; sleep 0.15; sendevent /dev/input/event8 1 304 0; sendevent /dev/input/event8 0 0 0'
T0=$(date +%s); log "load pressed pid=$PID"
result=nofault; shot_done=0
while [ $(( $(date +%s) - T0 )) -lt $WATCH ]; do
  sleep 1
  fc=$(A shell cat /sys/class/kgsl/kgsl-3d0/snapshot/faultcount 2>/dev/null | tr -d '\r')
  if [ -n "$fc" ] && [ "$fc" != "$fc0" ]; then
    result=fault$fc; log "FAULT $fc0->$fc at +$(( $(date +%s) - T0 ))s after load"
    R "$(winpath "$DEV/trace_stop.sh")" | tee -a $D/timeline.txt
    R "$(winpath "$DEV/fault_collect.sh")" > $D/dmesg-kgsl.txt
    A pull /data/local/tmp/fw-snapshot.bin "$WD/kgsl-snapshot-fault$fc.bin" > /dev/null 2>&1
    A shell rm -f /data/local/tmp/fw-snapshot.bin
    grep -a -o -E 'CP [A-Z_ ]*opcode error[^|]*\| opcode=0x[0-9a-f]+|Preemption Fault[^,]*|GPU hang|syncpoint deadlock' $D/dmesg-kgsl.txt | sort | uniq -c | tee -a $D/timeline.txt
    break
  fi
  cur=$(A shell pidof com.shadps4.android 2>/dev/null | tr -d '\r')
  if [ "$cur" != "$PID" ]; then result=proc_ended; log "process $PID ended (now '$cur')"; break; fi
  if [ $shot_done = 0 ] && [ $(( $(date +%s) - T0 )) -ge 40 ]; then python -I $S/gow3.py shot "trials/$TAG/level-40s" > /dev/null; R "$(winpath "$DEV/gpu_state.sh")" | tee -a $D/timeline.txt; shot_done=1; fi
done
[ "$result" = nofault ] && { log "no fault within ${WATCH}s"; python -I $S/gow3.py shot "trials/$TAG/end" > /dev/null; R "$(winpath "$DEV/trace_stop.sh")" | tee -a $D/timeline.txt; }
A pull /data/local/tmp/shadps4dde-trace.txt "$WD/kgsl-trace.txt" > /dev/null 2>&1 && A shell rm -f /data/local/tmp/shadps4dde-trace.txt
A shell run-as com.shadps4.android cat files/host/log/android-host.log > $D/android-host.log 2>&1
A logcat -d -v threadtime -t 4000 > $D/logcat-tail.txt 2>&1
A logcat -b crash -d > $D/logcat-crash.txt 2>&1
A shell rm -f /data/local/tmp/rootsh_in.sh /data/local/tmp/rootsh_out.txt
log "RESULT $TAG $result"
echo "{\"tag\":\"$TAG\",\"arm\":\"$ARM\",\"stream\":$STREAM,\"result\":\"$result\",\"load_epoch\":$T0,\"end_epoch\":$(date +%s)}" >> $ROOT/trials/trials.jsonl
