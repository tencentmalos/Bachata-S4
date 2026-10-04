#!/usr/bin/env bash
# athreads.sh [seconds] [label] : per-thread CPU over a window, plus fps, draws/flip and GPU busy,
# for the running shadPS4 Android session. Raw snapshots stay in athreads-<label>.raw.
S=${SERIAL:-01108YHE01017563}
W=${1:-8}
L=${2:-last}
SP="$(cd "$(dirname "$0")" && pwd)"
adb -s $S shell "
P=\$(dumpsys activity service com.shadps4.android debug_status | sed -n 's/^ *pid: \([0-9]*\).*/\1/p' | head -1)
snap() { for t in /proc/\$P/task/*; do read -r line < \$t/stat 2>/dev/null || continue; c=\$(cat \$t/comm 2>/dev/null); echo \"\${t##*/} \$c|\$line\"; done; }
cnt() { dumpsys activity service com.shadps4.android debug_status | awk '/guest_flip:/{f=\$2} /host_draw:/{d=\$2} END{print f, d}'; }
gb() { cat /sys/class/kgsl/kgsl-3d0/gpubusy 2>/dev/null || echo 0 0; }
a=\$(cnt); g0=\$(gb); snap > /data/local/tmp/ath0-$L.txt; t0=\$(date +%s%N)
sleep $W
b=\$(cnt); g1=\$(gb); snap > /data/local/tmp/ath1-$L.txt; t1=\$(date +%s%N)
echo \"PID \$P T \$t0 \$t1 C0 \$a C1 \$b G0 \$g0 G1 \$g1 F \$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null)\"
echo ===0; cat /data/local/tmp/ath0-$L.txt; echo ===1; cat /data/local/tmp/ath1-$L.txt
rm /data/local/tmp/ath0-$L.txt /data/local/tmp/ath1-$L.txt
" > "$SP/athreads-$L.raw"
python "$SP/athreads_parse.py" "$SP/athreads-$L.raw"
