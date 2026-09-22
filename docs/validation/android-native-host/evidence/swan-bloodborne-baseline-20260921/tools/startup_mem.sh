#!/usr/bin/env bash
# Attribute the shadPS4 process's early RSS growth. Waits for the process to appear,
# then at fixed offsets captures /proc/<pid>/status, dumpsys meminfo and an RSS
# aggregation of /proc/<pid>/smaps grouped by mapping name (root shell required for
# smaps of another uid; falls back to the summary-only sample when unreadable).
# Usage: bash startup_mem.sh <outdir>
set -u
S=PB3110PGL6240001G
OUT="$1"; mkdir -p "$OUT"
export MSYS_NO_PATHCONV=1
echo "waiting for process $(date -u +%T)" > "$OUT/sampler.log"
p=""
for i in $(seq 1 1200); do
  p=$(adb -s $S shell pidof com.shadps4.android 2>/dev/null | tr -d '\r')
  [ -n "$p" ] && break
  sleep 0.5
done
[ -z "$p" ] && { echo "no process" >> "$OUT/sampler.log"; exit 1; }
t0=$(date +%s%N)
echo "pid=$p first seen $(date -u +%T)" >> "$OUT/sampler.log"
sample() {
  local tag="$1"
  adb -s $S shell "echo '# t=$tag'; grep -E 'VmRSS|VmHWM|VmSwap|RssAnon|RssFile|RssShmem' /proc/$p/status; grep -E 'MemAvailable|SwapFree' /proc/meminfo; echo '# smaps by mapping (kB rss, top 40)'; awk '/^[0-9a-f]+-[0-9a-f]+ /{name=\$6; if(name==\"\")name=\"[anon]\"; for(i=7;i<=NF;i++)name=name\" \"\$i} /^Rss:/{rss[name]+=\$2} /^Swap:/{swap[name]+=\$2} END{for(n in rss) printf \"%10d %10d %s\\n\", rss[n], swap[n], n}' /proc/$p/smaps 2>&1 | sort -rn | head -40" 2>&1 | tr -d '\r' > "$OUT/mem-$tag.txt"
  adb -s $S shell dumpsys meminfo $p 2>/dev/null | tr -d '\r' | sed -n '1,60p' > "$OUT/meminfo-$tag.txt"
  echo "sampled $tag at $(( ($(date +%s%N)-t0)/1000000 )) ms" >> "$OUT/sampler.log"
}
sample 0s
for off in 1 2 4 8 15 30 60 120; do
  while [ $(( ($(date +%s%N)-t0)/1000000000 )) -lt $off ]; do sleep 0.2; done
  [ -z "$(adb -s $S shell pidof com.shadps4.android 2>/dev/null | tr -d '\r')" ] && { echo "process gone before $off s" >> "$OUT/sampler.log"; exit 0; }
  sample "${off}s"
done
echo "done" >> "$OUT/sampler.log"
