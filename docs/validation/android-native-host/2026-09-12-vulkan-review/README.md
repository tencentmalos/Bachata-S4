# 2026-09-12 Vulkan/NDK repair evidence

See [review and handoff](../vulkan-review-2026-09-12.md). Base `fd3587fd` plus working-tree repairs, no child changes or game execution.

- `before-syntax.json`: all104 root-CMake graph source files, 97 pass/7 fail. `verify_graphics.py` is the historical pre-fix review driver; the maintained tool is now under scripts/android.
- `after-objects.json`: final stable source manifest (including untracked headers), commands, compiler, source diff and104 successful `-c` results. Paths inside records are observations from this machine.
- `objects.json`: each output verified ELF64 little-endian ET_REL/EM_AARCH64; binaries remain under ignored build/. Logs beside this file correspond to final commands (passing TUs often have empty logs); `before-failures/` preserves original failing logs separately.
- `source-fix.patch`: tracked production changes only; new header/tests/runner are actual working-tree files and must be included when committing.
- `session-lifecycle.txt` (807/0), `swapchain-acquire.txt` (19/0), `runner-negative.json` (both old wrappers exit1 on missing source).
- `ayn-vkjson.json` / `device-summary.json`: **system** driver read-only evidence, not Turnip/app/WSI/game results. Default execution strategy is bionic Turnip; its actual loader and capabilities remain to validate.
- `pins.txt`, `miniz-config.log`: selected child pins and actual NDK miniz export-header generation.

Reproduce source compilation (an initialized dependency checkout and NDK/CMake/Ninja are required):

```sh
python3 scripts/android/check-host-ndk-sources.py \
  --ndk "$ANDROID_NDK_HOME" --out build/host-graphics-check --jobs 6 --emit-objects
cmake -S tests/video_core -B build/video-core-tests -G Ninja
cmake --build build/video-core-tests
ctest --test-dir build/video-core-tests --output-on-failure
```

The source census uses a fixed native API33 profile and explicit probe includes, not the final host CMake dependency closure. It neither links the complete host nor compiles/runs Vulkan GLSL/SPIR-V on a GPU. Later full Android CMake owns generated targets, public flags and dependency/API identity; do not use this census as a replacement.
