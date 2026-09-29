#!/usr/bin/env bash
# Final-render-target snapshot through the embedded scrcpy capture SDK (capture_video):
# records briefly, pulls the H.264 and decodes the last frame. Needs ffmpeg on PATH.
# Usage: bash snap.sh <output.png> [seconds=1]
set -u
SDK=${SCRCPY_CAPTURE_SDK:-C:/workspace/my_mcp_tools/dev_tools/mcp/scrcpy/capture-sdk}
python "$SDK/tools/capturectl.py" --serial PB3110PGL6240001G snapshot "$1" --seconds "${2:-1}"
