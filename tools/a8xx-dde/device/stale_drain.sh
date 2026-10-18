f=/sys/class/kgsl/kgsl-3d0/snapshot/dump
cat $f > /data/local/tmp/stale-snapshot.bin 2>/dev/null
chmod 644 /data/local/tmp/stale-snapshot.bin
echo "stale_snapshot_bytes: $(stat -c %s /data/local/tmp/stale-snapshot.bin)"
