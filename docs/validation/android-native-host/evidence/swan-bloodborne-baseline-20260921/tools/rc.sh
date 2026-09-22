#!/usr/bin/env bash
# Swan root exec wrapper: bash rc.sh "<command>"
export MSYS_NO_PATHCONV=1 ANDROID_SDK_ROOT=/c/Users/Admin/AppData/Local/Android/Sdk TMPDIR="C:/Users/Admin/AppData/Local/Temp"
cd /c/workspace/devices && exec bash tools/android-root/rootctl-win --serial PB3110PGL6240001G exec -- "$@"
