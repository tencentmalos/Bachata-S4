#!/bin/bash
# wsl-turnip-prepare.sh COMMIT : in WSL, check out a local Turnip commit from the Windows checkout's
# submodule git dir into a minimal tree that scripts/android/build-turnip-source can build.
# The commit does not need to be pushed. Needs /opt/mesa (a Mesa clone, deep enough for the
# fork's base) and the NDK/meson setup described in CLAUDE.md (Turnip, WSL build).
set -euo pipefail
COMMIT="$1"
WIN_REPO="${WIN_REPO:-/mnt/d/workspace/shadps4}"
GITDIR="$WIN_REPO/.git/modules/references/mesa-turnip"
TREE="${TURNIP_TREE:-/opt/shadps4-turnip/tree}"
git config --global --add safe.directory '*' || true
cd /opt/mesa
git cat-file -e "$COMMIT^{commit}" 2>/dev/null || git fetch --no-tags "$GITDIR" "$COMMIT"
git cat-file -e "$COMMIT^{commit}"
mkdir -p "$TREE/scripts/android" "$TREE/runtime/locks" "$TREE/references"
tr -d '\r' < "$WIN_REPO/scripts/android/build-turnip-source" > "$TREE/scripts/android/build-turnip-source"
tr -d '\r' < "$WIN_REPO/runtime/locks/turnip-bionic-mainline.json" > "$TREE/runtime/locks/turnip-bionic-mainline.json"
chmod +x "$TREE/scripts/android/build-turnip-source"
if [ -d "$TREE/references/mesa-turnip" ]; then
  git -C "$TREE/references/mesa-turnip" checkout --force --detach "$COMMIT"
  git -C "$TREE/references/mesa-turnip" clean -fdq
else
  git -C /opt/mesa worktree add --detach "$TREE/references/mesa-turnip" "$COMMIT"
fi
git -C "$TREE/references/mesa-turnip" log --oneline -1
echo PREPARE_OK
