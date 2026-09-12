# shadps4_host_core — full Android host link plan (2026-09-12)

Executing [PKG v2 §4.3 → §5](android-native-host-pkg-v2.md) build foundation:
a real `--no-undefined` bionic host `.so` the in-process app links to run TMNT.
Base branch `codex/android-fex-round2` @ `4740f053`. Design evidence: root
`CMakeLists.txt`, `externals/CMakeLists.txt`, `cmake/fex/CMakeLists.txt`, the app
CMake, `emulator.cpp`, and the NDK census (`scripts/android/check-host-ndk-sources.py`).

## Decisive constraint: Bachata's Android host is glibc, not bionic
`references/Bachata-S4-android/runtime/scripts/build-shadps4-arm64.sh` builds the
full `shadps4` with `CMAKE_SYSTEM_NAME=Linux` / `aarch64-linux-gnu`, links the
glibc `ld-linux-aarch64.so.1`, needs system libXext/libuuid/libudev — a Debian
ARM64 glibc ELF that runs inside Winlator's proot container (the out-of-process
model we do NOT copy). It `find_package`s FFmpeg/glslang from a glibc sysroot, so
**no bionic externals are reusable**. Our in-process app needs a true bionic NDK
build of the same top-level source lists — new work.

## Source-list facts (root CMakeLists.txt)
- `add_executable(shadps4)` @ 1264 pulls `AUDIO_CORE IMGUI INPUT COMMON CORE
  SHADER_RECOMPILER VIDEO_CORE EMULATOR SHADNET` + main.cpp.
- **`AUDIO_CORE` is never `set()` — expands empty.** `src/audio_core/` does not
  exist. Audio HLE is `AUDIO_LIB`/`SYSTEM_LIBS` inside `${CORE}`. Do not "port
  audio_core".
- `cpu_patches.cpp` + `FIBER_LIB` are already x86_64-only (CMakeLists.txt:969-974).
- Coupling: COMMON headless-clean; CORE mostly clean (the spine); SHADER_RECOMPILER
  bionic-clean (sirit/glslang/vk-headers); VIDEO_CORE renderer-coupled but
  NDK-compiles (104 objects done); IMGUI SDL/UI-coupled (exclude sdl3 backend +
  big_picture, keep imgui_impl_vulkan + core); INPUT SDL-coupled (adapt to
  Android); EMULATOR frontend — replace, don't link; SHADNET protobuf — defer.

## Target structure (layered static → one shared)
1. `shadps4_common` STATIC = `${COMMON}` (+ spdlog/fmt/boost-headers/Tracy). Clean.
2. `shadps4_host_core` STATIC = `${CORE}` + `${SHADER_RECOMPILER}` minus renderer =
   CPU/HLE/loader/filesys/crypto spine. Links guest_cpu_fex, shadps4_common, zydis,
   libressl, zarchive/zstd, zlib, png, freetype, fdk-aac/LibAtrac9, libusb, date.
   **FIRST LINK MILESTONE** (excluding FFmpeg consumers).
3. `shadps4_host_video` STATIC = `${VIDEO_CORE}` + imgui vulkan backend subset
   (VMA, Vulkan::Headers, glslang, sirit, Android surface path).
