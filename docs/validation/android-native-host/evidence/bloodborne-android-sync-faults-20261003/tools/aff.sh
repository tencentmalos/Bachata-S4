#!/usr/bin/env bash
# aff.sh <hex mask> [name regex] : set the CPU affinity of guest threads (default: every Guest-* thread
# except Guest-1) of the running shadPS4 session, as the app's own UID (run-as).
S=${SERIAL:-01108YHE01017563}
M=$1
R=${2:-'^Guest-([02-9]|1[0-9])'}
adb -s $S shell "P=\$(pidof com.shadps4.android); n=0; f=0; for t in /proc/\$P/task/*; do c=\$(cat \$t/comm); if echo \"\$c\" | grep -qE '$R'; then if run-as com.shadps4.android taskset -p $M \${t##*/} >/dev/null 2>&1; then n=\$((n+1)); else f=\$((f+1)); fi; fi; done; echo affinity $M set=\$n failed=\$f"
