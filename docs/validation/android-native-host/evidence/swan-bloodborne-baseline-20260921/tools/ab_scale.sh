#!/usr/bin/env bash
# Swan Bloodborne clinic A/B for the Render Scale coverage work.
# Usage: bash ab_scale.sh <label> <side_effect_knob 0|1> <outdir> [restart_session 0|1]
# Sets debug.shadps4.scale_side_effect_passes, (re)starts the game session so the
# Instance re-reads the policy, drives the intro to the clinic with on-screen Circle,
# warms up, measures 3x10 s FPS windows, snapshots gpu_memory diagnostics and the
# StatusLayer, and extracts this session's native-pass / GC log lines.
set -u
LABEL="$1"; KNOB="$2"; OUT="$3"; RESTART="${4:-1}"
S=PB3110PGL6240001G
SVC="com.shadps4.android/.service.FexSessionService"
HOSTLOG=/data/data/com.shadps4.android/files/host/log/android-host.log
RC=/c/Users/Admin/AppData/Local/Temp/claude/C--workspace-emulations-shadps4/eb61724d-810d-4431-a1e0-50dbc27fcf2d/scratchpad/swan/rc.sh
export MSYS_NO_PATHCONV=1
mkdir -p "$OUT"
present() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/host_present/{gsub(/[()]/,"",$2);print $2}' | tr -d '\r'; }
rate() { local a b; a=$(present); sleep 5; b=$(present); echo $(( (${b:-0}-${a:-0})/5 )); }
circle() { adb -s $S shell input swipe 1820 690 1820 690 250; }
stage() { adb -s $S shell dumpsys activity service $SVC 2>/dev/null | awk '/stage:/{print $2}' | tr -d '\r'; }
shot() { adb -s $S shell screencap -p /data/local/tmp/ab.png; adb -s $S pull /data/local/tmp/ab.png "$(cygpath -w "$OUT/$1.png")" >/dev/null; adb -s $S shell rm -f /data/local/tmp/ab.png; }

adb -s $S shell setprop debug.shadps4.scale_side_effect_passes "$KNOB"
adb -s $S shell setprop debug.shadps4.profile_sync '""'
echo "knob=$(adb -s $S shell getprop debug.shadps4.scale_side_effect_passes | tr -d '\r')" | tee "$OUT/property.txt"
OFFSET=$(bash $RC "stat -c %s $HOSTLOG" 2>/dev/null | tail -1 | tr -d '\r'); echo "hostlog_offset=$OFFSET" >> "$OUT/property.txt"

if [ "$RESTART" = "1" ] && [ "$(stage)" = "Running" ]; then
  adb -s $S shell am startservice -n $SVC -a com.shadps4.android.action.STOP_EMULATION >/dev/null 2>&1
  for i in $(seq 1 20); do sleep 2; st=$(stage); [ "$st" = "Stopped" ] && break; done; echo "stopped: $(stage)"
  sleep 5
fi
if [ -z "$(adb -s $S shell pidof com.shadps4.android | tr -d '\r')" ]; then
  adb -s $S shell am start -W -n com.shadps4.android/.MainActivity -f 0x24000000 --ez open_last_game true | grep Status; sleep 8
fi
adb -s $S shell am start -W -n com.shadps4.android/.MainActivity -f 0x24000000 --ez open_last_game true | grep Status
for i in $(seq 1 30); do sleep 2; [ "$(stage)" = "Running" ] && break; done
echo "session: pid=$(adb -s $S shell pidof com.shadps4.android | tr -d '\r') stage=$(stage)" | tee "$OUT/session-start.txt"
adb -s $S shell dumpsys activity service $SVC | tr -d '\r' > "$OUT/session-start.txt"
sleep 20
# Intro: Circle every 15 s until the present rate drops into the loading/clinic band.
for i in $(seq 1 12); do circle; sleep 15; f=$(rate); echo "press $i -> fps~$f"; [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break; done
for i in $(seq 1 40); do f=$(rate); echo "load-wait t=$((i*5)) fps~$f"; [ "$f" -lt 15 ] && [ "$f" -gt 2 ] && break; done
sleep 60
p0=$(present); until p1=$(present) && [ $((p1-p0)) -ge 250 ]; do sleep 5; done
echo "warmup presents=$((p1-p0))" | tee "$OUT/warmup.txt"
shot scene-before
for w in 1 2 3; do a=$(present); t0=$(date +%s%N); sleep 10; b=$(present); t1=$(date +%s%N); echo "fps_window $w presents=$((b-a)) ns=$((t1-t0)) fps=$(python -c "print(round(($b-$a)*1e9/($t1-$t0),3))")"; done | tee "$OUT/fps.txt"
adb -s $S shell dumpsys activity service $SVC gpu_memory request >/dev/null 2>&1; sleep 3
adb -s $S shell dumpsys activity service $SVC gpu_memory status | tr -d '\r' > "$OUT/gpu-memory.txt"
adb -s $S shell dumpsys activity service $SVC | tr -d '\r' > "$OUT/session-after.txt"
shot scene-after
bash $RC "tail -c +$((OFFSET+1)) $HOSTLOG | grep -E 'Internal scale|Texture GC ' | head -800" 2>/dev/null | tr -d '\r' > "$OUT/scale-log.txt"
adb -s $S shell 'p=$(pidof com.shadps4.android); echo "rss_kb=$(awk "/VmRSS/{print \$2}" /proc/$p/status) swap_kb=$(awk "/VmSwap/{print \$2}" /proc/$p/status) avail_kb=$(awk "/MemAvailable/{print \$2}" /proc/meminfo)"' | tr -d '\r' | tee "$OUT/memory.txt"
echo "done $LABEL"
