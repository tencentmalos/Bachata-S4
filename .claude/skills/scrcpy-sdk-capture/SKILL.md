---
name: scrcpy-sdk-capture
description: Take screenshots and recordings of a running shadPS4 game on Swan/Android without lens distortion — lossless PNG and H.264 of the game render target via the embedded scrcpy capture SDK (mono canvas, or one/both eyes of a PSVR side-by-side canvas), or the headset's composited XR view (cinema room, game screen, PSV layer). Use instead of adb screencap / scrcpy panel crops, which are distorted on Swan.
---

# Screenshots and recordings of a running game (scrcpy capture SDK)

## Pick the source

| Need | Source | MCP tool (`scrcpy.*`) | CLI |
|---|---|---|---|
| Game image, lossless still | embedded SDK `SnapshotSession` | `capture_app_screenshot(serial, eye)` | `capturectl.py screenshot OUT [--eye]` |
| Game image, video | embedded SDK `EncoderSession` (H.264) | `record_app_video(serial, durationSeconds, eye)` | `capturectl.py record OUT --seconds N [--eye]` |
| What the wearer sees (room + game quad + PSV/status), still | Pico OS system composite | `capture_headset_view(serial)` | `tools/xr/swan_xr_screenshot.py` |
| Same, video | Pico OS system recording | `record_headset_view(serial, durationSeconds)` | — |
| Panel debugging (eye crops, flicker, timing) | scrcpy panel mirror | `capture_eye_screenshot` / `start_recording` | — **lens-distorted on Swan** |

The SDK path is shadPS4's own final render target (`view: canvas`): no host
overlays and none of the OpenXR compositor layers (cinema room, PSV). Use the
headset tools for the XR scene. The headset tools always give one mono view.

## Mono / stereo (单目 / 双目)

The game canvas has one of two layouts, reported as `layout:` by every SDK
reply and in the MCP result:

| Game type | layout | canvas (Swan) | `eye` |
|---|---|---|---|
| ordinary game (2D, or XR cinema) | `mono` | e.g. 2592×1458 | ignored; full canvas, MCP adds a `note` |
| PSVR title (e.g. Beat Saber) | `stereo_sbs` | 5184×2400, left eye then right eye | `both` = full canvas, `left`/`right` = that half (2592×2400) |

Recordings follow the same rule; a left/right recording encodes only that eye.
If the layout changes while recording (2D intro → PSVR), the encoder restarts
on the new canvas; check `layout`/`width` in the final status. Swan's encoder
accepts the full 5184×2400 stereo canvas.

## Requirements

- Debuggable APK built with the SDK: the host build passes
  `--scrcpy-capture-sdk-root C:/workspace/my_mcp_tools` (`build/run-host-build-scrcpy.cmd`).
  The SDK must contain `capture-sdk/native/include/scrcpy_capture/snapshot_session.h`;
  without it CMake warns "embedded screenshots disabled" and `capture_screenshot`
  is missing. Check a built host: `grep -a -c "capture_screenshot request" build/android-host-api33/native/libshadps4_host.so`.
- A running session that presents frames (`dumpsys activity service
  com.shadps4.android/.service.FexSessionService` shows `stage: Running`, `host_present` rising).
- Code lives in `C:/workspace/my_mcp_tools/dev_tools/mcp/scrcpy`:
  `capture-sdk/native` (C++ SDK), `capture-sdk/tools/capturectl.py`,
  `mcp/scrcpy_mcp/undistorted.py` (MCP tools). shadPS4 side:
  `src/video_core/renderer_vulkan/capture_recorder.cpp` (commands, encoder) and
  `Presenter::RecordEmbeddedScreenshot` (PNG readback/crop).

## Screenshot

```sh
python C:/workspace/my_mcp_tools/dev_tools/mcp/scrcpy/capture-sdk/tools/capturectl.py \
  --serial PB3110PGL6240001G screenshot <scratch>/shot.png [--eye left|right|both]
```

Underneath: `capture_screenshot request TOKEN [EYE]` (TOKEN = 32 lowercase
hex), poll `status TOKEN` through `pending → capturing → ready | failed |
cancelled`, pull the PNG with `run-as com.shadps4.android cat <file>`, delete
the device copy. The reply carries `layout`, `eye`, PNG size and
`producer_frame`. HDR or non-8-bit swapchains fail with an explicit error.

## Recording

```sh
python .../capturectl.py --serial PB3110PGL6240001G record <scratch>/clip.h264 --seconds 10 [--eye left]
```

Underneath: `capture_video start [EYE]` … `stop`. Writes `clip.h264`
(Annex-B), `clip.h264.frames.csv` (codec PTS per sample) and
`clip.h264.status.json` (layout, eye, encoded size, samples, skipped). `--mp4`
remuxes with ffmpeg at a nominal 60 fps (needs ffmpeg on PATH; this Windows
host has none). `capture_video start_live TOKEN [EYE]` streams the same
packets over `localabstract:scapture.TOKEN` (see the SDK README). The encoder
may skip frames under back-pressure; it is not a fixed-rate capture. Only one
recorder runs at a time (`record_app_video` refuses a busy one).

## Headset view (XR composite)

`scrcpy.capture_headset_view` / `record_headset_view` (or
`tools/xr/swan_xr_screenshot.py` for stills) drive `com.picoxr.systemui`
`SCREEN_CAPTURE` (type 0 still, type 1 toggles recording), find the new file
in `/sdcard/DCIM/Screenshots` or `/sdcard/DCIM/ScreenRecording` by listing
difference (device clock can be wrong), pull it and delete only that file.
2560×1440 at the current head pose, no lens distortion, mono.

- Asleep headset: systemui ignores the request (`logcat -s ScreenCapture`:
  `powerState=SLEEP`); the tools send `KEYCODE_WAKEUP` first.
- Not worn / tracking lost: the runtime pauses the session; the result is black
  with a loading ring or a "前摄像头被遮挡" notice. The MCP still returns the
  image plus a `warning` (darkFraction > 0.97). This is not a rendering bug:
  ask the user to wear or aim the headset. The game keeps running meanwhile —
  use the SDK screenshot to check game state.

## Hygiene

- Save to the session scratchpad; publisher game art in captures is never committed.
- Tools remove their own device files; do not delete other files in the
  Pico capture folders or the app's captures directory.
- Drive the game into a scene with the DebugBus pad (`docs/debugbus-pad.md`);
  stop a session with `am startservice -n com.shadps4.android/.service.FexSessionService
  -a com.shadps4.android.action.STOP_EMULATION`.
