# COMMON/bionic independent review evidence

See [review](../pkg-v2-common-review-2026-09-12.md). The before-fix evidence uses production code `06bd43bd` within code base `7c675280`; the documentation-only delivery head was `2efe7004`. Before/after evidence is kept separately.

- `four-objects.json`: actual4-TU API33 object commands/results. Full source inventory is omitted from this smaller record; per-TU hashes and code base remain.
- `additional-probes.json`: crypto/libressl and IPC/root include checks pass; POSIX errno compile fails. Corresponding logs are beside it.
- `common_runtime_probe.cpp`: links unmodified production COMMON implementations; tests actual child-process mprotect faults, sigcontext fields and errno. Each child has a3-second alarm.
- `device-common-runtime.txt`:49 checks pass; malformed alignment accepted is separately printed as OBSERVATION, not counted as PASS.
- `manifest.json`: exact source and artifact SHA, matched device binary, native API33 and observed Android13/4KiB environment. This is shell/CLI auxiliary evidence, not app/Turnip/game validation.

Binary stays in ignored `build/pkg-v2-common-review/`; the reviewed device copy is under `/data/local/tmp/shadps4-common-review/`.

## Fixes and regression evidence

Both reviewed boundaries were fixed in this review and are committed with this evidence. The permanent tests are in [tests/common](../../../../tests/common/CMakeLists.txt).

- `fixed-manifest.json`: exact repaired source hashes, NDK CMake/build commands, device environment, matched artifact SHA and each test exit code.
- `fixed-common_error_tests.txt`: Android default GNU strerror_r, 7/7.
- `fixed-common_error_posix_tests.txt`: Android POSIX strerror_r, 7/7; explicitly built with `-U_GNU_SOURCE -D_POSIX_C_SOURCE=200809L`.
- `fixed-common_signal_context_tests.txt`: 50/50, including rejection of the formerly accepted misaligned record and 40 real child-process access faults.
- `fixed-host-common_error_tests.txt`: macOS POSIX errno 7/7 and CTest result.

Reproduce the Android targets with the manifest's CMake command, using your local NDK toolchain path, then build and run each executable via adb. Host check: `cmake -S tests/common -B build/common-tests -G Ninja && cmake --build build/common-tests && ctest --test-dir build/common-tests --output-on-failure`. Device checks remain shell/CLI auxiliary evidence; no ordinary APK/Turnip/game result is claimed.
