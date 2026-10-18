"""Preemption A/B for the GoW3 stream-submit GPU hang on the Odin3.

  gow3_ab.py prep A|B|C       force-stop shadPS4, set debug.mesa.tu.debug for the arm, launch GoW3
  gow3_ab.py run A|B|C MIN TAG check the driver flags, enable stream_submit 200, soak MIN minutes,
                             then collect fault evidence into OUT/ab-TAG.*
  gow3_ab.py restore         clear debug.mesa.tu.debug (stream_submit is per process, default 0)

Arm A: TU_DEBUG from the property is empty (the host still appends gmem_warmup).
Arm B: TU_DEBUG=cmd_no_preempt (CP_SCOPE_CNTL disable_preemption around every primary cmdbuf).
Arm C: TU_DEBUG=cs_recycle (retired command-stream BOs are kept and reused, mesa!44888).
"""
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GOW3 = os.path.join(HERE, "gow3.py")
SOAK = os.path.join(HERE, "gow3_soak.py")
ROOTSH = os.path.join(HERE, "rootsh-odin3.sh")
SERIAL = os.environ.get("ANDROID_SERIAL", "6edb0593")
PKG = "com.shadps4.android"
OUT = os.environ.get("GOW3_OUT", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "build", "validation", "a8xx-dde"))
ENV = dict(os.environ, MSYS_NO_PATHCONV="1", GOW3_OUT=OUT)
ARMS = {"A": "", "B": "cmd_no_preempt", "C": "cs_recycle", "D": "cs_recycle,cs_dsb", "E": "cs_recycle,syncdraw", "F": "cs_recycle,cs_skip_head", "G": "cs_recycle,dde_touch", "H": "cs_recycle,dde_check"}
# Driver log line that proves each arm's flag took effect.
NOTES = {"B": "cmd_no_preempt wraps primary command buffers",
         "C": "cs_recycle keeps retired command-stream BOs",
         "D": "cs_dsb issues DSB SY"}
DRIVER_SHA = os.environ.get("EXPECT_DRIVER", "c71158723b758eacef5c9ace3a7091a8aa8cd193a562da50074cdd9728d64046")  # default: Turnip ca038cc10f1


def adb(*args, binary=False):
    proc = subprocess.run(["adb", "-s", SERIAL, *args], capture_output=True, env=ENV)
    return proc.stdout if binary else proc.stdout.decode(errors="replace").strip()


def gow3(*args):
    return subprocess.run([sys.executable, "-I", GOW3, *args], capture_output=True,
                          env=ENV).stdout.decode(errors="replace").strip()


def root(script_text, name):
    path = os.path.join(OUT, name)
    with open(path, "w", newline="\n") as f:
        f.write(script_text)
    return subprocess.run(["bash", ROOTSH, path.replace("\\", "/")], capture_output=True,
                          env=ENV).stdout.decode(errors="replace")


def foreground():
    return adb("shell", "dumpsys activity activities | grep -m1 topResumedActivity")


def faults():
    return adb("shell", "cat /sys/class/kgsl/kgsl-3d0/snapshot/faultcount")


def prep(arm):
    top = foreground()
    if "launcher" not in top and PKG not in top:
        raise SystemExit(f"device busy, foreground: {top}")
    adb("shell", "am", "force-stop", PKG)
    adb("shell", f"setprop debug.mesa.tu.debug '{ARMS[arm]}'")
    value = adb("shell", "getprop", "debug.mesa.tu.debug")
    if value != ARMS[arm]:
        raise SystemExit(f"property is [{value}], wanted [{ARMS[arm]}]")
    start = adb("shell", "date '+%m-%d %H:%M:%S.000'")
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, f"prep-{arm}.txt"), "w").write(start + "\n")
    print(f"arm {arm} property=[{value}] faults={faults()} logcat_since={start}")
    print(gow3("launch").splitlines()[-1:])


