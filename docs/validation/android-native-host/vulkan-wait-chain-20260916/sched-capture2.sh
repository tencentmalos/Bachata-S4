set -eu
tr=/sys/kernel/tracing
[ "$(cat "$tr/tracing_on")" = 0 ] || exit 2
[ "$(cat "$tr/current_tracer")" = nop ] || exit 3
for event in sched_switch sched_wakeup sched_blocked_reason; do
 [ "$(cat "$tr/events/sched/$event/enable")" = 0 ] || exit 4
done
old_buffer=$(cat "$tr/buffer_size_kb")
old_clock=$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$tr/trace_clock")
cleanup() {
 echo 0 > "$tr/tracing_on"
 for event in sched_switch sched_wakeup sched_blocked_reason; do echo 0 > "$tr/events/sched/$event/enable"; done
 echo "$old_clock" > "$tr/trace_clock"
 echo "$old_buffer" > "$tr/buffer_size_kb"
}
trap cleanup EXIT HUP INT TERM
echo 4096 > "$tr/buffer_size_kb"
echo mono > "$tr/trace_clock"
echo > "$tr/trace"
for event in sched_switch sched_wakeup sched_blocked_reason; do echo 1 > "$tr/events/sched/$event/enable"; done
echo 1 > "$tr/tracing_on"
sleep 2
echo 0 > "$tr/tracing_on"
cat "$tr/trace" > /data/local/tmp/shad-queue-sched2.txt
