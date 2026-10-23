#!/usr/bin/env bash
# run_device.sh TAG [kgsl_slot_repro options...]
#   Push kgsl_slot_repro (built by build.sh), drain a stale KGSL snapshot, run one case as the
#   adb shell user and, if the KGSL fault count moved, collect dmesg and the snapshot as root.
#   Results: $DDE_OUT/minicase/TAG/ (repro.log, result.txt, dmesg-kgsl.txt, kgsl-snapshot-*.bin),
#   one line per case appended to $DDE_OUT/minicase/cases.jsonl.
# Env: ANDROID_SERIAL (6edb0593), ROOTCTL (devices repo rootctl-win), DDE_OUT, MINICASE_BIN,
#      BUSY_ARGS (if set, a second kgsl_slot_repro process runs `--busy-only $BUSY_ARGS` next to
#      the case, so that the GPU switches page tables and preempts; its log is busy.log).
set -u
S="$(cd "$(dirname "$0")" && pwd)"
DEV="$S/../device"
REPO="$(cd "$S/../../.." && pwd)"
SERIAL="${ANDROID_SERIAL:-6edb0593}"
ROOT="${DDE_OUT:-$REPO/build/validation/a8xx-dde}"
BIN="${MINICASE_BIN:-$ROOT/minicase/kgsl_slot_repro}"
export ROOTCTL="${ROOTCTL:-/c/workspace/devices/tools/android-root/rootctl-win}"
winpath() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf %s "$1"; fi; }
A() { MSYS_NO_PATHCONV=1 adb -s "$SERIAL" "$@"; }
# Root scripts: directly when adbd already runs as root (Swan), otherwise through rootctl (Odin3).
if [ "$(A shell id -u | tr -d '\r')" = 0 ]; then
  R() { A push "$1" /data/local/tmp/rootsh_in.sh > /dev/null && A shell sh /data/local/tmp/rootsh_in.sh 2>&1 | tr -d '\r'; }
else
  R() { MSYS_NO_PATHCONV=1 ANDROID_SERIAL="$SERIAL" bash "$S/../trial/rootsh-odin3.sh" "$1" 2>&1 | grep -v pushed; }
fi
TAG=$1; shift
D="$ROOT/minicase/$TAG"; WD="$(winpath "$D")"
[ -e "$D" ] && { echo "$D exists; tags must be unique" >&2; exit 1; }
mkdir -p "$D"
RD=/data/local/tmp/a8xxmini
A shell mkdir -p $RD
A push "$(winpath "$BIN")" $RD/kgsl_slot_repro > /dev/null && A shell chmod 755 $RD/kgsl_slot_repro
sha=$(sha256sum "$BIN" | cut -c1-16)
sz=$(R "$(winpath "$DEV/stale_drain.sh")" | grep -o "stale_snapshot_bytes: [0-9]*" | grep -o "[0-9]*$")
if [ -n "$sz" ] && [ "$sz" -gt 0 ]; then A pull /data/local/tmp/stale-snapshot.bin "$WD/kgsl-snapshot-stale-before.bin" > /dev/null 2>&1; fi
A shell rm -f /data/local/tmp/stale-snapshot.bin
R "$(winpath "$DEV/gpu_state.sh")" > "$D/gpu-before.txt"
fc0=$(A shell cat /sys/class/kgsl/kgsl-3d0/snapshot/faultcount | tr -d '\r')
up0=$(A shell cat /proc/uptime | cut -d' ' -f1 | tr -d '\r')
echo "tag=$TAG bin=$sha args=$* faultcount=$fc0 uptime=$up0 stale_snapshot_bytes=${sz:-0}" | tee "$D/result.txt"
if [ -n "${BUSY_ARGS:-}" ]; then
  echo "busy process: --busy-only $BUSY_ARGS" | tee -a "$D/result.txt"
  A shell "cd $RD && rm -f busy.log && (nohup ./kgsl_slot_repro --busy-only $BUSY_ARGS > busy.log 2>&1 &)"
  sleep 1
fi
A shell "cd $RD && ./kgsl_slot_repro $*; echo exit=\$?" 2>&1 | tr -d '\r' | tee "$D/repro.log"
if [ -n "${BUSY_ARGS:-}" ]; then
  for i in $(seq 1 15); do
    [ "$(A shell pidof kgsl_slot_repro | tr -d '\r')" = "" ] && break
    sleep 1
  done
  A shell "pkill -f 'kgsl_slot_repro --busy-only'" > /dev/null 2>&1
  A shell cat $RD/busy.log | tr -d '\r' > "$D/busy.log"
fi
rc=$(grep -o 'exit=[0-9]*' "$D/repro.log" | tail -1 | cut -d= -f2)
fc=$(A shell cat /sys/class/kgsl/kgsl-3d0/snapshot/faultcount | tr -d '\r')
if [ "$fc" != "$fc0" ]; then
  R "$(winpath "$DEV/fault_collect.sh")" > "$D/dmesg-kgsl.txt"
  A pull /data/local/tmp/fw-snapshot.bin "$WD/kgsl-snapshot-fault$fc.bin" > /dev/null 2>&1
  A shell rm -f /data/local/tmp/fw-snapshot.bin
  grep -a -o -E 'CP [A-Z_ ]*opcode error[^|]*\| opcode=0x[0-9a-f]+|GPU PAGE FAULT|GPU hang|Preemption Fault[^,]*|syncpoint deadlock' "$D/dmesg-kgsl.txt" | sort | uniq -c | tee -a "$D/result.txt"
fi
done_line=$(grep -a 'done trials=' "$D/repro.log" | tail -1 | sed 's/^\[[^]]*\] //')
echo "exit=$rc faultcount $fc0 -> $fc" | tee -a "$D/result.txt"
printf '{"tag":"%s","bin":"%s","args":"%s","exit":"%s","faultcount_before":"%s","faultcount_after":"%s","done":"%s"}\n' \
  "$TAG" "$sha" "$*" "$rc" "$fc0" "$fc" "$done_line" >> "$ROOT/minicase/cases.jsonl"
