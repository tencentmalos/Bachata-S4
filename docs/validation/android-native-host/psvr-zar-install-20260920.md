# AYN: two additional PSVR archives — 2026-09-20

User requested the two remaining PSVR games under `/Users/bytedance/game/ps4`, then clarified to use newer versions and keep one ZAR per game. Final output is **two archives**, not four separate base/update archives. This layout choice applies to these two titles.

| Title | Selected content | Layout | Logical content | ZAR | Saved |
| --- | --- | --- | ---: | ---: | ---: |
| AllInOneSports / CUSA36289 | 01.02 | all-in-one: app/ + update/ | 4,203,003,771 B | 1,591,524,981 B | 62.13% |
| Tetris Effect: Connected / CUSA13427 | 02.05 integrated (SFO VERSION=02.05, APP_VER=01.00) | single integrated game root; no backport overlay | 9,739,675,926 B | 5,246,330,525 B | 46.13% |

Local final directory: `/Users/bytedance/game/ps4/zar/`.

- AllInOneSports combines the supplied base and 1.02 update as internal `app/` and `update/` layers in `CUSA36289.zar`. The only supplied 1.02 update is labelled BACKPORT by its source filename; its SFO category is `gp`. The archive retains both layers in one file, using the existing all-in-one mount support.
- Tetris Effect: Connected uses the supplied 9.00 integrated package in `CUSA13427.zar`. Its SFO **VERSION=02.05, APP_VER=01.00**; those original fields are unchanged. Consequently the library may show 1.00. The separate 5.05–7.55 backport is excluded after the user's clarification. No standalone DLC was present.
- RAR containers were extracted with `unar`; PKGs were classified by SFO and extracted using the repository's `zar_packer`, with the existing `build/zar-tools/zarchive` writer. Archive root layouts and nonempty executables were checked. Both final archives were fully read and compared byte-for-byte against all extracted source files (250 and 166 files respectively); file counts and total logical bytes also match. The savings refer to extracted logical content, not RAR/PKG sizes.
- Original source archives remain untouched. Earlier split Sports output and the unused Tetris backport derivative produced before clarification were removed; they are not additional delivered archives.

Device target is **AYN Thor `9c2841a4`**, package `com.shadps4.android`, using the already installed APK. Final locations are `files/games/<TITLE_ID>/<TITLE_ID>.zar` in app-private storage. Transfers use hidden staging plus `.partial`, check exact size and SHA-256 on device, and rename only after verification. Native library reconciliation reads effective SFO/icon through the production archive mount and creates the install manifest. No game content is expanded into loose device files.

This task validates packaging, transfer and library registration only. Game launch/playability and VR rendering remain for subsequent testing; no new APK or driver was installed.

Evidence: [input identities](psvr-zar-install-20260920/pkg-inventory.json), [Sports archive](psvr-zar-install-20260920/sports-final.json), [Tetris archive](psvr-zar-install-20260920/tetris-final.json). Device transfer and library registration are verified below.

## Final device evidence

Both final files match local SHA-256 and exact byte size: [installed archives](psvr-zar-install-20260920/installed-archives.json), [transfer](psvr-zar-install-20260920/device-transfer.txt). Native archive inspection created `INSTALLED` manifests for both titles. Cached SFO bytes exactly match Sports' internal update and Tetris' integrated base, respectively: [registration evidence](psvr-zar-install-20260920/library-registration.json). Each device title has exactly four files: one ZAR, SFO, icon and install manifest.

During read-only inspection after refreshing the library, the device was already showing a Tetris startup failure: `missing guest dependency: EOSSDK-PS4-Shipping.debug_prx`. The captured error is [here](psvr-zar-install-20260920/tetris-startup-error.png). The source/archive inventory does contain `prx/eossdk-ps4-shipping.prx`; packaging comparison passed, so the observed failure needs guest module name/search-path resolution analysis rather than an unsupported claim that the transfer omitted the SDK. No runtime alias or library-loading change was made in this packaging task. AllInOneSports gameplay was not inspected. The user may continue testing; no new running game was stopped for this verification.

An initial `adb exec-in` upload did not create the staged file and was rejected before publication. A bounded byte round-trip verified `adb shell -T`; the final two transfers used that path and passed complete on-device hash checks. Failed attempt remains in local build evidence.

Temporary unpacked PKG/game trees were removed after verification; original RARs and the two final ZARs remain. [Cleanup](psvr-zar-install-20260920/cleanup.json), [evidence manifest](psvr-zar-install-20260920/manifest.json).
