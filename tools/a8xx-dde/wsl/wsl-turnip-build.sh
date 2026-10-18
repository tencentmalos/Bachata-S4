#!/bin/bash
# wsl-turnip-build.sh NAME : build the tree prepared by wsl-turnip-prepare.sh and copy the result to
# $WIN_REPO/build/turnip-wsl-NAME, for SHADPS4_TURNIP_PREBUILT on the Windows host build.
set -euo pipefail
NAME="$1"
WIN_REPO="${WIN_REPO:-/mnt/d/workspace/shadps4}"
TREE="${TURNIP_TREE:-/opt/shadps4-turnip/tree}"
OUT="/opt/shadps4-turnip/out-$NAME"
WIN_OUT="$WIN_REPO/build/turnip-wsl-$NAME"
export PATH=/opt/turnip-venv/bin:/usr/local/bin:/usr/bin:/bin
export SHADPS4_TURNIP_MESON=/opt/turnip-venv/bin/meson
export TURNIP_BUILD_JOBS="${TURNIP_BUILD_JOBS:-20}"
cd "$TREE"
rm -rf "$OUT"
python3 scripts/android/build-turnip-source build --ndk "${ANDROID_NDK:-/opt/android-ndk-r29}" --out "$OUT" 2>&1 | tail -20
rm -rf "$WIN_OUT"; mkdir -p "$WIN_OUT"; cp -r "$OUT/." "$WIN_OUT/"
python3 - "$WIN_OUT" <<'PY'
import hashlib, json, pathlib, sys
out = pathlib.Path(sys.argv[1])
identity = json.loads((out / "identity.json").read_text())
library = out / identity["library"]
print("source_commit", identity.get("source_commit"), "dirty", identity.get("source_dirty"))
print("library_sha256", hashlib.sha256(library.read_bytes()).hexdigest(), identity["library_sha256"])
PY
echo TURNIP_WSL_BUILD_PASS
