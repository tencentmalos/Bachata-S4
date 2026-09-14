# Android R8 tiling probe

`run_android_r8_tiling.py` compiles the actual `tiling.comp` for three R8 tile
layouts and checks both directions against independent CPU address fixtures on a
real Vulkan device. All byte lanes, zero values and a nonzero destination sentinel
are covered. No game data is required. See the script's `--help` for its explicit
NDK, adb serial, package and private loader parameters. `glslangValidator`,
`spirv-val`, `spirv-dis` and `adb` must be on PATH.

The loader factory is built from `cmake/renderdoc`. It and the pinned driver/hooks
must already be in a directory readable by the selected debuggable package.
The probe does not silently use system Vulkan and does not install an APK or alter
GPU layer settings. Each run uses and removes its own device scratch directories.
`results.txt` retains commands, queried features and numerical mismatches; failure
exits nonzero. This is auxiliary compute validation, not ordinary-game APK or
all-format/MSAA acceptance.
