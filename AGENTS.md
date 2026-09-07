# Repository context for coding agents

## Project objective and current state

- This fork is developing toward Android 16 / ARM64 / **16 KiB host pages**: native ARM64 shadPS4 host services, with **FEXCore executing only PS4 x86-64 guest code**. Beat Saber means the PS4/PSVR release; one non-VR game is also a target.
- The current repository still contains the desktop shadPS4 core. The Android application is a pinned reference under `references/Bachata-S4-android/android/BachataS4`; an integrated NDK backend has **not** been implemented or validated here.
- Start with [the Android/ARM64 integration audit](docs/android-arm64-integration-audit.md), then [the implementation plan](docs/fex-android16-native-guest-host-plan.md). All research entry points are in [docs/README.md](docs/README.md).
- The fixed initial baseline is commit `a712889343ccd2588b712988dae0a76a104e0dce`; its [status record](docs/baselines/2026-09-07-android-fex-foundation.md) distinguishes committed source, validation and excluded local work. Keep that historical record intact and add new milestone records for later progress.
- The next implementation milestone is [V0](docs/specs/android-fex-v0.md), with normative API and acceptance documents linked there. Use its bounded scope for the first validation build; it is a specification, not evidence of completed implementation. The [execution handoff](docs/specs/android-fex-v0-handoff.md) is the entry point for the implementing agent.
- Prioritize whether source interfaces can be integrated over small app version differences. The published app label is not proof of source or APK correspondence.

## Repository layout and reference ownership

- `src/`: this fork's emulator implementation; `externals/` and `foundation/`: build dependencies; `references/`: independently versioned source for comparison and migration. Do not overwrite the main core with a complete reference fork.
- Before changing a dependency, read [subrepository ownership](docs/subrepository-ownership.md). Android/ARM64 development branches live in `tencentmalos/Bachata-S4`; FEX now has a `tencentmalos/FEX` fork with separate old-runtime and new-audit branches. Reference gitlinks remain unchanged; branch existence is not proof of integration or permission to bypass contribution rules.
- Reuse [Spatial Foundation](docs/foundation-integration.md) for common reflection/packing, host diagnostics and networking. The current `shadps4::foundation` target enables only DebugBus (plus dumpsys on Android). Reflection/network still require their actual NDK dependency and lifecycle audit; do not invent replacement frameworks or claim that they are already enabled. Guest CPU/Orbis semantics remain host-owned adapters.
- Foundation's development branch is `codex/shadps4-android-fex-v0`, based on the same library used by azahar. Push child changes before advancing the parent gitlink. Do not blindly import mimalloc TLS slot settings, fixed libevent configuration, or azahar-specific dependency paths.
- [references/README.md](references/README.md) records each reference's purpose, source and pinned revision. The authoritative checkout revisions are the Git gitlinks, not a remote branch's latest HEAD.
- Android UI/session/input reference: `references/Bachata-S4-android` at `67dbf4e5…`. It includes its own historical C++/runtime combination for controlled comparisons.
- ARM64 guest/HLE migration source: `references/shadps4-arm64` at `be6bc2e9…`. It contains no Android Gradle frontend.
- FEX upstream research checkout: `references/FEX` at `50e6eee9…`. The two emulator references use **older FEX `f2b679f6…`** in their runtime locks. Keep API adaptation separate from Android integration; do not replace that pin blindly.
- `references/Bachata-S4` preserves the SG8275 bring-up fork. Local changes inside submodules are independent of parent commits. Inspect and preserve them; a parent gitlink does not save uncommitted child files.
- `references/dynarmic-citron` and `references/dynarmic-azahar` pin the two measured Dynarmic revisions. The comparison script defaults to these in-repository checkouts; full citron/azahar applications remain external references.
- Read applicable instructions within a reference before changing it. In particular, FEX's own `AGENTS.md` and `CLAUDE.md` prohibit AI-generated code contributions; its checkout is used for source inspection here.
- When changing a reference pin, verify that its configured remote contains that commit and update the reference index/evidence as needed. Avoid `git submodule update --remote` as an incidental setup step.

## Confirmed integration facts and hard constraints

