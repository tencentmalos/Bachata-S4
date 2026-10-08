# Citron online shop extraction into Foundation

Date: 2026-10-07. Scope: a shared non-UI SDK for Citron and shadPS4, with Android,
Windows and macOS as first-class build targets. No user account or cloud files
were used in this validation.

## Findings

Citron has two independent online providers. Its Baidu shop uses Gamer520 for
catalog/article/artwork/share discovery and a Go Baidu worker for transfer and
publication. Ghost is separate and remains in Citron. The extracted feature set
includes search, pagination, six Switch sort modes, article metadata/artwork,
public share-code resolution, login status, file-selection plans, task queue,
4/8/12/16/20 range connections, pause/resume/cancel, range identity validation,
SHA-256 receipts, encrypted/multipart archive handling through 7-Zip, publication,
local removal and Switch NSZ normalization.

The original implementation coupled deployment to macOS: a sibling Go executable,
sibling 7zz, and Swift/WebKit login. The portable layer was already mostly Rust/Go,
but it was not an Android SDK. Forking an executable from writable app storage is
not an appropriate Android deployment model. Windows also needed replacements for
Unix statfs/no-follow file APIs and atomic replacement semantics.

[Gamer520](https://www.gamer520.com/) links its PS4/5 menu to
[2468c](https://2468c.com/), rather than using the Switch category. The Switch
WordPress taxonomy confirms category 2; it is not a PS4 category. The PS4 site's
homepage and taxonomy API returned HTTP 403. No verified PS4/PSVR category IDs or
working online catalog are claimed. The new adapter reserves this origin, requires
a verified category before browsing, and allows user-supplied Baidu share links.
PSVR is PS4 catalog metadata; it is not a separate downloadable container format.

## Implementation

The shared module is [Foundation online_content](../../../foundation/modules/online_content/README.md).
It can build alone, or through the opt-in `SPATIAL_BUILD_ONLINE_CONTENT` option.

| Layer | Responsibility |
| --- | --- |
| Rust SDK | Catalog/resolver/cache, queue and persistence, cancellation, progress, source identity, C ABI |
| Go shared library | Baidu API/transfer/integrity, filesystem backends, archive-tool invocation and publication |
| Android JNI/Kotlin | UTF-8 commands, handle lifetime and APK native library packaging |
| Host adapter | Storage and cloud namespace, cookies, archive binary path, lifecycle, login UI |
| Emulator | Format metadata/decryption, installation/mounting and launch |

The Go library is loaded in-process, with operation-local event sequences and
cancellation. Closing a service joins its writers. Go's runtime stays loaded until
process exit. Its shared-library signal handling also shares the host process;
emulator fault-handler chaining during concurrent gameplay remains untested.
Cookies can be supplied in memory; clearing them disables the legacy
credential-file fallback for that owner. Logs expose allowlisted fields. State is
bound to an absolute host-selected root and source profile; workers do not infer
application bundles or default platform credential locations.

Windows implements disk-space and no-follow checks with Win32 APIs, and Rust state
replacement with MoveFileExW. Android uses packaged arm64 libraries, static libc++,
16 KiB ELF alignment and an explicit SONAME. APK instrumentation caught and fixed
a missing SONAME: the first JNI build recorded `artifacts/libfoundation_baidu.so`
in DT_NEEDED, which worked in the build layout but failed inside an app. The fixed
library records `libfoundation_baidu.so`.

PS4 publication accepts raw PKG framing, validates the header's declared size and
entry/body/content/PFS ranges with overflow-safe checks, and retains SHA receipts.
It neither decrypts the package nor marks it compatible. Synthetic PKG fixtures
exercise invalid magic/size/table/overflow cases, successful publication, reopening
and rejection after tampering. Switch behavior is preserved in the same module.

## Host integration

Citron's Rust Gamer520 owner now delegates to the Foundation crate. The original
Rust catalog/queue/service files and Go implementation have been removed from the
host source tree. Its Ghost owner remains local. CMake builds/copies the Go library;
the macOS helper packager builds this same library plus the host's existing Swift
login and pinned 7zz. The Android project includes the shared Gradle library and
provides `Gamer520Store`; its existing online UI has not been changed to expose it.

shadPS4's Android data module depends on the SDK and exposes `OnlineContentStore`
with a PS4 root/cloud namespace and direct-share planning. Its category defaults
to zero until the site taxonomy is verified. The initial SDK extraction did not
include a store UI. The subsequent desktop integration included in the October 8
publication adds the Big Picture store page and `--online-store` entry point,
share/file review, transfer controls and adding published content to the library.
macOS adds a Swift/WebKit login helper and a versioned app packager; Windows and
Android login/UI integration remain separate work. C/C++ hosts consume the same
public C ABI without Qt/JNI.

## Validation

- macOS arm64: standalone CMake Rust archive/Go dylib build; real C ABI load,
  command/snapshot, missing-credentials delivery, category gating and close/reopen.
- Go shared ABI: 12 simultaneous isolated operations, per-operation sequences,
  invalid request handling and cancel/join through the actual dynamic library.
- Rust SDK: 21 tests; strict Clippy. Citron Rust: 14 tests and strict Clippy;
  Citron CMake library/worker targets and the full macOS Qt executable built
  successfully, with the Go dylib copied into the app bundle. Citron Android
  SDK AAR assembly and `compileMainlineDebugKotlin` passed as well.
- Go: race-enabled fixture suite, including the explicit macOS 7zz encrypted/volume
  and unsafe-archive cases, plus new PKG framing/publication tests.
- Android arm64/API 30: full Rust+Go+JNI link, SDK AAR assembly and shadPS4 data
  module Kotlin compilation. Pocket DS (Android 13): native C ABI smoke passed;
  APK instrumentation passed actual System.loadLibrary, Kotlin/JNI command and
  snapshot, Unicode/emoji storage paths, source gating and close/reopen.
- Windows x64: MSVC C++ executable + Rust MSVC static archive + Go cgo DLL built
  and linked on the Windows host. Native C ABI smoke and 12-operation shared-worker
  test passed. Rust: 15 tests passed (six Unix subprocess tests are not applicable).
  Go fixture suite passed, including PKG publication/tamper cases; two archive
  tests skipped because no 7-Zip executable was configured. The build rejects a
  MinGW host compiler to avoid mixing its archive/ABI conventions with MSVC Rust.

This does not certify authenticated Baidu download, cloud transfer, login expiry,
resume after app process death, or a complete PS4/PSVR browsing experience on all
three platforms. No emulator gameplay changes were tested or deployed.

## Remaining integration boundaries

Hosts must provide their login flow and credential storage on Windows/Android.
Compressed archives need a trusted archive tool packaged for the target OS/ABI;
the SDK takes its absolute path and does not download executable code. The Android
AAR does not currently bundle 7-Zip. Raw PKG and existing raw Switch/NSZ paths do not
require an external archive process. PS4 `name_contents` metadata/renaming is left
to the host; the existing naming command is Switch-specific.

Desktop Linux, Windows ARM64 and other Android ABIs are not runtime-validated.
The SDK is therefore portable and usable at the API layer on the tested targets,
while end-to-end store availability also depends on site access, login, packaging
and host UI integration.

## Reproduction artifacts

[Saved test logs and Android JUnit result](evidence/) accompany this report.

Local build/test logs and artifacts are in `/tmp/content-sdk-validation` on the
build Mac; the generated AAR is under Foundation's ignored
`modules/online_content/android/build/outputs/aar/`. Windows uses the isolated
`C:\bug_reports_work\online-content-20261007` directory, including source,
scoped toolchains/caches, DLL/static library and logs. It is retained for review;
no system PATH or existing project was changed. Android's disposable test package
was removed by the test runner and its native smoke directory was removed.

At the end of the initial extraction, changes remained in the working trees of
shadPS4, Citron and their Foundation checkouts. On October 8 the Foundation module
was published as `5fbdef1c180d41bbd173aa34dcf155cac6911738` on
`origin/codex/shadps4-online-content`. shadPS4's parent pin and desktop/Android
adapters are included in [the October 8 publication](../android-native-host/git-publish-20261008.md).
This publication does not change or publish the independent Citron checkout.
