"""Advance Bloodborne on the Pocket DS from boot to Central Yharnam without moving the character.

Presses Circle only on a stable low-draw screen (dialog, title, menu) and stops once the world
draws more than 600 per flip.
"""
import os
import re
import subprocess
import sys
import time

SERIAL = os.environ.get("SERIAL", "01108YHE01017563")
HERE = os.path.dirname(os.path.abspath(__file__))
ENV = dict(os.environ, MSYS_NO_PATHCONV="1")


def status():
    out = subprocess.run(["adb", "-s", SERIAL, "shell",
                          "dumpsys activity service com.shadps4.android debug_status"],
                         capture_output=True, text=True, timeout=20, env=ENV).stdout
    flip = re.search(r"guest_flip: (\d+)", out)
    draw = re.search(r"host_draw: (\d+)", out)
    if not flip or not draw:
        return None
    return int(flip.group(1)), int(draw.group(1))


def circle():
    subprocess.run(["bash", os.path.join(HERE, "apad.sh"), "0x2000"], capture_output=True,
                   text=True, timeout=30, env=ENV)


previous = status()
last_rate = None
deadline = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 300)
while time.time() < deadline:
    time.sleep(2)
    current = status()
    if current is None or previous is None:
        previous = current
        continue
    flips = current[0] - previous[0]
    draws = current[1] - previous[1]
    previous = current
    if flips <= 0:
        continue
    rate = draws // flips
    print(f"{time.strftime('%H:%M:%S')} flips/2s={flips} draws/flip={rate}", flush=True)
    if rate > 600:
        print("world reached", flush=True)
        break
    if rate < 100 and rate == last_rate:
        print("  press circle", flush=True)
        circle()
    last_rate = rate
