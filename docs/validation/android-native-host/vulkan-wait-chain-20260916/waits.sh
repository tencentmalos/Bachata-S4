i=0
while [ "$i" -lt 120 ]; do
  printf 'S %s\n' "$i"
  cat /proc/uptime
  for tid in 13992 14012 14043; do
    printf '%s ' "$tid"
    cat /proc/8463/task/$tid/wchan
    printf ' '
    cat /proc/8463/task/$tid/syscall
  done
  i=$((i + 1))
  sleep 0.05
done
