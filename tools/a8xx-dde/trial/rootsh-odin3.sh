#!/usr/bin/env bash
# rootsh-odin3.sh <local-script> : push a script to the Odin3, run it as root, print its output.
# Root goes through the devices repo bridge (rootctl); set ROOTCTL / ANDROID_SERIAL to override.
set -e
SERIAL="${ANDROID_SERIAL:-6edb0593}"
ROOTCTL="${ROOTCTL:-/d/workspace/devices/tools/android-root/rootctl-win}"
adb -s "$SERIAL" push "$1" /data/local/tmp/rootsh_in.sh >/dev/null
bash "$ROOTCTL" --serial "$SERIAL" exec -- "sh /data/local/tmp/rootsh_in.sh > /data/local/tmp/rootsh_out.txt 2>&1; chmod 644 /data/local/tmp/rootsh_out.txt" >/dev/null 2>&1 || true
adb -s "$SERIAL" shell cat /data/local/tmp/rootsh_out.txt
