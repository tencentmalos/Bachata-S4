# HN1.1 — dependency closure for an embeddable headless host

Date: 2026-09-11. Base `codex/android-fex-round2` @ `9ac6c300` + HN0 commits.
This is the HN1.1 deliverable from [the spec](../../specs/android-native-host-v1.md):
map the source/dependency closure of a reusable, headless (no SDL, no renderer,
no audio) host so the desktop executable and the Android JNI both compose from it
instead of each aggregating the whole core. Findings are read from the current
tree; the target split is a design a follow-up executes against a configured
build (desktop configure on this host is slow but proceeds; a full build is the
compile gate for the refactor).

## What the desktop executable aggregates today

`CMakeLists.txt:1263` `add_executable(shadps4 ...)` directly compiles the source
lists `COMMON CORE VIDEO_CORE SHADER_RECOMPILER AUDIO_CORE INPUT EMULATOR IMGUI
SHADNET` plus `main.cpp`/`emulator.cpp`/`sdl_window.cpp`, and links a broad set
(SDL3, FFmpeg, ImGui, OpenAL, glslang, VMA, Zydis, protobuf, discord-rpc,
miniupnpc, cpp-httplib, …). There is **no reusable core library target**; the
Android JNI cannot compose from it without dragging the whole desktop closure.

## Verified coupling map (what is / isn't headless-clean)

Read from the current sources:

| Component | Renderer / SDL / audio coupling | Headless-clean? |
|---|---|---|
| `core/loader/` (elf/self, symbols_resolver, dwarf) | none | yes |
| `core/linker.cpp`, `core/module.cpp` | none (no video_core/audio include) | yes |
| `core/tls.cpp`, `core/aerolib/` | none | yes |
| `core/libraries/kernel/*` (threads/process/memory/time/…) | none | yes |
| `core/memory.cpp` | **`#include "video_core/renderer_vulkan/vk_rasterizer.h"`; ~10 `rasterizer->Map/UnmapMemory` calls** | **NO — one seam to abstract** |
| `emulator.cpp` | `Frontend::WindowSDL`, `g_window`, discord_rpc; `window->WaitEvent()` loop; 6× `std::quick_exit` | frontend, stays desktop-side |

The load→link→execute path (loader → linker → `linker->Execute`) is renderer-free
and audio-free. The **only** hard coupling inside the CPU/memory closure is
`core/memory.cpp` → `vk_rasterizer.h` for GPU dirty-tracking
(`MapMemory`/`UnmapMemory`). This is the abstraction the headless profile needs:
an interface (e.g. `MemoryHooks`/`RasterizerHooks`) the Orbis MemoryManager calls,
with the Vulkan rasterizer as the desktop implementation and a null/no-op
implementation on the headless path. This matches the assessment's §4.3 point that
memory ownership is the hard part.

## Proposed target split (design; execute against a configured build)

Introduce reusable static-library targets; names adaptable to project style:

- `shadps4_common` — COMMON (logging, fs, types). No SDL/renderer.
- `shadps4_host_core` — loader-memory-kernel-min: `core/loader`, `linker`,
  `module`, `tls`, `aerolib`, `core/memory` (behind the rasterizer-hooks seam),
  `core/libraries/kernel` and the minimal Orbis HLE needed by a fixture. Links
  `shadps4_common` + `guest_cpu` (public API). No video_core/audio/SDL.
- Desktop `shadps4` exe = `shadps4_host_core` + `video_core` + `shader_recompiler`
  + `audio_core` + `input` + `imgui` + `emulator.cpp`/`sdl_window.cpp` + the
  Vulkan `RasterizerHooks` impl + the full desktop dependency list.
- Android JNI `.so` = `shadps4_host_core` + the FEX backend + a null
  `RasterizerHooks` + the host_runtime session layer (HN0). Renderer/audio join at
  HN4/HN5.

Do **not** copy the giant CORE list into Gradle, and do not compile
`emulator.cpp`/`Emulator::Run` into the `.so`. The desktop keeps its real build
path; regression is scoped to the touched core cases, not an unrelated full
rebuild.

## First-batch dependency handling

| Category | First batch |
|---|---|
| fmt / spdlog / toml / needed Boost / ELF helpers | native build in the loader-minimal closure |
| FEX / guest_cpu | via cmake/fex, pinned FEX `385a0cc4` |
| Foundation | existing DebugBus/Android diagnostics only; audited network off |
| Zydis / xbyak | guest-decode audited per call; x86 stub only as guest code via FEX |
| SDL / ImGui / desktop window | decoupled from the headless host; desktop adapter keeps them |
| Vulkan / glslang / sirit / VMA / shader deps | HN4/HN5, not first batch |
| FFmpeg / OpenAL / USB / Discord / updater / net | deferred; missing symbols must return Unsupported or be an explicit block, never a stub target that links |

`core/memory.cpp`'s rasterizer include is the one first-batch blocker for a clean
headless link; the `RasterizerHooks` seam resolves it without touching the GPU
dirty-tracking semantics on desktop.

## Status

- Coupling map: **DONE** (verified against current sources).
- `RasterizerHooks` seam: **IMPLEMENTED + compile-verified.** `core/rasterizer_hooks.h`
  defines a 3-method interface (MapMemory/UnmapMemory/InvalidateMemory);
  `MemoryManager` now holds a `Core::RasterizerHooks*` and `core/memory.cpp`
  includes the interface instead of `video_core/renderer_vulkan/vk_rasterizer.h`;
  `Vulkan::Rasterizer` implements the interface (`: public Core::RasterizerHooks`,
  three methods marked `override`). Verified against the configured desktop build's
  own compile command (`build/desktop-probe/compile_commands.json`): `memory.cpp`
  now compiles with `-fsyntax-only` **without any video_core header** (the one
  headless blocker is gone); the change introduces zero `RasterizerHooks`-related
  errors in `vk_rasterizer.cpp`. (The desktop `vk_rasterizer.cpp` compile on this
  macOS host still fails on pre-existing environment issues — `std::stop_token`/
  `std::jthread` libc++ availability and a Vulkan-Hpp `eMesaKosmickrisp` version
  mismatch — which also fail an untouched `vk_scheduler.cpp`, so they are the
  toolchain, not this change; the project builds these on Linux/CI.)
  Also added the missing `#include <shared_mutex>` to `memory.cpp`, which had
  relied on `std::shared_lock` arriving transitively through the removed header.
- Target split (library targets `shadps4_common` / `shadps4_host_core` +
  composing the desktop exe and Android `.so` from them): **DESIGNED**,
  NOT_IMPLEMENTED — the CMake refactor needs a full desktop link (or a headless
  NDK host target) to verify, which this macOS host cannot complete due to the
  environment issues above. With the `RasterizerHooks` seam landed, the remaining
  step is mechanical: move the loader-memory-kernel-min sources into
  `shadps4_host_core`, prove the desktop exe still links on Linux/CI, then compose
  the Android `.so` from it. No `Emulator::Run` code was moved in this pass; that
  (and removing its `quick_exit` calls) stays with the target-split step where it
  can be compile-verified.
