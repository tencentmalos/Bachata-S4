#!/usr/bin/env python3
"""Drive God of War III (emupsp-CUSA01740) on the Odin3 through adb and DebugBus.

  kh3.py launch                 wake the device and start GoW3
  kh3.py status                 debug_status summary (session, pid, generation, run uuid)
  kh3.py pad MASK [HOLD_MS]     press buttons through the DebugBus pad (owner gow3)
  kh3.py shot NAME              screenshot to OUT/NAME.png
  kh3.py fault                  KGSL fault count
  kh3.py prop VALUE             set debug.mesa.tu.debug ('-' clears it)
"""
import json
import os
import re
import subprocess
import sys
import time

SERIAL = os.environ.get("ANDROID_SERIAL", "6edb0593")
PKG = "com.shadps4.android"
SERVICE = f"{PKG}/.service.FexSessionService"
OUT = os.environ.get("GOW3_OUT", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "build", "validation", "a8xx-dde"))
STATE = os.path.join(OUT, "pad-action-id.txt")
ENV = dict(os.environ, MSYS_NO_PATHCONV="1")


def adb(*args, check=True, binary=False):
    proc = subprocess.run(["adb", "-s", SERIAL, *args], capture_output=True, env=ENV)
    if check and proc.returncode != 0:
        raise SystemExit(f"adb {' '.join(args)} failed: {proc.stderr.decode(errors='replace')}")
    return proc.stdout if binary else proc.stdout.decode(errors="replace")


def debugbus(*command):
    return adb("shell", "dumpsys", "activity", "service", SERVICE, *command, check=False)


def session():
    text = debugbus("debug_status")
    info = {}
    for key in ("session", "pid", "generation", "run_uuid", "phase", "stop_reason", "state"):
        m = re.search(rf'"?{key}"?\s*[:=]\s*"?([^",\s}}]+)', text)
        if m:
            info[key] = m.group(1)
    return info, text


def next_action_id():
    os.makedirs(OUT, exist_ok=True)
    value = 1
    if os.path.exists(STATE):
        value = int(open(STATE).read().strip() or "0") + 1
    open(STATE, "w").write(str(value))
    return value


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd = sys.argv[1]
    if cmd == "launch":
        adb("shell", "input", "keyevent", "KEYCODE_WAKEUP")
        adb("shell", "wm", "dismiss-keyguard", check=False)
        print(adb("shell", "am", "start", "-W", "-n", f"{PKG}/.MainActivity", "-f", "0x24000000",
                  "--es", "game_id", "emupsp-CUSA01740"))
    elif cmd == "status":
        info, text = session()
        print(json.dumps(info))
        if "-v" in sys.argv:
            print(text)
    elif cmd == "stick":
        # stick LX LY HOLD_MS [MASK]: left stick in [-1, 1] (held at most 2 s by DebugBus).
        lx, ly, hold = sys.argv[2], sys.argv[3], sys.argv[4]
        mask = sys.argv[5] if len(sys.argv) > 5 else "0"
        info, text = session()
        pid, gen, uuid = info.get("pid"), info.get("generation"), info.get("run_uuid")
        if not (pid and gen and uuid):
            raise SystemExit("no running session:\n" + text)
        action = next_action_id()
        reply = debugbus("pad", "state", pid, gen, uuid, "gow3", str(action), "0", hold, mask,
                         lx, ly, "0", "0", "0", "0", "0", "0", "0")
        line = next((l for l in reply.splitlines() if l.strip().startswith("{")), reply.strip())
        print(line)
    elif cmd == "pad":
        mask = sys.argv[2]
        hold = sys.argv[3] if len(sys.argv) > 3 else "150"
        info, text = session()
        pid, gen, uuid = info.get("pid"), info.get("generation"), info.get("run_uuid")
        if not (pid and gen and uuid):
            raise SystemExit("no running session:\n" + text)
        action = next_action_id()
        reply = debugbus("pad", "state", pid, gen, uuid, "gow3", str(action), "0", hold, mask,
                         "0", "0", "0", "0", "0", "0", "0", "0", "0")
        line = next((l for l in reply.splitlines() if l.strip().startswith("{")), reply.strip())
        print(line)
    elif cmd == "shot":
        name = sys.argv[2]
        os.makedirs(OUT, exist_ok=True)
        data = adb("exec-out", "screencap", "-p", binary=True)
        path = os.path.join(OUT, name + ".png")
        open(path, "wb").write(data)
        print(path)
    elif cmd == "fault":
        print(adb("shell", "cat", "/sys/class/kgsl/kgsl-3d0/snapshot/faultcount").strip())
    elif cmd == "prop":
        value = sys.argv[2]
        adb("shell", f"setprop debug.mesa.tu.debug '{'' if value == '-' else value}'")
        print("debug.mesa.tu.debug=[" + adb("shell", "getprop", "debug.mesa.tu.debug").strip() + "]")
    elif cmd == "bus":
        print(debugbus(*sys.argv[2:]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
