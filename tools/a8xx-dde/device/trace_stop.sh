T=/sys/kernel/tracing/instances/shadps4dde
[ -d $T ] || { echo "no instance"; exit 0; }
on=$(cat $T/tracing_on)
echo "pagefaults_at_stop: $(sed -n 2p /sys/class/kgsl/kgsl-3d0/pagefaults)"
echo 0 > $T/tracing_on
grep -E "entries|overrun" $T/per_cpu/cpu*/stats | grep -E "overrun|entries" | tr '\n' ' '
echo
cat $T/trace > /data/local/tmp/shadps4dde-trace.txt
chmod 644 /data/local/tmp/shadps4dde-trace.txt
echo 0 > $T/events/enable
echo > $T/events/kgsl/adreno_gpu_fault/trigger 2>/dev/null
echo '!traceoff' > $T/events/kgsl/adreno_gpu_fault/trigger 2>/dev/null
rmdir $T && echo "instance removed (was tracing_on=$on) uptime=$(cat /proc/uptime)"
ls -la /data/local/tmp/shadps4dde-trace.txt
