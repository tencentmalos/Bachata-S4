# Recorded VR input trials

`pose_trial.py` drives the debug APK's existing `pad` bus with leased head/DS4
poses and ordinary buttons, sticks or touch. It verifies the game identity and
actual guest consumption of every action. This is a bounded input trial, **not
scene recognition**: inspect the before/after screenshots and a separately
recorded video before calling a transition successful.

```sh
python3 tools/vr-automation/pose_trial.py --serial DEVICE --pid APP_PID \
  --game-id CUSA12392 --game-version 01.04 \
  --plan tools/vr-automation/plans/astro-left-planet.json --out NEW_OUTPUT_DIR
```

Start a bounded scrcpy recording before the trial; keep its identity alongside
`identity.json`, `plan.json`, screenshots and `actions.jsonl`. Host timestamps
bound receipt times only; they are not calibrated game/video clock alignment.
The directory must not already exist. Stop on changed session, focus, host
modal capture, rejected actions or absent guest delivery. The runner releases
its owner on exit; native leases expire within 10 seconds even if it dies.

Plans contain 1–128 steps and at most 300 seconds total. Each step requires a
`reason`; `seconds` is 0.1–120. `head`/`ds4` are seven numbers: camera-space
position in metres followed by unit quaternion **x,y,z,w**. +X is right, +Y up;
the virtual player is at z=+1.5 m and faces the camera (-Z). Poses persist across
steps and are renewed during the step. `buttons` is the PS4 mask (Cross=16384),
`hold_ms` is 1–2000, `axes` is four sticks (-1..1) then two triggers (0..1), and
`touch` is `[down, x, y]` with normalized coordinates. Input is issued once at
the start of a step; the remaining time observes its result.

ASTRO EU 01.04 recipes, checked on Thor/SBS:

- `astro-calibration.json`: start at the title awaiting Cross. Supplies the seated
  head and centered DS4 needed by the body/controller alignment page. It does
  not detect when the opening cinematic finishes.
- `astro-left-planet.json`: start after the cinematic when the robot directs the
  player to the left planet. Look at yaw 79°, pitch +14°, then press Cross.
  This reached the first level; gaze alone did not enter it.
- `astro-scene-hold.json`: start inside the level; hold a neutral head/controller
  for 30 seconds to collect a stable view. No gameplay completion claim.

The pose API is `pad pose PID GEN UUID OWNER ACTION TARGET HOLD_MS X Y Z QX QY QZ QW`.
`pad capabilities` describes ranges; `pad status` reports active leases and the
latest checked guest HMD/DS4 outputs. Receipts acknowledge `VrTrackerGetResult`
after its output is written to guest memory. An acknowledgement proves delivery,
not game acceptance. Only the first registered player DS4 is targeted. Host
OpenXR poses and persistent settings are unaffected. Pad trace v1 does not
contain pose samples: preserve the runner's action log.