def run(arm, minutes, tag):
    since = open(os.path.join(OUT, f"prep-{arm}.txt")).read().strip()
    log = adb("logcat", "-d", "-T", since)
    # The host logs the merged TU_DEBUG to its own file (rotated per session), not to logcat.
    host_log = adb("shell", "run-as", PKG, "cat", "files/host/log/android-host.log")
    flags = re.findall(r"Turnip TU_DEBUG=(\S+)", host_log)
    notes = {a: note in log for a, note in NOTES.items()}
    info = json.loads(gow3("status"))
    want = {a: a == arm for a in NOTES}
    flag_ok = bool(flags) and all((ARMS[a] in flags[-1]) == want[a] for a in NOTES)
    if not flag_ok or notes != want:
        raise SystemExit(f"driver flags {flags} / notes {notes} do not match arm {arm}")
    preempt_note = notes["B"]
    profile = re.findall(r"Turnip profile: \S+ \(property='[^']*'\)", log)
    drivers = sorted(set(re.findall(r"native-drivers/([0-9a-f]{64})", log)))
    print("session", info, "TU_DEBUG", flags[-1], "preempt note", preempt_note)
    print("profile", profile[-1:], "driver sha", [d[:16] for d in drivers])
    if drivers and DRIVER_SHA not in drivers:
        raise SystemExit(f"loaded driver {drivers} is not {DRIVER_SHA[:16]}")
    before = gow3("bus", "upload_diag", "stream_submit", "200")
    time.sleep(10)
    after = gow3("bus", "upload_diag", "stream_submit", "200")
    print("stream_submit", before.strip(), "->", after.strip())
    fault0 = faults()
    t0 = time.monotonic()
    soak = subprocess.run([sys.executable, "-I", SOAK, str(minutes)], capture_output=True,
                          env=ENV).stdout.decode(errors="replace")
    elapsed = time.monotonic() - t0
    fault1 = faults()
    pid = adb("shell", "pidof", PKG)
    result = {
        "arm": arm, "tu_debug": flags[-1], "session": info, "minutes": minutes,
        "elapsed_s": round(elapsed), "faults_before": fault0, "faults_after": fault1,
        "pid_after": pid, "stream_submit": after.strip(), "soak": soak.splitlines(),
    }
    tail = adb("logcat", "-d", "-T", since)
    hits = [l for l in tail.splitlines()
            if re.search(r"Device lost|DeviceLost|kgsl|GPU fault|GuestFault|FATAL|abort", l)]
    result["logcat_hits"] = hits[-40:]
    result["cs_cache"] = [l for l in tail.splitlines() if "kgsl cs cache" in l][-6:]
    kernel = root("dmesg | grep -iE 'kgsl|adreno|gmu|preempt|snapshot' | tail -80\n"
                  "cat /sys/class/kgsl/kgsl-3d0/snapshot/faultcount\n"
                  "cat /sys/class/kgsl/kgsl-3d0/snapshot/timestamp 2>/dev/null\n",
                  f"ab-{tag}-dmesg.sh")
    result["dmesg"] = kernel.splitlines()
    if fault1 != fault0:
        root("cat /sys/class/kgsl/kgsl-3d0/snapshot/dump > /data/local/tmp/ab-snapshot.bin\n"
             "chmod 644 /data/local/tmp/ab-snapshot.bin\nls -la /data/local/tmp/ab-snapshot.bin\n",
             f"ab-{tag}-snap.sh")
        snap = os.path.join(OUT, f"ab-{tag}-snapshot.bin")
        subprocess.run(["adb", "-s", SERIAL, "pull", "/data/local/tmp/ab-snapshot.bin", snap],
                       env=ENV, capture_output=True)
        adb("shell", "rm", "/data/local/tmp/ab-snapshot.bin")
        result["snapshot"] = snap if os.path.exists(snap) else None
    with open(os.path.join(OUT, f"ab-{tag}.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=1)
    print(json.dumps({k: result[k] for k in ("arm", "elapsed_s", "faults_before", "faults_after",
                                              "pid_after")}))
    print("\n".join(result["soak"][-4:]))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "prep":
        prep(sys.argv[2])
    elif cmd == "run":
        run(sys.argv[2], float(sys.argv[3]), sys.argv[4])
    elif cmd == "restore":
        adb("shell", "setprop debug.mesa.tu.debug ''")
        print("debug.mesa.tu.debug=[" + adb("shell", "getprop", "debug.mesa.tu.debug") + "]")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
