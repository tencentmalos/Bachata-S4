echo "pagefaults_at_fault: $(sed -n 2p /sys/class/kgsl/kgsl-3d0/pagefaults)"
dmesg | grep -i -E 'kgsl|adreno|gmu|preempt|snapshot|opcode|hang' | tail -80
cat /sys/class/kgsl/kgsl-3d0/snapshot/dump > /data/local/tmp/fw-snapshot.bin
chmod 644 /data/local/tmp/fw-snapshot.bin
ls -la /data/local/tmp/fw-snapshot.bin
