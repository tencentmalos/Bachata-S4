#!/system/bin/sh
# Bounded sched-only ftrace (64 MiB total) capture in an isolated tracefs instance (root).
# Usage: sh sched-capture.sh <pid> <seconds> <outdir>
set -eu
PID="$1"; SECS="$2"; D="$3"
I=/sys/kernel/tracing/instances/shad_sched_$$
mkdir -p "$D"
exec > "$D/control.log" 2>&1
[ "$(id -u)" = 0 ]
[ ! -e "$I" ]
mkdir "$I"
trap 'echo 0 > "$I/tracing_on"; rmdir "$I" 2>/dev/null || true' EXIT
printf 0 > "$I/tracing_on"
N=$(ls -d "$I"/per_cpu/cpu* | wc -l)
K=$((64 * 1024 / N))
echo "$K" > "$I/buffer_size_kb"
echo mono > "$I/trace_clock"
echo 1 > "$I/options/overwrite"
for O in record-cmd record-tgid; do if [ -e "$I/options/$O" ]; then echo 1 > "$I/options/$O"; fi; done
for E in sched/sched_switch sched/sched_waking sched/sched_wakeup power/cpu_frequency; do
  echo 1 > "$I/events/$E/enable"
done
cat /proc/$PID/stat > "$D/process-before.txt"
for T in /proc/$PID/task/*; do echo "$T"; cat "$T/comm"; done > "$D/thread-names.txt"
cat /proc/sys/kernel/random/boot_id > "$D/boot-id.txt"
date +%s%N > "$D/wall-start-ns.txt"
cat /proc/uptime > "$D/uptime-start.txt"
echo 1 > "$I/tracing_on"
sleep "$SECS"
echo 0 > "$I/tracing_on"
cat /proc/uptime > "$D/uptime-end.txt"
for F in "$I"/per_cpu/cpu*/stats; do echo "[$F]"; cat "$F"; done > "$D/per-cpu-stats.txt"
cat /proc/$PID/stat > "$D/process-after.txt"
cat "$I/trace" | gzip -1 > "$D/sched_trace.txt.gz"
chmod -R 755 "$D"
echo collected > "$D/status"
