#!/usr/bin/env bash
# DebugBus pad input for the current session (docs/debugbus-pad.md).
# Usage: bash pad.sh <button|mask> [hold_ms=150] [lx ly rx ry]
#   buttons: cross circle square triangle up down left right l1 r1 l2 r2 l3 r3 options touchpad none
set -u
S=PB3110PGL6240001G; SVC=com.shadps4.android/.service.FexSessionService; OWNER=claude_swan
STATE=/c/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad/swan/pad-action-id
declare -A M=([cross]=0x4000 [circle]=0x2000 [square]=0x8000 [triangle]=0x1000 [up]=0x10 [down]=0x40 [left]=0x80 [right]=0x20 [l1]=0x400 [r1]=0x800 [l2]=0x100 [r2]=0x200 [l3]=2 [r3]=4 [options]=8 [touchpad]=0x100000 [none]=0)
B="${M[$1]:-$1}"; HOLD="${2:-150}"; LX="${3:-0}"; LY="${4:-0}"; RX="${5:-0}"; RY="${6:-0}"
DUMP=$(adb -s $S shell dumpsys activity service $SVC 2>/dev/null | tr -d '\r')
PID=$(echo "$DUMP" | awk '/^ *pid:/{print $2; exit}'); GEN=$(echo "$DUMP" | awk '/^ *generation:/{print $2; exit}'); UUID=$(echo "$DUMP" | awk '/^ *run_uuid:/{print $2; exit}')
ID=$(( $(cat "$STATE" 2>/dev/null || echo 0) + 1 )); echo $ID > "$STATE"
adb -s $S shell dumpsys activity service $SVC pad state $PID $GEN $UUID $OWNER $ID 0 $HOLD $B $LX $LY $RX $RY 0 0 0 0 0 2>/dev/null | tr -d '\r' | grep -o '"status":"[a-z_]*"' | head -1
