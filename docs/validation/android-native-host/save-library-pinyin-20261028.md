# Android save library and Chinese IME — 2026-10-28

## Scope

User confirmed library-only entry. Game details now offer **存档管理**, listing real user/save directories, SFO subtitle/detail, latest modification, aggregate size and constituent files. Detail view supports independent notes, refresh and unsaved-close confirmation. No pause-menu entry, save deletion or binary editing was added.

Save identity uses INSTALL_DIR_SAVEDATA before TITLE_ID. Managed EmuPSP entries use admitted metadata; ordinary installations resolve active launch metadata, including updates. The configured global home directory is respected; default is `files/host/home`. Scanning skips symlinks, bounds depth and listed files, and reports unreadable entries.

Notes are UTF-8 sidecars under `files/save-notes/<SHA256(home,user,title,directory)>.txt`, separate from game payload, capped at4096 UTF-16 characters/16KiB. Atomic temporary-write/fsync/rename preserves the previous note on failure; empty text removes only that note. Writes reject moved/deleted/forged paths. Modal controller events cannot launch a game behind the dialog. Touch editing uses the device's system IME. Dialog layout accounts for keyboard/system insets.

## SteamPSP Chinese keyboard port

The PS4 in-game Ime/ImeDialog panel now has EN/中 selection, pinyin preedit, paged candidates, space/enter commit and composition-aware backspace/cancel. The existing checked text/filter/UTF-16 limits remain in the insertion path; password and non-text modes disable pinyin. The two native dialogs share one leased decoder and release it on teardown/move. This panel is distinct from the Android system keyboard used by save notes.

Port based on local SteamPSP `app/src/main/cpp/xrimmersive/xr_pinyin.{h,cpp}`. After the user updated the shared input-method dependency, shadPS4 now consumes Foundation `modules/pinyin` through `spatial::foundation_pinyin`; the dictionary is embedded from its `FOUNDATION_PINYIN_DICTIONARY` property by `cmake/pinyin`. The earlier duplicate `externals/pinyinime` tree was moved to local validation evidence and is no longer part of the build. Desktop/Android use the same embedded dictionary, with no runtime download. Learned data is private to the app and separate from game saves.

Foundation merged SteamPSP's `addcb9bf` with existing profiler/audio history (merge `188b832e`). Nested PinyinIME starts at fork revision `97886655` (AOSP `49aebad1`, including LP64 fixes). Long ambiguous input reproduced the bounded milestone-pool assertion in that shared revision: an empty-root extension published candidates after failing to obtain a dictionary handle. The shared decoder now rejects that failed extension and resets its candidate count (`229ef17`); Foundation adds a repeated long-input/reset/reopen smoke case and pins the fix (`1f869922`). These are **local commits, not pushed**; publish the nested dependency before Foundation before the parent repository. Dictionary SHA256: `6bf0bbde4e3134cce38d08524a9f4dc1af40435243c8e36b38eb68d1e14462b2`.

## Verification

- Kotlin core:data94/0, feature:library20/0. Save-specific cases cover Unicode/emoji persistence, unchanged payload bytes/timestamps, user/title separation, symlinks, removal, excessive notes and malformed SFO. Modal controller regression prevents background launches.
- Pinyin decoder Mac23/0 and Odin3 Android23/0: 你好/中国/中文, backspace, literal text, reset, invalid candidates, two-owner exclusion, move/reopen and80-letter input bounded to40. The original long-input assertion is retained in evidence.
- Full Android host and F-Droid APK built; final `c73dda7a` installed and device SHA checked (see [GoW4 report](gow4-startup-20261028.md)).
- Odin3 library lists7 existing GoW3 saves. Detail correctly shows 愛芙羅黛蒂的寢室, difficulty, playtime9:20:45, three files/362kB. A UI-entered note `Save——note——test` persisted outside the save. All three original payload SHA256 values match before/after. Reopen/cleanup and keyboard layout final check recorded below.
- Native in-game Chinese panel visual/gameplay input has not yet been exercised in a game; decoder tests and host compilation do not replace that check. Desktop full application build and UI were not tested in this task.

Shared-dependency follow-up: Mac composer23/0, Foundation smoke1/1 and Odin3 composer23/0 passed; full Android host/APK passed. APK `927928e6` / host `82017e63` was installed and device SHA verified before the renewed GoW4 investigation. The old shared decoder assertion is retained in `pinyin-shared-negative.log`; `candidate-8-identity.json` records full identities. This supersedes the earlier vendored-decoder build, without adding an in-game visual acceptance claim.

Local evidence: `build/validation/gow4-startup-20261028/`, especially save XML/screenshots, notes-ui-saved.json, save-before/after-sha.txt and native-tests. Parent changes remain uncommitted; shared dependency local commits are listed above. Nothing was pushed; preserve earlier dirty work.

## Persistence and cleanup completion

The note was reopened from the library after process termination and an APK update: exact text restored. It was then cleared using the visible editor and **保存备注**; the notes directory became empty. The original three game files still match the before-test SHA256 values. `save-reopened-list.xml` and `save-reopened-detail-2.xml` retain the round trip; `save-cleanup-sha.txt` retains the final payload check. Final UI spacing keeps the keyboard away from the action row. Native IME candidate focus is now highlighted for controller navigation; in-game visual validation remains outstanding.

Final Odin3 keyboard screenshot `save-keyboard-release.png`: action text bounds y442–489, keyboard starts y533; both actions remain visible. Device left in the save detail page with the keyboard hidden and test note removed.
