# RenderDoc Android replay with the capture's Turnip

The normal shadPS4 capture works through Android's GPU debug layer settings. The
paired RenderDoc Android replay server initially used the system ICD and rejected
the capture's `VK_KHR_workgroup_memory_explicit_layout` requirement. Do not remove
that requirement from the capture or change the game's driver to work around it.

`android-vulkan-loader.patch` adds an explicit replay loader factory to RenderDoc.
Apply it to an isolated checkout of the **actual configured MCP ABI manifest's
commit** (validated here: `6ae929af16150fe39fd13f2c41b4ee00b60c704b`). Keep the
RenderDoc host, ABI manifest and Android package paired. The Android package's
base revision remains that commit, with this patch and APK/DSO hashes recorded as
a dirty build. The unrelated working checkout is not modified. The patch also
permits reusing an existing `renderdoccmd/debug.keystore`; use the installed
server's signing key for `adb install -r`, never uninstall to bypass a mismatch.

Build the adapter from this directory with the NDK Android toolchain,
`ANDROID_ABI=arm64-v8a`, `ANDROID_PLATFORM=android-26`, `ANDROID_STL=c++_static` and
`CMAKE_BUILD_TYPE=Release`. It builds the repository's pinned adrenotools and four
hook libraries. Place `librenderdoc_turnip.so`, all four hooks and the exact
capture driver `vulkan.ad07xx.so` together in the **replay server's private files**
directory. Validate every SHA; do not point to another APK's private directory.
The factory keeps its loader/namespace alive for the process and never loads the
game/JNI DSO. An explicit factory failure has no system-driver fallback.

Save `debug.rdoc.vulkan_loader` before testing, set it to the absolute private
`librenderdoc_turnip.so` path, and restart the replay server. Restore the saved
value afterward. The normal empty-property path is unchanged. Check the actual
replay log for `VK_DRIVER_ID_MESA_TURNIP`, exact driverInfo and adapter startup;
a successful `dlopen` alone is not driver acceptance.

Use the paired MCP `rdc_offline_warmup` with host RDC, Android device and device
RDC path together. Its default flicker projection preserves actions, resources,
outputs and candidate images; it does not materialize deep shader/descriptor
state. For a persistent online Android inspection, run the same paired MCP
server with `--android-device=<serial>` and `--android-capture-path=<device RDC>`;
open the host RDC only in **that remote-configured server**, never replay Adreno
captures locally on macOS. Keep one live remote controller and close it after
inspection. Save typed queries and image review outputs.

See the main graphics-toolkit delivery for capture/driver/build hashes, failed
system-ICD evidence, successful private Turnip replay and remaining limitations.
This local SDK adaptation is not an upstream published RenderDoc release or a
new repository gitlink. GPU Reshape app instrumentation is a separate path; the
isolated plain replay APK did not bundle a GPU Reshape replay provider.