1. The Android FEX launch path runs an **ARM64 glibc shadPS4 process with FEXCore linked inside**, not FEXLoader translating the whole x86 emulator. Guest/HLE veneers, ABI marshaling, pthread/TLS and callbacks already have implementations worth migrating.
2. The actual service control protocol is newline-delimited ASCII **`BACHATA/1`**. `RuntimeProtocol.kt`'s length-prefixed JSON is not the service's active C++ protocol. Do not infer pause/stop/debug support from unused scaffold types.
3. Both reference engines reject host pages other than 4096. Android 16 KiB support requires host allocation/protection, guard pages, JIT, VA probing and GPU-tracking work. **Never globally replace 4096 with 16384**: host pages, PS4 ABI granularity and internal tracking indices are distinct.
4. The current ARM64 build is Linux/glibc, not Android NDK/bionic. JNI entry points, allocator/TLS/signal integration and the dependency closure still need implementation.
5. The current renderer defines an Android Vulkan macro but lacks the Android Surface creation path. The reference Android presentation copies AHB pixels through CPU buffers/Bitmap/Canvas. Native Surface/swapchain lifecycle is a separate implementation task.
6. Vortek client/server revisions differ between the references. A successful version handshake is insufficient proof of serialization/WSI compatibility. Preserve a matching set when reproducing the legacy path.
7. `package-runtime.mjs` can replace a newly built core with a local deep-guest pin. Use `BACHATA_DEEP_GUEST_PIN=0` when validating source iterations, and check the deployed binary's Build ID plus matching symbols.
8. Ordinary DS4 input is not PSVR/Move 6DoF support. Establish a non-VR execution/render/input/audio baseline before treating Beat Saber as a VR compatibility milestone.

## Debugging model

- Read [guest debugger feasibility](docs/fex-guest-debugger-feasibility.md) and [host LLDB to guest workflow](docs/fex-lldb-host-guest-workflow.md) before implementing debugger controls.
- Prefer host LLDB plus an explicit guest thread/state adapter first. Existing FEX Linux GDB-stub behavior does not constitute a ready shadPS4 guest debugger; several protocol operations are incomplete.
- A host stop is not automatically a guest instruction boundary. CPUState may be stale while guest values reside in JIT host registers. Distinguish a spilled safe-point snapshot from an asynchronous JIT stop.
- `STATE=x28` is specific to the audited ARM64 JIT context; it is not a universal process-register interpretation. Use the exact deployed FEX/layout version and host-PC-to-guest-RIP metadata.
- Keep inspection read-only where possible. Do not evaluate inferior helpers such as state reconstruction routines without checking side effects, locking and stop context.

## Validation and documentation

- Build instructions for the desktop core are under `documents/building-*.md`; existing tests are under `tests/`. Select checks for the actual implementation change.
- Foundation's standalone build probe is `tests/foundation`; see its [integration record](docs/foundation-integration.md) for commands. macOS smoke passed and an API 35 NDK shared library was built with 16 KiB ELF alignment; neither is Android 16 runtime validation. Locally labelled r28c/r29 installs both advertised max API 35 in their actual metadata: validate the sysroot and compiler, not just the directory/package label.
- Reproduce the reference transport checks with `python3 scripts/analysis/run_android_arm64_contract_tests.py`; required reference/dependency setup is in `references/README.md`. This compiles actual C++ runtime/controller/audio code and upstream tests.
- The recorded transport result is **18/18 on macOS ARM64**. Android Kotlin/Java, FEX execution, Vulkan, APK installation and 16 KiB device execution were not exercised by that test. Do not describe it as Android end-to-end validation.
- `scripts/analysis/compare_cpu_core_size.py` measures source text at Git revisions. Nonblank lines are not binary size, complete dependency size, effort or performance; preserve the stated counting scope.
- `docs/data/` holds dated source snapshots, lock metadata and bounded validation evidence. Historical paths/Build IDs in evidence are observations, not portable setup instructions.
- Use repository-relative links in committed documentation, and exact upstream commit links when citing external code. Keep findings, proposed design and verified runtime results clearly separated.
- Before committing, inspect both parent and submodule status, stage intended paths explicitly, and verify reference availability. Do not include game binaries, downloaded runtimes, build outputs, unrelated local checkouts or credentials.
