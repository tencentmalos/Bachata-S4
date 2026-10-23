#!/usr/bin/env bash
# batch_controls.sh [PREFIX] : layout controls for the minimal reproduction, one run_device.sh
# case each (60 s, m2m reads, stop at the first zero read). Not run yet as a whole (record
# section 10.6). Close other GPU applications on the device first. Tags are PREFIX-*.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
P="${1:-c3}"
FAST="--mode m2m --churn-us 0 --vbo 2048 --vbo-bind-us 200 --vbo-reads 64 --life-ms 3000 --seconds 60"
OUT="${DDE_OUT:-$S/../../../build/validation/a8xx-dde}/minicase/cases.jsonl"
run() { local tag=$1; shift; timeout 300 bash "$S/run_device.sh" "$tag" "$@" > /dev/null 2>&1; tail -1 "$OUT"; }
i=0
for s in 6 0 1 2 3 4 5 7 6 8 9 10 11 12 13 14 15 6; do
  i=$((i + 1))
  run "$P-$i-s$s" $FAST --slot "$s"
done
run "$P-keep" $FAST --slot 6 --keep
run "$P-low5" $FAST --slot 6 --low-keep 5
run "$P-align64k" $FAST --align64k
run "$P-natural" $FAST --slot -1
run "$P-from7" $FAST --slot 6 --range 0x1000:0x2000
run "$P-trw" $FAST --slot 6 --t-rw
run "$P-vbo64-zero" --mode m2m --churn-us 0 --vbo 64 --vbo-reads 64 --life-ms 3000 --seconds 60 --slot 6
run "$P-vbo64-fill" --mode m2m --churn-us 0 --vbo 64 --vbo-fill --vbo-reads 64 --life-ms 3000 --seconds 60 --slot 6
run "$P-reads1" --mode m2m --churn-us 0 --vbo 2048 --vbo-reads 1 --life-ms 3000 --seconds 60 --slot 6
echo BATCH_DONE
