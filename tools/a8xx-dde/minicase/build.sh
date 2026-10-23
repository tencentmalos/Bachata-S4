#!/usr/bin/env bash
# build.sh [OUT] : build kgsl_slot_repro for arm64 Android (static, API 33) with the NDK.
# NDK: $ANDROID_NDK_HOME / $ANDROID_NDK_ROOT, else the newest one under the SDK.
# The only header besides the NDK sysroot is the kgsl UAPI header from the Turnip checkout.
set -euo pipefail
S="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$S/../../.." && pwd)"
OUT="${1:-$REPO/build/validation/a8xx-dde/minicase/kgsl_slot_repro}"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$NDK" ]; then
  for sdk in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" "${LOCALAPPDATA:-}/Android/Sdk" "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do
    [ -n "$sdk" ] && [ -d "$sdk/ndk" ] || continue
    NDK="$sdk/ndk/$(ls "$sdk/ndk" | sort -V | tail -1)"
    break
  done
fi
[ -d "$NDK" ] || { echo "NDK not found; set ANDROID_NDK_HOME" >&2; exit 1; }
BIN=$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin | head -1)
CC="$BIN/aarch64-linux-android33-clang"
[ -e "$CC" ] || CC="$CC.cmd"
KGSL_INC="$REPO/references/mesa-turnip/src/freedreno/vulkan"
[ -f "$KGSL_INC/msm_kgsl.h" ] || { echo "missing $KGSL_INC/msm_kgsl.h (init references/mesa-turnip)" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
"$CC" -O2 -g -Wall -Wextra -std=gnu11 -static -I"$KGSL_INC" "$S/kgsl_slot_repro.c" -o "$OUT"
echo "$OUT"
sha256sum "$OUT" 2>/dev/null || shasum -a 256 "$OUT"
