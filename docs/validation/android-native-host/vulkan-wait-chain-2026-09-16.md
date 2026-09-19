# Vulkan submission waits and current CPU/shader policy

2026-09-16. Diagnostic work only; no production source, FEX, driver, precision,
or persistent performance setting changes. Further device work paused at the
user's request pending a rooted device. Preserve all previous dirty work.

## Result

The current renderer submits GPU work asynchronously, but **CPU submission is
not isolated from driver waits**. The measured propagation is:

```text
KGSL completion event
  -> GpuDone completion worker resumes
  -> vkQueueSubmit on GpuComm or Presenter resumes
  -> shared VkQueue mutex becomes available to the other submitter
  -> GpuComm finishes processing/flushing
  -> GpuIdle software interrupt clears the GNM submission gate
  -> guest main thread resumes SubmitAndFlip
```

This is an observed wake chain, not yet a native stack proving the exact driver
mutex. In particular, `GpuIdle` here means Liverpool's software queue has drained;
the source does not call `vkDeviceWaitIdle` at this boundary. A driver-blocked
submission makes this software boundary depend indirectly on GPU progress.

One ordinary VkQueue requires external host synchronization; Vulkan's explicit
state model does not permit concurrent unprotected access to that same queue.
`vk_instance.cpp` requests one queue and retrieves family/queue index 0 for both
graphics and present. The static `Scheduler::submit_mutex` is broader than a
per-instance design needs, but this one-session run does not demonstrate
cross-instance contention. Removing the lock would be invalid. See the
[Vulkan host-synchronization requirement](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html).

## Actual device and scope

- AYN Thor `9c2841a4`, API 33 / 4 KiB, ordinary APK, PID 8463 / Session generation 2,
  process UUID `1c9aee872e67e28297d55a653dacd1b6`.
- APK `9c28a1b90e383835a38ef852bd09bc87c2729f5107e06e66ea091bc05f83d2b0`;
  host Build ID `8bf15985aef4038795e3555e0061f4fcca31e600`.
- Turnip Mesa 26.0.0-devel, reported commit `5ac41be677`, driver SHA-256
  `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`.
- Same TMNT rooftop executable/calibration package as the preceding
  [rooftop report](tmnt-rooftop-calibration-2026-09-16.md). No input during the
  profiling or overlay A/B windows.
- Warmup screenshots 12 and 16 show Leonardo moving from the roof center toward
  the right wall, with camera response after stick-right touches. The scripted
  acceptance **failed because the submitted review was stale**; its recorded
  `ERROR_UNVERIFIED` is retained. Do not relabel it `GAMEPLAY_REVIEWED`.
  Actual rooftop screenshots support scene attribution, not a new full-flow pass.

## Submission and scheduling evidence

The first 20 s PROF contains 267 returned frame/submit wrappers. Frame duration
averages 74.52 ms; SubmitFlipRotateBuffer averages 45.01 ms. There are 535 GNM
gate entries because both SubmitAndFlip and SubmitDone check it; their 22.26 ms
mean must not be mistaken for a once-per-frame wait. GpuComm's 446 submissions
average 16.17 ms waiting for the queue mutex and 17.63 ms inside submission.

A second 8 s PROF plus 5 s handoff capture overlaps a **2.019 s raw ftrace**
window. All clocks use Android CLOCK_MONOTONIC. Trace header reports
235616 retained / 235616 written events, without buffer overrun. Only complete
handoff scopes within that trace window are used below:

| Thread / scope | Calls | Elapsed total | Actually on CPU | Principal wake source |
|---|---:|---:|---:|---|
| GpuComm / Vulkan.SubmitLock | 45 | 674.53 ms | 0.38 ms | Presenter, 673.18 ms sleep |
| GpuComm / Vulkan.Submit | 45 | 803.55 ms | 2.51 ms | GpuDone 14014, 799.50 ms sleep |
| Presenter / Vulkan.SubmitLock | 218 | 784.77 ms | 0.63 ms | GpuComm, 783.17 ms sleep |
| Presenter / Vulkan.Submit | 218 | 832.63 ms | 9.28 ms | GpuDone 14014/14018, 821.26 ms sleep |
| Presenter / Present.DriverCall | 123 | 13.58 ms | 12.85 ms | Very little sleep |

