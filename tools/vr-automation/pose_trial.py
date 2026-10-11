#!/usr/bin/env python3
"""Bounded, recorded DebugBus VR input trial. Delivery checks are not scene recognition."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import uuid


def validate(plan):
    if not isinstance(plan, list) or not 1 <= len(plan) <= 128:
        raise ValueError("plan must contain 1..128 steps")
    total = 0
    for step in plan:
        if not isinstance(step, dict):
            raise ValueError("each step must be an object")
        if set(step) - {"seconds", "reason", "head", "ds4", "buttons", "hold_ms", "axes", "touch"}:
            raise ValueError("unknown step field")
        seconds = step.get("seconds", 1)
        if type(seconds) not in (int, float) or not math.isfinite(seconds) or not 0.1 <= seconds <= 120:
            raise ValueError("step duration outside 0.1..120 seconds")
        total += seconds
        if not isinstance(step.get("reason"), str) or not step["reason"].strip():
            raise ValueError("each intervention needs a reason")
        for target in ("head", "ds4"):
            if target not in step:
                continue
            p = step[target]
            if not isinstance(p, list) or len(p) != 7 or not all(type(x) in (int, float) and math.isfinite(x) for x in p):
                raise ValueError("pose must be finite xyz + xyzw")
            if any(abs(x) > 10 for x in p[:3]) or abs(sum(x*x for x in p[3:])-1) > .01:
                raise ValueError("invalid position or non-unit quaternion")
        if type(step.get("buttons", 0)) is not int or step.get("buttons", 0) & ~0x11ffff:
            raise ValueError("invalid button mask")
        if type(step.get("hold_ms", 300)) is not int or not 1 <= step.get("hold_ms", 300) <= 2000:
            raise ValueError("button hold outside 1..2000 ms")
        axes = step.get("axes", [0]*6)
        if not isinstance(axes, list) or len(axes) != 6 or any(
                type(v) not in (int, float) or not math.isfinite(v) or
                not (-1 if i < 4 else 0) <= v <= 1 for i, v in enumerate(axes)):
            raise ValueError("axes require four sticks in -1..1 and two triggers in 0..1")
        touch = step.get("touch", [0, 0, 0])
        if not isinstance(touch, list) or len(touch) != 3 or type(touch[0]) is not int or \
                touch[0] not in (0, 1) or any(type(v) not in (int, float) or
                not math.isfinite(v) or not 0 <= v <= 1 for v in touch[1:]):
            raise ValueError("touch requires down(0|1) and normalized x,y in 0..1")
    if total > 300:
        raise ValueError("trial exceeds 300 seconds")


class Trial:
    def __init__(self, args):
        self.args = args
        self.adb = ["adb", "-s", args.serial]
        self.out = Path(args.out)
        self.out.mkdir(parents=True, exist_ok=False)
        self.events = (self.out / "actions.jsonl").open("w", buffering=1)
        self.owner = "vrtrial_" + uuid.uuid4().hex[:12]
        self.action = 0
        self.identity = None
        self.poses = {}

    def bus(self, words):
        begin = time.monotonic_ns()
        raw = subprocess.run(self.adb + ["shell", "dumpsys", "activity",
            self.args.package + "/.MainActivity", "debugbus", "pad", *map(str, words)],
            capture_output=True, text=True, timeout=8, check=True).stdout
        lines = [x for x in raw.splitlines() if x.startswith("{")]
        if len(lines) != 1:
            raise RuntimeError("missing/ambiguous DebugBus JSON: " + raw[:400])
        reply = json.loads(lines[0])
        self.events.write(json.dumps({"host_start_ns": begin, "host_end_ns": time.monotonic_ns(),
            "command": words, "response": reply}) + "\n")
        if reply.get("status") not in ("ok", "dispatched"):
            raise RuntimeError("DebugBus rejected input: " + reply.get("status", "unknown"))
        return reply

    def send(self, command, *args):
        self.action += 1
        return self.bus([command, *self.identity, self.owner, self.action, *args])

    def observe(self):
        status = self.bus(["status"])
        identity = [status[k] for k in ("pid", "generation", "run_uuid")]
        if identity != self.identity or status["phase"] != 3 or not status["focused"] or status["ui_captured"]:
            raise RuntimeError("session/focus changed")
        return status

    def screenshot(self, name):
        data = subprocess.run(self.adb + ["exec-out", "screencap", "-p"],
            capture_output=True, timeout=8, check=True).stdout
        if not data.startswith(b"\x89PNG\r\n\x1a\n"):
            raise RuntimeError("invalid screenshot")
        (self.out / name).write_bytes(data)
        self.events.write(json.dumps({"screenshot":name, "sha256":hashlib.sha256(data).hexdigest(),
            "host_received_ns":time.monotonic_ns()}) + "\n")

    def run(self, plan):
        initial = self.bus(["capabilities"])
        if initial.get("pose_targets") != ["head", "ds4"]:
            raise RuntimeError("APK lacks VR pose API")
        self.identity = [initial[k] for k in ("pid", "generation", "run_uuid")]
        if self.args.pid and self.identity[0] != self.args.pid:
            raise RuntimeError("unexpected PID")
        raw = subprocess.run(self.adb + ["shell", "dumpsys", "activity",
            self.args.package + "/.MainActivity", "debugbus", "profiler_ring", "status"],
            capture_output=True, text=True, timeout=8, check=True).stdout
        (self.out / "profiler-identity.txt").write_text(raw)
        title = re.search(r"\btitle=(\S+)", raw)
        version = re.search(r"\bapp_ver=(\S+)", raw)
        if not title or title[1] != self.args.game_id:
            raise RuntimeError("unexpected or missing game ID")
        if self.args.game_version and (not version or version[1] != self.args.game_version):
            raise RuntimeError("unexpected or missing game version")
        (self.out / "identity.json").write_text(json.dumps({"serial":self.args.serial,
            "package":self.args.package,"identity":self.identity,"owner":self.owner,
            "mode":"recorded_input_trial","scene_acceptance":"requires separate observation",
            "game_id":title[1], "game_version":version[1] if version else None,
            "clock_alignment":"host receive bounds only"}, indent=2))
        self.observe()
        try:
            for i, step in enumerate(plan):
                self.events.write(json.dumps({"step":i,"reason":step["reason"]})+"\n")
                self.screenshot(f"{i:03}-before.png")
                self.poses.update({k:step[k] for k in ("head","ds4") if k in step})
                until = time.monotonic() + step.get("seconds",1)
                first = True
                while first or time.monotonic() < until:
                    self.observe()
                    receipts = []
                    for target, pose in self.poses.items():
                        receipts.append(self.send("pose",target,10000,*pose))
                    if first and any(k in step for k in ("buttons", "axes", "touch")):
                        receipts.append(self.send("state",0,step.get("hold_ms",300),step.get("buttons",0),
                            *step.get("axes", [0]*6), *step.get("touch", [0,0,0])))
                    time.sleep(min(2, max(.1,until-time.monotonic())))
                    for r in receipts:
                        receipt = self.bus(["status",*self.identity,self.owner,r["action_id"]])
                        if not receipt["guest_poll_seen"]:
                            raise RuntimeError("game did not consume action " + str(r["action_id"]))
                    first = False
                self.screenshot(f"{i:03}-after.png")
                status = self.observe()
                print(json.dumps({"step":i,"tracking":status.get("tracking")}), flush=True)
        finally:
            unwinding = sys.exc_info()[0] is not None
            try:
                self.send("release_all")
            except Exception as error:
                # Keep the original failure visible when the app/session died.
                # Native pose leases still expire without the runner.
                self.events.write(json.dumps({"release_failed":str(error)})+"\n")
                if not unwinding:
                    raise
            finally:
                self.events.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--package", default="com.shadps4.android")
    parser.add_argument("--pid", type=int)
    parser.add_argument("--game-id", required=True)
    parser.add_argument("--game-version")
    parser.add_argument("--plan", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    plan = json.loads(Path(args.plan).read_text())
    validate(plan)
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    trial = Trial(args)
    (trial.out / "plan.json").write_text(json.dumps(plan, indent=2))
    try:
        trial.run(plan)
    finally:
        trial.events.close()


if __name__ == "__main__":
    main()