4. `shadps4_host` SHARED (`--no-undefined`, build-id) = the above + a new Android
   Run driver (replaces emulator.cpp's SDL loop) + JNI. App links this.

Placement: root CMakeLists.txt, reuse the existing source-list VARIABLES (the
census script parses VIDEO_CORE/SHADER_RECOMPILER by regex; forking a second list
is forbidden by CLAUDE.md). Guard: `option(BUILD_HOST_CORE OFF)`; make host-core
and the desktop `add_executable(shadps4)` mutually-exclusive branches. Do NOT
overload bare `ANDROID` (the app's cmake/fex subbuild sets it; keep host libs
independently selectable + Darwin-NDK-census-testable).

## Staging order to first --no-undefined (real deps, no stubs)
1. shadps4_common NDK object closure (done) → static lib.
2. Cross-compile non-FFmpeg source externals for NDK: libressl, zarchive/zstd,
   freetype, fdk-aac, LibAtrac9, protobuf(+host protoc if shadnet), zydis, glslang,
   sirit, VMA, png, miniz, date. **Enable externals/date for Android** (currently
   `if(APPLE OR FreeBSD)` at externals/CMakeLists.txt:252) and **drop uuid/discord**.
3. Link shadps4_host_core EXCLUDING FFmpeg consumers (VDEC_LIB, AVPLAYER_LIB,
   FFmpeg AJM paths) — first CPU+HLE `--no-undefined`. Resolve undefined symbols by
   link map; NO empty RegisterLib, NO `--unresolved-symbols=ignore-all`.
4. FFmpeg-from-source (bionic) → re-add VDEC/AVPLAYER. Biggest external effort.
5. Link shadps4_host_video (Vulkan closure — 104 objects compile).
6. Combine into shadps4_host SHARED + Android Run driver + JNI → whole-thing
   `--no-undefined`.
7. Then Turnip native loader wiring, then execution (WP-B).

## Concrete risks (file:line)
1. **FFmpeg** (externals/ffmpeg-core/CMakeLists.txt:9-32) downloads glibc
   `ffmpeg-linux-arm64.zip`, no Android branch → not bionic-linkable. Blocks
   VDEC_LIB(625)/AVPLAYER_LIB(411). Cross-compile from source or defer.
2. **emulator.cpp SDL frontend** (:60 g_window, :587 WindowSDL, :698-701 WaitEvent
   loop, :706 quick_exit; Restart :709+ uses fork/execvp). Replace with an Android
   Run driver taking ANativeWindow*, driving frames without the SDL loop; same-
   process restart = SessionCore generation model, not fork.
3. **Renderer seam SDL-typed** (vk_platform.h:26 `CreateSurface(vk::Instance, const
   Frontend::WindowSDL&)`; swapchain/instance/presenter/imgui_core include
   sdl_window.h). Android branch reads window_info.render_surface as ANativeWindow*
   but type is WindowSDL. Provide a WindowSDL-compatible shim or refactor to
   WindowSystemInfo; risk = dragging sdl_window.cpp (full SDL) into the link.
4. **Execution gaps (link OK, run FAIL — WP-B, not this link):** linker.cpp:41-63
   RunMainEntry UNREACHABLE on arm64; module.cpp:104 native-pointer guest-entry
   cast. Must not mistake a clean link for a runnable host.
5. **shaderInt64 vs Turnip:** SHADER_RECOMPILER emits Int64 unconditionally; the
   selected bionic Turnip must advertise shaderInt64 or pipelines fail.
6. **protoc host-tool** (CMakeLists.txt:1242): cross-compiling protobuf builds
   target protoc, unrunnable on host. Make SHADNET optional/deferred.
7. **Android-inapplicable:** uuid Linux-only (1318; kernel.cpp uses arc4random_buf);
   date APPLE/FreeBSD-gated but REQUIRED on Android (time.cpp); discord-RPC force
   OFF; libusb permission-gated at runtime.
8. **IMGUI/INPUT SDL depth:** imgui_impl_sdl3.cpp 273 SDL refs, big_picture SDL
   renderer, INPUT includes SDL. Exclude SDL imgui backend + big_picture; adapt
   INPUT to Android. Shallow SDL in emulator_settings/user_manager/ipc/devtools —
   audit but not deep.

## First actionable steps (this session)
A. Enable `externals/date` for Android; ensure uuid/discord/epoll-shim are not
   pulled on Android. (cheap, unblocks time.cpp link)
B. Add `option(BUILD_HOST_CORE)` + the layered target skeleton in root CMakeLists,
   reusing the source-list vars, desktop `add_executable` guarded as the else branch.
C. Drive the shadps4_common + shadps4_host_core (minus FFmpeg consumers) NDK
   configure+link, resolving undefined symbols by link map.
