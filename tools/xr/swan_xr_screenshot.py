#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Undistorted headset screenshot from Swan (Pico OS system capture).

  python tools/xr/swan_xr_screenshot.py --serial PB3110PGL6240001G out.jpg [--png]

`adb screencap` and scrcpy mirror the physical panel: both eyes, rotated,
after lens distortion and reprojection, so text and geometry cannot be read.
Pico OS's own screenshot (com.picoxr.systemui SCREEN_CAPTURE) composites all
OpenXR layers into one undistorted 2560x1440 view of the current head pose,
the same image the headset's screenshot button produces: the cinema
environment, the game quad and the PSV status layer as the wearer sees them.

The script triggers the capture, waits for a new file in
/sdcard/DCIM/Screenshots, pulls it and deletes only that file from the
device (files that were there before are left alone). It works while a game
runs and does not touch the app. The view follows the headset's real pose:
an unworn headset looks wherever it lies.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

DIR = '/sdcard/DCIM/Screenshots'


def adb(serial, *args, check=True):
    return subprocess.run(['adb', '-s', serial, *args], check=check, capture_output=True, text=True).stdout


def listing(serial):
    return set(adb(serial, 'shell', f'ls {DIR} 2>/dev/null', check=False).split())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--serial', required=True)
    ap.add_argument('out', type=Path)
    ap.add_argument('--png', action='store_true', help='convert the JPEG to PNG (needs Pillow)')
    ap.add_argument('--timeout', type=float, default=10)
    args = ap.parse_args()
    if 'com.picoxr.systemui' not in adb(args.serial, 'shell', 'cmd package list packages com.picoxr.systemui'):
        sys.exit('no com.picoxr.systemui: not a Pico OS headset')
    before = listing(args.serial)
    # systemui ignores the request while the headset sleeps (proximity off,
    # "handleBusiness: powerState=SLEEP"); wake it first.
    if 'mWakefulness=Awake' not in adb(args.serial, 'shell', 'dumpsys power | grep mWakefulness='):
        adb(args.serial, 'shell', 'input keyevent KEYCODE_WAKEUP')
        time.sleep(2.5)
    adb(args.serial, 'shell', 'am startservice -p com.picoxr.systemui -a systemui.intent.action.SCREEN_CAPTURE '
                              '--ei type 0 --es from adb')
    deadline = time.monotonic() + args.timeout
    new = set()
    while time.monotonic() < deadline and not new:
        time.sleep(.5)
        new = listing(args.serial) - before
    if not new:
        sys.exit('no new capture in ' + DIR + ' (check logcat -s ScreenCapture)')
    name = sorted(new)[-1]
    remote = f'{DIR}/{name}'
    # Wait until the writer has finished (size stable).
    size = -1
    while time.monotonic() < deadline:
        now = adb(args.serial, 'shell', f'stat -c %s {remote}').strip()
        if now == size and now != '0':
            break
        size = now
        time.sleep(.4)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    jpeg = args.out.with_suffix('.jpg') if args.png else args.out
    adb(args.serial, 'pull', remote, str(jpeg))
    adb(args.serial, 'shell', f'rm {remote}')
    if args.png:
        from PIL import Image
        Image.open(jpeg).save(args.out)
        jpeg.unlink()
    print('XR_SCREENSHOT', args.out, name)


if __name__ == '__main__':
    main()
