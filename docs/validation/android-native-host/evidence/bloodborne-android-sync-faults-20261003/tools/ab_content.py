"""A/B a DebugBus toggle on the Pocket DS: for each value set it, settle, then sample threads and
the GPU write-watch / content-prediction counters over a window.

usage: ab_content.py <window_s> <label> "<command prefix>" value value ...
"""
import os
import re
import subprocess
import sys
import time

SERIAL = os.environ.get("SERIAL", "01108YHE01017563")
HERE = os.path.dirname(os.path.abspath(__file__))
ENV = dict(os.environ, MSYS_NO_PATHCONV="1")
SETTLE = float(os.environ.get("SETTLE", "10"))


def shell(cmd, timeout=60):
    return subprocess.run(["adb", "-s", SERIAL, "shell", cmd], capture_output=True, text=True,
                          timeout=timeout, env=ENV).stdout


def bus(cmd):
    return shell(f"dumpsys activity service com.shadps4.android {cmd}")


def counters():
    st = bus("debug_status")
    flip = int(re.search(r"guest_flip: (\d+)", st).group(1))
    up = bus("upload_diag status")
    values = {"flip": flip}
    for key in ("release_calls", "release_pages", "predicted_pages", "watch_calls", "watch_pages",
                "syscalls", "hashed_pages", "rewritten_pages", "cold", "cold_known", "after_dirty", "run_start", "run_start_window", "unchanged_pages", "cross_released", "texture_invalidates",
                "texture_skipped", "texture_ns", "kept_pages"):
        m = re.search(rf"\b{key}=(\d+)", up)
        values[key] = int(m.group(1)) if m else 0
    return values


def thermal():
    out = shell("dumpsys thermalservice")
    m = re.search(r"Thermal Status: (\d+)", out)
    active = shell("cat /sys/devices/system/cpu/cpu3/core_ctl/active_cpus "
                   "/sys/devices/system/cpu/cpu7/core_ctl/active_cpus").split()
    level = re.search(r"level: (\d+)", shell("dumpsys battery"))
    return ((m.group(1) if m else "?") + " active_mid/big=" + "/".join(active) + " battery=" +
            (level.group(1) if level else "?"))


window = float(sys.argv[1])
label = sys.argv[2]
prefix = sys.argv[3]
MIN_BATTERY = int(os.environ.get("MIN_BATTERY", "6"))


def battery_level():
    level = re.search(r"level: (\d+)", shell("dumpsys battery"))
    return int(level.group(1)) if level else 100


for n, value in enumerate(sys.argv[4:], 1):
    if battery_level() < MIN_BATTERY:
        print(f"battery below {MIN_BATTERY}%, stopping", flush=True)
        sys.exit(3)
    if prefix == "@aff":
        print(subprocess.run(["bash", os.path.join(HERE, "aff.sh"), value], capture_output=True,
                             text=True, env=ENV).stdout.strip(), flush=True)
    elif prefix == "-":
        for command in value.split(";"):
            print(bus(command).strip().splitlines()[-1], flush=True)
        value = value.split(";")[0].split()[-1]
    else:
        print(bus(f"{prefix} {value}").strip().splitlines()[-1], flush=True)
    time.sleep(SETTLE)
    c0 = counters()
    tag = f"{label}-{n}-{value}"
    subprocess.run(["bash", os.path.join(HERE, "athreads.sh"), str(int(window)), tag],
                   capture_output=True, text=True, env=ENV)
    out = subprocess.run(["python", "athreads_parse.py", f"athreads-{tag}.raw"], cwd=HERE,
                         capture_output=True, text=True, env=ENV).stdout
    c1 = counters()
    flips = max(c1["flip"] - c0["flip"], 1)
    per = {k: (c1[k] - c0[k]) / flips for k in c0 if k != "flip"}
    lines = out.strip().splitlines()
    head = lines[0] if lines else "?"
    fps = re.search(r"fps=([\d.]+)", head)
    cores = re.search(r"process CPU ([\d.]+)", out)
    threads = {}
    for ln in lines[2:]:
        m = re.match(r"\s*([\d.]+)%\s+([\d.]+) ms/frame\s+cpu\S*\s+(.+?) \(\d+\)$", ln)
        if m:
            threads.setdefault(m.group(3), float(m.group(2)))
    def t(name):
        if name.endswith("-"):
            return sum(v for k, v in threads.items() if k.startswith(name))
        return threads.get(name, 0.0)
    print(f"{tag}: thermal={thermal()} fps={fps.group(1) if fps else '?'} "
          f"cores={cores.group(1) if cores else '?'} Guest-1={t('Guest-1'):.2f} "
          f"GpuComm={t('shadPS4:GpuComm'):.2f} VkRecord={t('shadPS4:VkRecor'):.2f} Guest-17={t('Guest-17'):.2f} workers21-25={sum(t(f'Guest-{i}') for i in range(21, 26)):.2f} "
          f"guest_all={t('Guest-'):.2f} Waker={t('shadPS4:Waker'):.2f} "
          f"cpu_per_frame={(float(cores.group(1)) * 1000.0 / float(fps.group(1))) if cores and fps and float(fps.group(1)) > 0 else 0:.1f} ms/frame | per frame: faults={per['release_calls']:.1f} "
          f"released={per['release_pages']:.1f} predicted={per['predicted_pages']:.1f} "
          f"watch_calls={per['watch_calls']:.1f} watch_pages={per['watch_pages']:.1f} "
          f"mprotect={per['syscalls']:.1f} hashed={per['hashed_pages']:.1f} "
          f"rewritten={per['rewritten_pages']:.1f} fault_kinds cold(known)/after_dirty/run_start="
          f"{per['cold']:.1f}({per['cold_known']:.1f})/{per['after_dirty']:.1f}/{per['run_start']:.1f} "
          f"(window {per['run_start_window']:.1f}) unchanged={per['unchanged_pages']:.1f} "
          f"kept={per['kept_pages']:.1f} cross={per['cross_released']:.1f} textures/skipped={per['texture_invalidates']:.1f}/"
          f"{per['texture_skipped']:.1f} texture_us={per['texture_ns'] / 1000:.1f}", flush=True)
