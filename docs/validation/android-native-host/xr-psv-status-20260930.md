# PSV status scene / model correction and indicator (2026-09-30)

Latest follow-up: [fixed spatial anchor and tighter projection](#fixed-spatial-anchor-and-tighter-projection).

Implemented locally on `feature/malos/swan_performance` (`b0778a7f` base). No commit or push.

## Model and reference correction

The first model was wrong: its axis-aligned 27 × 8 mm shoulder blocks protruded
past the rounded shell, making the rear/top silhouette look like a long pen.
The PS/SELECT/START keys were boxes; the PS label used two letters; an invented
blue front LED had been added. These were removed or rebuilt, not hidden by a
camera change.

The corrected PCH-1000-shaped mesh has separate curved transparent shoulder caps
(`KHR_materials_transmission`), a rounded shell edge, oval lower buttons, a
PlayStation symbol on the PS key, tapered D-pad arms, and corrected rear panel /
grip / pattern placement. The model remains an independent approximation based
on photographs, not official CAD or a scan. Sony's published 182 × 83.5 × 18.6 mm
envelope is used; controls may protrude in depth.

- [Saved photo references and sources](evidence/xr-psv-status-20260930/references/README.md).
- [Front preview](evidence/xr-psv-status-20260930/model/psv-front.png),
  [back](evidence/xr-psv-status-20260930/model/psv-back.png),
  [top](evidence/xr-psv-status-20260930/model/psv-top.png).
- Rebuild: `tools/xr/build_psv_model.py`; generated asset:
  `android/shadps4-app/app/src/main/assets/xr/psv_status.glb`.
- Editable Blender model and build logs:
  `build/validation/xr-psv-status-20260930/` (local, not committed).
- The PlayStation SVG attribution is in `tools/xr/README.md`.

Blender inspection images are not evidence of the Lite Engine lighting pipeline.

## Scene, status data and lighting

`openxr/status_scene.cpp` creates a Foundation Lite Engine scene using
`XrSceneVulkanLayer`, as in Azahar. Both PSVR projection presentation and ordinary
cinema presentation append the same stereo 3D status scene. The model is anchored
in LOCAL space below/in front of the initial head position; recenter updates the
anchor. Camera matrices use runtime eye poses/FOV. The mapped screen is part of
the 3D model, not a second independently aligned quad.

The 960 × 544 unlit screen uses the existing StatusLayer / Foundation PerfHud
snapshot and cached DeviceMetricsSampler data. Screen updates and snapshot
publication are limited to 4 Hz. Only new game frames increment the FPS counter;
redraws may refresh status/battery without inflating FPS. Missing measurements
remain `--`; no separate Java/Kotlin sampler or JNI environment is introduced.
Assets use the existing JniHelper asset manager. The CPU text rasterizer does not
change the renderer's ImGui context.

A neutral white rectangular light is enabled with voxel GI. Model CPU shadow
geometry is explicitly retained for GI. Graphite, pearl and blue palettes modify
named material instances. Transparent shoulders and the scene projection use
premultiplied blending. The model and screen share the same render/occlusion
space.

The PCH-1000 indicator is the **left PS key**; the upper-right circle is a camera.
The key's emissive material and a small attached point light follow one state
function. Active sessions show steady blue; losing focus starts a brief blue
standby transition, then current charging status determines orange/off. Low
charge preventing startup and notification blinking are explicit preview modes
only: the emulator does not invent those events from FPS or a battery threshold.
See [official references and simulation timing](evidence/xr-psv-status-20260930/references/indicator-states.md).

DebugBus (OpenXrActivity):

```text
xr_status status
xr_status visible on|off
xr_status theme graphite|pearl|blue
xr_status gi on|off
xr_status recenter
xr_status indicator auto|off|running|standby|charging|charge_low|notification
```

Indicator overrides are diagnostic previews, not actual battery readings.
The default and final state are visible, graphite, GI on, indicator auto.

## Integration and validation

Foundation basic services are linked for Android Lite Engine, without enabling
the desktop DebugBus TCP listener. Existing profiler-ring/math/JNI ownership is
reused; host nlohmann-json and Foundation consumers share the same target/version.
The renderer reuses the existing Vulkan instance/device/queue and queue mutex.

Checks:

- Host and APK builds passed.
- Export contract checks passed: physical outline, separate transmissive shoulders,
  display dimensions/front normals/top-left UVs/unlit material, no invented LED.
  The same checker rejects the old packaged GLB at an actual shoulder vertex
  outside the rounded case; it is a negative control, not just a source assertion.
- C++ indicator tests (six states, blink phases and 60,000 time/state samples)
  passed with UBSan.
- Swan `PB3110PGL6240001G`: Beat Saber CUSA12878 reached the safety screen with
  the status model, live values, blue PS light; pearl + orange charging preview
  and GI on/off were captured. PID16030 / generation1 /
  run `b09a7e6f72bbb16369310ad57418227e` stopped through the service with
  `user_stop`, guest return 0.
- Bloodborne CUSA03023 cold launch showed ordinary cinema plus the same PSV
  status scene. PID23672 / generation1 / run `b3d4595e2a3052993211055de4d157b1`:
  status counter advanced to 3616; GI reported 52,230 triangles, 9,960 voxels,
  1,870 probes, zero skipped models, one geometry rebuild and zero failed shader
  variants. The screenshot shows the pre-existing improper-exit warning. No
  game input or save formatting was injected.
- These GI counters verify actual geometry/light processing. They do not establish
  photometric accuracy, a GPU performance improvement, or a fix for the earlier
  Turnip hangs. Probe invalid counts are recorded, not claimed to be zero.

Pico system-composited screenshots are stored under
`evidence/xr-psv-status-20260930/device/`. The tool names outputs `.png`, but these
captures contain JPEG data; the archived copies use `.jpg`. The earlier native
physical-display crop is rotated/distorted and is not used as the main preview.

One attempt to launch Bloodborne directly after service-stop remained in the
stopped XR Activity/loading spinner. A cold app launch worked. This was not
investigated as a lifecycle regression here; do not call the warm switch verified.
The final package/installed SHA and final device state are recorded in the
accompanying evidence directory. Earlier GPU diagnostics/cache cleanup entries
remain separate and unchanged. No global or per-game settings were edited.

## Final deployment

The final snapshot publication tweak also refreshes on repeated mirror frames,
without counting them as game frames. Rebuilt host and APK, installed and verified
whole-package SHA against the local file:

- APK `bd799c72236ed30b0aee7e2ce0ea29e26be2e606e0c6206ada197763c7ea9e89`.
- Host `f04aed84e715c241816fd0ec830c377872f49d75f5d01d07f619cea5d33a3018`.
- Model `00b68979ed078efab71a3b5eb27ecfdb6044a9c6d23ec7678286793adf64b464`.
- Turnip unchanged, loaded SHA `a95b15df82ed9e49e7d2166a5e33f959b06a6655ec626f735cd6a2c98159dec3`.
- Saved APK: `build/apk-release/shadps4-psv-status-bd799c72-playstoreDebug.apk`.

Final Beat Saber: PID16389 / generation1 / run
`cc58d1487f02edb1e8440dd2b9f5a00a`, Running with 947 guest flips and 951 status
frames at the recorded sample. The scene reports graphite, GI on, indicator auto
(steady blue), zero failed shader variants. The game is left running for viewing.
Bloodborne was stopped normally with guest return 0 before installation.

Five system screenshot files created by this round were removed from the device
only after their SHA matched a preserved local copy. Earlier screenshots and old
GPU diagnostic/cache cleanup state were not touched. No injected controller input,
debugger, forwards or scrcpy session was created.


## Fixed spatial anchor and tighter projection

The user asked for the model below the view, queried the low resolution, then
cancelled the requested 4× MSAA experiment. **The final status layer remains
single-sample**. The temporary Foundation external-resolve changes were fully
removed before deployment; this follow-up leaves no new Foundation MSAA code.

- Status swapchain remains **2592 × 2400 per eye**, SBS **5184 × 2400**.
  The screen raster stays **960 × 544**, with the same glyph bake and 4 Hz update.
  No game resolution, game MSAA, ETFR or upscaler setting was edited.
- Placement uses the midpoint and averaged forward direction of both runtime
  eyes (individual eyes are canted). It places the model at `(0, -0.42, -1.15)` m
  relative to the initial horizontal heading. **Capture the anchor once**, then
  keep it fixed in OpenXR LOCAL space. Turning, leaning or lowering the head only
  changes camera views. Explicit recenter / runtime reference-space change
  re-establishes the anchor. The initial follow-heading candidate was rejected by
  the user and removed.
- Keep the model, lights and GI geometry in a stationary panel coordinate frame;
  transform current runtime eye poses into that frame. Restore the same rendered
  frame's LOCAL-space camera poses when submitting to the compositor, including
  when an image is reused. Real IPD, eye cant and head roll are preserved.
- Project all eight corners of the mesh's actual bounds, add 6% tangent-space
  padding on each side, then narrow each eye's FOV symmetrically about its optical
  axis, capped by the original runtime FOV. Bounds crossing the near plane retain
  the original FOV and normal clipping; the panel does not disappear abruptly.
  The model's physical size and distance do not change when switching projections.

The first off-centre crop showed a visible position/perspective change in the
Pico composed image. Keeping original runtime eye poses did not remove it; sharing
one off-centre FOV between both eyes and including the optical axis did not remove
it either. **Centred symmetric cropping matches the full-FOV model placement**.
This is a measured compatibility choice for this path, not proof of a Turnip bug
or a fully isolated Pico runtime defect. OpenXR itself permits application FOVs
that differ from `xrLocateViews`:
[Khronos projection-view specification](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrCompositionLayerProjectionView.html).
The temporary `projection_test` modes are removed from the final code.

`xr_status projection full|cropped` remains as a reversible diagnostic; default
and final state are `cropped` (reported as `cropped-symmetric`). At the recorded
pose, runtime full FOV is 95° × 86° and cropped FOV is about 53° × 58°. Tangent-space
pixel-density ratios from the actual FOV are approximately **2.2× horizontal,
1.7× vertical, 3.7× pixel area**. These are render-target sampling ratios, not a
claim of 3.7× perceived clarity or FPS. The screen source, optics and compositor
still limit visible text detail. The crop varies with viewing position; it
approaches the full FOV when the model moves toward the edge of the view.

Validation:

- C++/UBSan: 3,600 corner containment / ray-equivalence checks across yaw, pitch,
  roll, canted eyes and 50/64/78 mm IPD, plus fixed-anchor movement, FOV cap,
  vertical heading fallback and invalid-bounds cases. A separate local matrix
  probe also checked the existing math matrix path against analytic projected rays.
- Final Android host and APK builds passed. Actual packaged host SHA matches the
  build output. Installed APK SHA was read back and matched.
- Final Swan Beat Saber safety screen: PID28487, cropped-symmetric, samples=1,
  fixed model world position `(-0.831, -0.726, -1.662)` at the recorded sample;
  GI rebuilt once, zero skipped models, zero failed shader variants. Full/cropped
  A/B screenshots show matching model placement. No controller input was injected.
- The normal cinema mode shares this StatusScene path; its earlier model test is
  recorded above, but this final crop package was not separately replayed in cinema.
- No long GPU hang regression, through-lens acuity measurement or performance
  benefit is claimed. Intermediate exits used service STOP and reported user_stop
  with guest return `18446744071562199125`, **not return 0**.

Final APK `9a13293a175415646292d533aa37b41a8899b30bab5b6b4ae3de11a7c808f399`,
host `287a73552f8f94ccf0b01ad6948519cb8d7ede146d67b8dd8c5da3039923ca75`.
Model and source-built Turnip SHA remain unchanged. Saved package:
`build/apk-release/shadps4-psv-projection-9a13293a-playstoreDebug.apk`.

[Evidence, intermediate negative cases and final A/B](evidence/xr-psv-status-20260930/projection/README.md).
