#!/usr/bin/env bash
# NDK bionic compile sweep for the renderer-free host-core closure.
set -u
cd /Users/bytedance/workspace/emulations/ps4/shadps4
NDK="$HOME/Library/Android/sdk/ndk/29.0.14206865"
CLANG="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/clang++ 2>/dev/null | head -1)"

INC=( -I externals/fmt/include )
# shellcheck disable=SC2013
for tok in $(cat /tmp/host_inc.txt); do INC+=("$tok"); done

DEFS=( -DUSE_OS_TZDB=1 -DARCH_ARM64=1 -DAL_LIBTYPE_STATIC -DBOOST_ASIO_STANDALONE
       -DHAS_STRING_VIEW=1 -DNOMINMAX -DONLY_C_LOCALE=0 -DPUGIXML_NO_EXCEPTIONS
       -DSPDLOG_FUNCTION=__func__ -DZYCORE_STATIC_BUILD -DZYDIS_STATIC_BUILD -DNDEBUG )

TUS=(
  src/core/linker.cpp src/core/module.cpp src/core/tls.cpp
  src/core/memory.cpp src/core/address_space.cpp
  src/core/aerolib/aerolib.cpp src/core/aerolib/stubs.cpp
  src/core/loader/elf.cpp src/core/loader/symbols_resolver.cpp src/core/loader/dwarf.cpp
)
# extra sets appended by args
if [ "$#" -gt 0 ]; then TUS=("$@"); fi

pass=0; fail=0; failed=()
for tu in "${TUS[@]}"; do
  log="/tmp/ndk_$(echo "$tu" | tr '/' '_').log"
  if "$CLANG" --target=aarch64-linux-android33 -std=gnu++2b "${DEFS[@]}" "${INC[@]}" -fsyntax-only "$tu" 2>"$log"; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); failed+=("$tu")
  fi
done
echo "pass=$pass fail=$fail"
for f in "${failed[@]:-}"; do
  [ -n "$f" ] || continue
  echo "--- FAIL: $f ---"
  head -6 "/tmp/ndk_$(echo "$f" | tr '/' '_').log"
done
