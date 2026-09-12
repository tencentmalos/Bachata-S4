#!/usr/bin/env bash
# Compatibility entry point; the maintained source census lives under scripts/android.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to an installed NDK}"
exec python3 "$ROOT/scripts/android/check-host-ndk-sources.py" \
  --ndk "$ANDROID_NDK_HOME" --out "${NDK_SWEEP_OUT:-$ROOT/build/ndk-graphics-sweep}" \
  --set graphics "$@"
