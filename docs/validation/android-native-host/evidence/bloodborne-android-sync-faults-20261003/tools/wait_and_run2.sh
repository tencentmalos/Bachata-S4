#!/usr/bin/env bash
# Wait for the Pocket DS to charge to TARGET%, then run the new-build experiments.
export MSYS_NO_PATHCONV=1
S=${SERIAL:-01108YHE01017563}
TARGET=${TARGET:-15}
cd "$(dirname "$0")"
while :; do
    level=$(adb -s $S shell dumpsys battery | sed -n 's/^ *level: \([0-9]*\).*/\1/p' | tr -d '\r')
    echo "$(date +%H:%M:%S) battery=${level:-?}"
    if [ -n "$level" ] && [ "$level" -ge "$TARGET" ]; then
        break
    fi
    sleep 120
done
bash ${RUN:-run_exp2.sh}
