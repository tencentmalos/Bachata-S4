#!/usr/bin/env bash
# Example batch: C (cs_recycle) vs H (cs_recycle,dde_check), two H trials per C trial.
# Arms are defined in gow3_ab.py (ARMS). Tags must be unique across batches.
S="$(cd "$(dirname "$0")" && pwd)"
for i in 1 2 3 4 5 6; do
  for pair in "C c" "H h" "H j"; do
    set -- $pair; arm=$1; tag=m$i$2
    bash $S/dde_trial.sh $tag $arm 200 180 2>&1 | grep -v per_cpu | grep -E "trial|TU_DEBUG|dde_check|stale_snapshot|gpu_state|load pressed|FAULT|opcode|GPU hang|Preemption|no fault|RESULT|ended"
    sleep 5
  done
done
