# Swan context preemption comparison candidate

Status: built and host-checked, NOT device-validated. This does not fix the proven failure of the previous ringbuffer candidate. The device is disconnected; no new installation was attempted.

APK: `shadps4-b0778a7f-kgsl-preempt-styles.apk`
APK SHA256: `d272bb0e422bbb531ad7b61266e75a196fbb5f341687fe7e03fbca47bed23af3`
Driver SHA256: `c1a77fde0d5b25300e55f0eb9cc59313818c9e96a707ab9b59ab6da4e37f3d82`
Host SHA256: `aa7a5a4375cce8c2c5a7914571242c972cf4163cf9dd53e63fea931de8df0518`

Source: shadPS4 b0778a7f with existing local capture changes; Mesa 86ca472fc22b88a1241bb2eece5c2c128c2f48ae plus `turnip-combined.patch`. Mapper5 encoded capture repair retained. SDS alignment and both KGSL modes are default-off.

Android process-start property `debug.mesa.tu.debug`:
- empty: original context request, style 0.
- `kgsl_preempt_rb`: style 1, already failed on Swan.
- `kgsl_preempt_fg`: style 2, new untested comparison.
- both modes together: rejected with EINVAL before ioctl.

Only one variable should be enabled per run. The property must be set before process creation and restored afterward. A returned style value does not prove the exact hardware preemption policy. No global preemption setting, KMD, firmware, context priority, or command-stream change is part of this candidate.

Before reconnect testing, follow ../cleanup-pending.json: verify Swan identity, collect persistent devcd15 and frozen B2 trace if available, restore property/cache state after verifying the full com.shadps4.android process is stopped, and preserve current test cache and saves. The installed old package is 2b0e7b7a..., and its last property is still kgsl_preempt_rb. Do not assume installing this new package clears that property or restores the original cache.

Validation: Mesa library + Android host + PlaystoreDebug APK built. APK contents hashes verified. 40 host checks extract production context-create code and UAPI constants with a mocked ioctl, UBSan enabled. They do not test KMD, GMU, rendering, or stability. Published driver SHA and generated default assets were restored; default host and APK rebuilt and verified in restored-default-identity.json.

Rebuild from repository root using the existing configured build directories: run the Mesa ninja build, then python3 build/validation/swan-cp-preempt-20260929/candidate-fg/build_candidate.py. The script temporarily binds the local library into host/APK and restores source SHA pins and published assets in a finally block. Rebuild default host/APK afterward; keep candidate APK separate. No release URL or source dependency was changed.
