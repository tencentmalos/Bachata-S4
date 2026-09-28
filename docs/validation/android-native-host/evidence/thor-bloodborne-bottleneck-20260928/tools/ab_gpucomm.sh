#!/usr/bin/env bash
# Same-session A/B of the GpuComm batch-1 switches on AYN Thor. The session must already be at the
# save point. Each phase sets both switches, settles, then runs sample.sh.
# Usage: bash ab_gpucomm.sh <outdir> <seconds> <label:hold:fine> ...
#   hold = upload_diag read_hold on|off, fine = profiler_ring fine on|off
set -u
OUT="$1"; SEC="$2"; shift 2; S=9c2841a4; SVC=com.shadps4.android/.service.FexSessionService
D=$(dirname "$0"); mkdir -p "$OUT"
cmd() { adb -s $S shell dumpsys activity service $SVC "$@" 2>/dev/null | tr -d '\r'; }
for phase in "$@"; do
  IFS=: read -r L HOLD FINE <<<"$phase"
  echo "== $L read_hold=$HOLD fine=$FINE" | tee -a "$OUT/phases.txt"
  cmd upload_diag read_hold "$HOLD" | tail -1 | tee -a "$OUT/phases.txt"
  cmd profiler_ring fine "$FINE" | tail -1 | tee -a "$OUT/phases.txt"
  adb -s $S shell "read -t 6 </dev/zero" 2>/dev/null
  { cmd gpu_memory request >/dev/null; adb -s $S shell "read -t 2 </dev/zero"; cmd gpu_memory status; } | grep -E "stream copies|stream_read_cache" > "$OUT/$L-stream0.txt"
  bash "$D/sample.sh" "$OUT" "$L" "$SEC" | head -8
  { cmd gpu_memory request >/dev/null; adb -s $S shell "read -t 2 </dev/zero"; cmd gpu_memory status; } | grep -E "stream copies|stream_read_cache" > "$OUT/$L-stream1.txt"
  paste -d'\n' "$OUT/$L-stream0.txt" "$OUT/$L-stream1.txt" | tee -a "$OUT/phases.txt"
done
