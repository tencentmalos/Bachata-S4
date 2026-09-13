# HN2 Stage 0 — real TMNT guest code executes on FEX (AYN Thor, 2026-09-13)

## Result: STAGE0 achieved — real title bytes translate and execute on the FEX backend

The production APK still runs a synthetic decrement loop. Stage 0 proves the FEX
guest backend translates and executes the REAL TMNT eboot's code, identifying the
exact next blocker (HLE imports).

## Method

`tools/eboot_prologue_harness.cpp` (arm64, links `guest_cpu_fex` + the host `Elf`
loader in `libshadps4_host.so`), run on the AYN Thor (Android 13, arm64/API33/4KiB):

1. Extract the real decrypted eboot from the TMNT base PKG on-device via the
   production `bachata_pkg` extractor (`PkgExtractInstrumentedTest`): contentId
   `UB0511-CUSA50828_00-...`, `eboot.bin` 27 MB, SELF magic `4f153d1d`.
2. Open it with the repo `Core::Loader::Elf` (correct SELF segment resolution).
   `type=0xfe10` (ET_SCE_DYNEXEC, dynamic PIE), `entry=0x80`, 12 phdrs, module span
   `[0, 0x1d2e000)`.
3. Create a `GuestAddressSpace` (reservation sized to the module + stack, capped at
   `max_guest_address=0x1000000000`), map each PT_LOAD (+ inter-segment gaps for
   .bss), `Write` decrypted bytes, `Protect` to segment perms.
4. Apply `R_X86_64_RELATIVE` relocations from `DT_SCE_RELA`/`DT_SCE_RELASZ` in
   `PT_SCE_DYNLIBDATA`: **92929 relocations applied** (rebased to the reservation).
5. Lay out a minimal `EntryParams` (argc=0) on the stack, set RDI=&EntryParams,
   RSI=exit target, and `CpuContext::Run` from `e_entry`.

## Evidence of real execution

- Disassembly of the entry (`0x80`) matches the OpenOrbis `_start` prologue and it
  executed on FEX: `push rbp; mov rbp,rsp; push r15/r14/rbx/rax; mov r14d,[rdi]`
  (reads EntryParams argc — the initial null-RDI crash proved this instruction ran),
  `lea r15,[rdi+8]; call <crt init>`.
- With RDI set + 92929 RELATIVE relocations applied, the guest ran through crt
  relocation/init and only faulted at **`pc=0` (SEGV_MAPERR, null call)** — i.e. the
  guest called an unresolved HLE import whose GOT slot is still null. That is the
  precise Stage 2 boundary (HLE veneers fill those slots).
- Progression across fixes is itself the proof: null-RDI deref (1 instr) → after RDI
  fixed, deep fault inside module span → after relocations+gap-fill, a null *import
  call* far into crt. Each step is real guest execution advancing.

## Boundary / next blocker (honest)

- Stage 0 does NOT run the game; it proves the CPU path executes real title code.
- The natural blocker is exactly as planned: **unresolved HLE imports** (null GOT
  call). Stage 2 = HLE veneer emitter + GOT patch so `sceKernel*`/library imports
  trap into `Hle::HleCallRegistry` instead of calling null.
- Also observed: a guest jump/call to address 0 currently surfaces as an
  unrecoverable host SIGSEGV rather than a clean `StopReason::GuestFault` (the JIT
  dispatched to guest 0). A catchable-stop for wild guest control transfers is a
  backend hardening item, tracked separately; it does not change the Stage 0 finding.
- TLS (`fs_base`) and full symbol/JUMP_SLOT relocation are not set up here; they are
  Stage 2-4.

## Reproduce

```
# 1. extract eboot on-device (once)
adb push <TMNT base v1.00 .pkg> /data/local/tmp/tmnt_base.pkg
./gradlew :core:runtime:connectedDebugAndroidTest \
  -Pandroid.testInstrumentationRunnerArguments.class=com.shadps4.android.runtime.pkg.PkgExtractInstrumentedTest
# -> /data/local/tmp/tmnt_extract2/eboot.bin (world-readable)

# 2. build + run the harness (arm64, links guest_cpu_fex + host Elf)
cmake --build build/v0-fex --target eboot_prologue_harness   # or the manual link in this doc
adb push .../eboot_prologue_harness /data/local/tmp/ && adb shell chmod 755 ...
adb shell "cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./eboot_prologue_harness tmnt_extract2/eboot.bin"
```