GpuComm's queue-lock waits overlap the other thread's queue ownership for
673.67 / 674.53 ms. Presenter has the reverse dependency. Guest main thread
13992 accumulates 1079.94 ms sleeping until GpuComm wakes it. GpuDone 14014
accumulates 1719.37 ms sleeping until kernel `kgsl-events` wakes it. Durations
across rows overlap and **must not be added into a frame budget**.

This narrows the problem beyond generic mutex contention: long submits spend
almost all their elapsed time off CPU and resume after the completion workers.
`queuePresentKHR` itself is comparatively short in this capture. A dedicated
submit worker may prevent propagation to command decoding, but cannot alone
remove the underlying completion dependency or make GPU work cheaper.

Independent non-atomic `/proc/task/{wchan,syscall}` samples observe futex waits
and ioctl `0x400c0907`; fd 74 resolves to `/dev/kgsl-3d0`. That ioctl is
`IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID`. The raw scheduler trace also contains
`hwsched_sendcmd` blocked reasons. Some other caller fields are zero or implausible;
do not interpret every kernel caller field as a reliable unwind. The two proc
reads are not simultaneous and do not establish the surrounding Vulkan API.

## Driver hypothesis to verify on the rooted device

The driver-reported Mesa revision's
[timeline implementation](https://raw.githubusercontent.com/chaotic-cx/mesa-mirror/5ac41be677/src/vulkan/runtime/vk_sync_timeline.c)
protects timeline allocation, lookup and garbage collection with a timeline
state mutex. Its normal blocking wait explicitly releases that mutex around
the underlying wait. Therefore, “all timeline waits hold the mutex for 50 ms”
is **not** supported by this source.

However, timeline GC calls a zero-timeout underlying wait while holding the
mutex. The matching
[KGSL implementation](https://raw.githubusercontent.com/chaotic-cx/mesa-mirror/5ac41be677/src/freedreno/vulkan/tu_knl_kgsl.cc)
turns an expired/zero deadline into an ioctl timeout of zero. The local AOSP
KGSL reference in `workspace/bug_reports/references/aosp_kernel_msm_kgsl` treats
zero in `adreno_drawctxt_wait` as an infinite wait. **That is not yet proof of
the running Thor kernel's semantics**, but it is a concrete candidate for why
an apparent poll can serialize submissions behind GPU completion. Confirm the
running kernel's ioctl argument and call chain before changing any behavior.

Rooted-device follow-up should correlate the actual waiting futex address,
timeline object, ioctl timeout/context/timestamp, submitter and GpuDone stacks,
and KGSL queued/submitted/retired events. Distinguish timeline GC polling,
legitimate GPU waits and queue admission pressure. Do not remove valid resource
retirement/fence waits or declare submitted work complete early.

## StatusLayer A/B/A

Same process/generation/camera, approximately 21 s per window, resident ring and
GPU queries retained, file capture off. Only overlay visibility changed:

| Overlay | Guest flips/s | Vulkan submits/s |
|---|---:|---:|
| Shown A1 | 13.363 | 129.050 |
| Hidden B | 13.618 | 36.378 |
| Shown A2 | 13.404 | 129.317 |

Removing redraws eliminates much submission traffic but improves this short
window by only about 1.8% against the mean of the controls. It does not explain
the principal long frame. The overlay was restored to shown. This is one short
sequence, not a statistically established sustained speedup.

## Current CPU and shader policy

| Area | Current shadPS4/FEX behavior | Comparison / limitation |
|---|---|---|
| CPU backend | FEX x86-64 -> ARM64 JIT; FEX Release, optimized build; host/JNI RelWithDebInfo | Citron NCE executes ARM64 guest code natively. There is no equivalent NCE switch for PS4 x86-64 on ARM64. |
| JIT block policy | `GDBSERVER=1`, `MULTIBLOCK=0` unconditionally in FexContext::Initialize | Core-only interrupt-page entry check, not a running GDB server. Multiblock differs from FEX's true default. This is a persistent debugging-related codegen constraint, not zero cost when unattached. |
| CPU optimization / memory | Default `O0=false`; scalar TSO=true, vector TSO=false, memcpy/set TSO=false, half-barrier TSO=true | Optimizations are enabled; it is not an interpreter or an all-accuracy/no-optimization mode. Turning off scalar TSO is not a safe general fast preset. |
| CPU floating point | Default `X87ReducedPrecision=false`; native feature detection; no Dynarmic Unsafe preset applied | Full x87 path by policy; this does not imply x87 is a measured TMNT hotspot. AFP support is detected, not asserted from the device name. |
| Shader arithmetic | FP32 ops remain FP32; add/sub/mul/div/FMA emit `NoContraction`; no general `RelaxedPrecision`/FP16 fast preset | Conservative contraction policy. This is not a complete guarantee of hardware-exact PS4 floating point. Denorm handling also depends on guest registers and device support. |
| Renderer accuracy policy | No Low/Normal/High/Extreme preset corresponding to Citron | GNM submission gate and actual completion checks are concrete policies, not a selected “High shader precision” UI value. |

Source anchors: `src/core/guest_cpu/fex/fex_context.cpp:1033`,
`references/FEX/FEXCore/Source/Interface/Config/Config.json.in`,
`src/shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp:9`.
The embedder sets options after ReloadMetaLayer and does not load FEXLoader's
environment configuration layer. The listed defaults are source-derived policy,
not a live debugger dump of every FEX option.

Local Citron `3da08ee52d` distinguishes CPU backend NCE/Dynarmic from Dynarmic
Auto/Accurate/Unsafe/Paranoid. Its GPU accuracy also controls fence completion,
command-memory reads, macro refresh and texture-coherency checks. Calling all
of these “shader precision” would hide synchronization changes. This audit
did not reproduce or dispute the user's newer BOTW comparison; locally retained
BOTW measurements are historical and do not identify that test's exact settings.

Priority after driver attribution: remove unnecessary CPU blocking at the
submission/completion boundary with correct command/resource ownership; separately
measure the cost of always-single-block FEX while retaining pause/cancel/debug
publication correctness. Shader contraction/precision A/B needs separate cache
identity and pixel validation. None of these settings was relaxed in this run.

## Evidence quality and cleanup

- [Artifact hashes](vulkan-wait-chain-20260916/artifacts.json),
  [scheduling analysis](vulkan-wait-chain-20260916/scheduling-analysis.json),
  [overlay A/B/A](vulkan-wait-chain-20260916/overlay-aba.json).
  Large raw PROF/handoff/ftrace files remain in `build/vulkan-wait-chain-20260916`;
  the dated analyzer expects those files alongside it. This is not a portable
  general-purpose collection tool.
- Both PROF imports retain boundary-span truncation diagnostics, zero skipped
  chunks and no gap markers. Only complete relevant intervals are compared.
- Device Perfetto v25 probes repeatedly failed during ftrace setup; both 1237-byte
  outputs contain no useful scheduling events. The first raw 10 s fallback trace
  overran its buffer and is excluded from the quantified table. The second 2 s
  trace has no reported buffer overrun. Its capture overhead is not zero.
- Native debugger first rejected unprovable default LLDB version identity.
  Matching NDK 21 liblldb/server with CodeLLDB passed the version check but the
  adapter closed during attach. Session `b895d8f1ba6b44a9a6a52d6f1d691ad9` evidence
  was exported; cleanup confirmed process alive, unchanged start time,
  `TracerPid=0`, no owned server. All reported performance captures precede
  this attach attempt. No valid native stack was obtained.
- Final state: game still Running at generation 2, no automated input, file
  capture ready/inactive, handoff duration-stopped, overlay shown, tracefs off
  with original local clock and 4 KiB per-CPU buffer restored. No debugger
  attached. No production modification, rebuild, full regression or commit/push.
