#!/usr/bin/env bash
# Measurement-only clinic window (no session restart, no intro input).
# Usage: bash measure_clinic.sh <label> <outdir> [warmup_presents=250]
# Records: host-log offset, warmup, 3x10 s FPS windows, gpu_memory snapshot pair
# (snap0/snap1 with the presents delta for break_delta.py), session dump, memory.
set -u
LABEL="$1"; OUT="$2"; WARM="${3:-250}"
S=PB3110PGL6240001G
SVC="com.shadps4.android/.service.FexSessionService"
HOSTLOG=/data/data/com.shadps4.android/files/host/log/android-host.log
RC=/c/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad/swan/rc.sh
export MSYS_NO_PATHCONV=1
mkdir -p "$OUT"
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
wait_s() { adb -s $S shell "read -t $1 </dev/zero" 2>/dev/null; }
snapshot() { adb -s $S shell dumpsys activity service $SVC gpu_memory request >/dev/null 2>&1; wait_s 3; adb -s $S shell dumpsys activity service $SVC gpu_memory status | tr -d '\r' > "$1"; }

echo "knob=$(adb -s $S shell getprop debug.shadps4.scale_side_effect_passes | tr -d '\r') fused_off=$(adb -s $S shell getprop debug.shadps4.fused_readback_off | tr -d '\r')" | tee "$OUT/property.txt"
OFFSET=$(bash $RC "stat -c %s $HOSTLOG" 2>/dev/null | tail -1 | tr -d '\r'); echo "hostlog_offset=$OFFSET" >> "$OUT/property.txt"
adb -s $S shell dumpsys activity service $SVC | tr -d '\r' > "$OUT/session-start.txt"
echo "session: pid=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r') $(grep -E 'stage:|generation:' "$OUT/session-start.txt" | tr '\n' ' ')"

p0=$(present); until p1=$(present) && [ $((p1-p0)) -ge $WARM ]; do wait_s 5; done
echo "warmup presents=$((p1-p0))" | tee "$OUT/warmup.txt"

snapshot "$OUT/snap0.txt"; s0=$(present)
for w in 1 2 3; do a=$(present); t0=$(date +%s%N); wait_s 10; b=$(present); t1=$(date +%s%N); echo "fps_window $w presents=$((b-a)) ns=$((t1-t0)) fps=$(python -c "print(round(($b-$a)*1e9/($t1-$t0),3))")"; done | tee "$OUT/fps.txt"
snapshot "$OUT/snap1.txt"; s1=$(present)
echo "snap_presents_delta=$((s1-s0))" | tee -a "$OUT/fps.txt"
cp "$OUT/snap1.txt" "$OUT/gpu-memory.txt"
adb -s $S shell dumpsys activity service $SVC | tr -d '\r' > "$OUT/session-after.txt"
adb -s $S shell dumpsys activity service $SVC hle_sync status | tr -d '\r' > "$OUT/hle-sync.txt"
bash $RC "tail -c +$((OFFSET+1)) $HOSTLOG | grep -E 'Internal scale|Texture GC |fused readback|texel buffer sync|pass resume|native pass' | head -800" 2>/dev/null | tr -d '\r' > "$OUT/scale-log.txt"
adb -s $S shell 'p=$(pidof com.shadps4.android); echo "rss_kb=$(awk "/VmRSS/{print \$2}" /proc/$p/status) swap_kb=$(awk "/VmSwap/{print \$2}" /proc/$p/status) avail_kb=$(awk "/MemAvailable/{print \$2}" /proc/meminfo)"' | tr -d '\r' | tee "$OUT/memory.txt"
echo "done $LABEL"
