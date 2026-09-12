# Native host library evidence — 2026-09-12

Source implementation commit: `1149e948080b475ee0c8a61dd891728f085cd610`.
See the [milestone report](../host-library-milestone-2026-09-12.md) and
[next implementation spec](../../../specs/android-native-host-production-runtime.md).

- `build-before-path-repair/`: first successful real host link at dirty `7157f801`.
- `device-before-path-repair/`, `path-initializer-crash.log`: exact deployed first artifact aborted before main, exit134, desktop path global initialization.
- `build-final/`: successful link after the explicit Android path initialization repair; complete source hashes and original dirty patch/status. Source hashes match implementation commit1149e948; generated SCM metadata retains the build-time identity.
- `device-final/`: exact host DSO + matching STL deployed/hashes compared, AYN Thor/API33/4KiB/shell UID2000;61 checks/0failures/exit0.
- `checks.json`, `portable-contract.log`, `missing-path.log`: macOS portable42/0; actual finalDSO missing-path CLI negative returns2 without constructor abort.
- `adapter-syntax.json`, `*.syntax.log`: three desktop adapters checked with the real NDK include/define profile; syntax only.
- `negative-results.json`, `api36.log`, `jobs0.log`, `static_stl.log`: actual profile/argument rejection checks.
- `elf-summary.*`, `symbols-summary.txt`, `map-summary.txt`: final ELF imports/BuildID, production symbol and map excerpts; full map SHA is recorded in build result. SDL JNI_OnLoad is still linked, FEX is not in this host-only artifact.

Raw build result paths are relative to the original build root (`runs/<attempt>/...`). In each archived build directory, their basename is the corresponding copied log/source file. Absolute local/device paths and dirty status are historical observations. `source.patch` excludes then-untracked files; their hashes are in source.json and final source exists in1149e948. The before-path-repair snapshot is intentionally older. No binary, driver package, game data or NDK is committed.

The full API/guest/HLE/WSI/lifecycle acceptance remains open. These are host link and auxiliary CLI results, not ordinary APK, ART, GPU, game or Swan acceptance.
