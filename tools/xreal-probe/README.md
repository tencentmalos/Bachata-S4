# XREAL Android pose probe

Standalone diagnostic app (`org.shadps4.xrealprobe`) for the experimental non-Unity route.
It does not change shadPS4 or feed its camera. SDK binaries are supplied locally, fingerprinted,
and kept in ignored build directories. No SDK binary patch, signal guard, frame submission,
camera image capture, or firmware update is performed.

Reference: [Clancey/xreal-sdk at 7cd64886](https://github.com/Clancey/xreal-sdk/tree/7cd64886d4099228e2d1101b7621ab37e92c5da3).
The minimal Unity interface adapter and companion Activity are adapted under the included
[MIT license](../../foundation/modules/xreal/LICENSE-community-MIT). The downloaded official XREAL SDK is separate proprietary
software, not covered by that license. Its binaries must not be committed here.

## Build and install

From repository root, extract the official SDK 3.1.0 archive into an ignored directory, then:

```sh
python3 foundation/modules/xreal/tools/prepare-sdk.py build/research/xreal/sdk/package tools/xreal-probe/build/sdk
export ANDROID_HOME="$HOME/Library/Android/sdk"
# Use JDK 17; project reuses the main Android app's Gradle wrapper/cache.
./android/shadps4-app/gradlew -p tools/xreal-probe assembleDebug --offline
adb -s DEVICE install -r tools/xreal-probe/build/outputs/apk/debug/xreal-pose-probe-debug.apk
adb -s DEVICE shell am start -n org.shadps4.xrealprobe/.ProbeActivity
```

Install the official Control Glasses 3.1 runtime on ordinary Android hosts. Its service is
discovered through `ai.nreal.nrsdk.network.type.server`. Runtime version and service resolution
are recorded at startup. The probe asks for CAMERA permission for the SDK tracking path;
it never requests image frames or audio. Thor is not an officially tested SDK host.

## Detection and pose interpretation

- USB vendor `0x3318` (from official device filters), XREAL/NREAL product/manufacturer names,
  or an explicitly named XREAL/NREAL secondary display identify **candidates** only.
  Other displays, including Thor's `Screen-2`, do not count. Unknown hardware can be probed
  using a Start button; the full USB and display inventory is always logged.
- SDK `GetDeviceType == 11/12` confirms **One Pro M/L**. Display resolution is not model proof.
- Default request: 6DoF (`tracking_type=0`), input source None. 3DoF is `1`.
  Requested mode, reported mode, capability, and measured motion are different evidence.
- Uses only XR-plugin `GetHeadPoseAtTime(uint64, float[16])`, preserving the raw 64-byte
  matrix. The session-manager's similarly named compact API has a different layout and
  reportedly horizon-stabilizes roll. Never call both paths in one query.
- `pose_usable` requires a started session, identified model, state Tracking (2), reason None
  (0), success return (1), a finite near-orthonormal proper rotation matrix, and an advancing
  HMD query clock. Native return conventions were checked in the fingerprinted SDK binary.
- The Foundation version also waits for the first non-identity pose before `pose_usable`;
  it records `sdk_pose_valid`, `startup_identity`, `translation_step` and `pose_rh` separately.
- HMD time is **query time, not sensor sample time**. Repeated matrices at rest are legitimate.
  A changing query clock cannot prove hardware sample freshness; this limitation is logged.
- RGB camera plug callback (action 2025, 1=plugin, 2=plugout) and RGB/position capability are
  logged independently. A capability flag alone does not prove an Eye is currently connected.
- Pose queries are scheduled 16 ms after the previous query on Foundation’s dedicated `XrealPose`
  worker. The UI updates at 1 Hz and never calls the SDK. Background/Pause stops the worker’s
  periodic work; Resume reuses the session. This is a query rate, not a measured sensor rate.

Bootstrap calls Java `System.loadLibrary` (real JNI_OnLoad), checks every required symbol,
provides minimal Unity display/input registries, initializes settings, creates the session,
starts the registered display/input lifecycle, calls XREALLoadAPI, then resumes. Queries are
never made before this sequence. Provider results and every phase's entry are flushed to disk.
SDK startup and independent rotation/translation were verified on Thor + One Pro M + Eye.
See [the validation record](../../docs/validation/android-native-host/xreal-pose-probe-thor-20261011.md).
The reusable implementation lives in [Foundation](../../foundation/modules/xreal/README.md).

## Logging and collection

Each launch has `files/runs/<date-time-pid>/` in the app's private storage:

| File | Evidence |
| --- | --- |
| `sdk/<run>/sdk-manifest.json` | Source version and SHA-256 of all 12 packaged SDK artifacts |
| `sdk/<run>/events.jsonl` | Hardware inventory, library loading, permission requests, tracking transitions, query count and lifecycle |
| `sdk/<run>/native.jsonl` | Symbol checks, Unity provider callbacks, initialization stages, SDK errors, raw glasses/Eye/wear events |
| `poses.jsonl` | Each query's raw matrix, SDK clock, host query time, return codes, mode/state/reason, capabilities and validity gates |
| `sdk/<run>/poses.jsonl` | Raw queries, converted poses, duration and publication validity |
| `sdk-logcat.txt` | This process's logcat, including SDK JNI/native output; survives USB removal |

JSON lines include wall time plus elapsed/monotonic time as labelled (Android elapsed time
includes suspend, MONOTONIC does not). Never subtract timestamps from different clock domains.
Native logs flush before SDK calls. Logs rotate: 16 MiB × 4 for poses, 8 MiB × 4 for events/logcat,
8 MiB × 3 for native; earlier runs are retained. The current run can be exported with **Export logs**
to Android Downloads as ZIP. Live collection can contain a partial last line; the summarizer
counts it rather than failing. The app captures only its UID's logcat; host collection also
captures a bounded system log tail, Control Glasses services and process exit information.

```sh
python3 tools/xreal-probe/collect.py --serial DEVICE --out build/validation/xreal-UNIQUE-RUN
python3 tools/xreal-probe/summarize.py build/validation/xreal-UNIQUE-RUN/probe-runs.tar
```

Collection is read-only, has per-command timeouts, and writes hashes and failures to a manifest.
Evidence may contain local network addresses and device identifiers; keep raw bundles in local
ignored build directories. `summarize.py` reads tar/ZIP members without extracting paths.

## Single USB-C port and session lifecycle

Before unplugging USB, enable and verify wireless adb on the same device:

```sh
adb -s USB_SERIAL tcpip 5555
adb connect DEVICE_WIFI_IP:5555
adb -s DEVICE_WIFI_IP:5555 shell getprop ro.serialno
```

If the existing macOS adb daemon cannot reach LAN while other processes can, a localhost-only
relay avoids restarting the shared adb server and disrupting other tests:

```sh
python3 tools/xreal-probe/adb-relay.py DEVICE_WIFI_IP
adb connect 127.0.0.1:15555
```

Keep that relay running during testing; collect using serial `127.0.0.1:15555`. After testing,
disconnect the adb transport and stop the relay; `adb -s DEVICE usb` restores USB-only adb.

Unplug/SDK-disconnect invalidates cached pose. For a fresh session or tracking mode, press
**Stop**, then open the app again (or `adb shell am force-stop org.shadps4.xrealprobe` and relaunch).
Stop kills only this standalone probe process and closes its logcat subprocess. It deliberately
does not attempt the community-reported unsafe asynchronous SDK DestroySession teardown or
patch that binary. Reconnection in the same native session is not claimed to work yet.

With glasses connected, validate in order: USB identity → SDK One Pro → Eye event/capability →
Tracking state → stillness → yaw/pitch/**roll** separately → translation without rotation →
cover Eye/loss/recovery → unplug → fresh process reconnect. No 6DoF claim until translation
changes correctly under controlled physical movement, not merely because mode 0 is selected.
