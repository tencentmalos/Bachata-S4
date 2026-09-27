---
name: ps4-ida-analysis
description: Static and runtime analysis of PS4 guest code (eboot.bin / .prx) for shadPS4 — export the running game's executables, build an analysis ELF that IDA and objdump accept (SCE e_type fixed, sections, EH function symbols, the module's own relocations applied, imports named from NIDs), find import call sites and cross references without IDA, open the ELF in IDA through the reverse_study MCP, and confirm static findings at run time with memory_describe and the guest debugger (including its MCP permission file). Use when asked to reverse, disassemble or "IDA" a PS4 game function, find who calls an HLE import (scePad*, sceAudio*, ...), read a vtable, explain why a game computes a value, or when IDA "cannot open" a PS4 ELF.
---

# PS4 guest code analysis (IDA + offline tools + runtime)

Tool: [`tools/ps4-guest-code/ps4_guest_code.py`](../../../tools/ps4-guest-code/ps4_guest_code.py)
(also used by the `ps4-guest-crash` skill). Python 3.10+. Game binaries and IDA databases are
never committed: keep them outside the repo (a scratch or `D:\workspace\<game>-export` folder).

## 1. Get the executables

From a running Android session (the loaded eboot/prx set, update overlay applied):

```
python scripts/debug/export_guest_executables.py --serial SERIAL --output D:/workspace/<title>-export
```

`app0/elf/eboot.bin.elf` is the decrypted ELF. Offline alternative: `ps4_guest_code.py extract
<CUSA.zar> eboot.bin -o eboot.bin`. See [docs/guest-executable-export.md](../../../docs/guest-executable-export.md).

## 2. Build the analysis ELF (do this before IDA)

```
python tools/ps4-guest-code/ps4_guest_code.py elf eboot.bin.elf -o eboot.analysis.elf
```

Why IDA "cannot open" a raw PS4 ELF: `e_type` is `0xFE10`/`0xFE00` (ET_SCE_*), there is no
section table, and the dynamic table uses `DT_SCE_*` tags, so a stock loader either refuses the file
(idalib `open_database` returns 4, which the reverse_study MCP reports as `database_in_use`) or
loads code with every vtable and pointer table reading as 0 (the pointers are
`R_X86_64_RELATIVE` relocations applied at load time). The analysis ELF fixes all of that:

- `e_type` ET_EXEC, one section per load (`.text`, `.data`, `.bss`), `fn_<offset>` symbols from
  `.eh_frame_hdr` (160k+ functions in a large eboot);
- the module's own relocations applied as if loaded at 0 (`DT_SCE_RELA` RELATIVE and self-defined
  symbols), so vtables/pointer tables hold module offsets;
- imports named from the NID table `src/core/aerolib/aerolib.inl`: `__imp_<name>` on each GOT slot,
  `<name>` on each PLT stub (`jmp [rip+slot]`).

`--raw` keeps the original bytes (no relocations, no import names).

Addresses everywhere are module offsets (PS4 modules start at vaddr 0). Runtime address =
load base + offset; the base is in the log line `Loading module eboot.bin to 0x400000`.

## 3. Quick answers without IDA (seconds, not minutes)

| Question | Command |
|---|---|
| Where is import X and who calls it? | `imports eboot.bin.elf scePadSetVibration --calls` |
| Who calls / jumps to / `lea`s an offset? | `xrefs eboot.bin.elf 0x269cca0 0xf91c10` |
| Which vtable slot or table holds a function pointer? | `xrefs eboot.bin.elf 0x269cca0 --data` |
| Function range around an offset | `func eboot.bin.elf 0x1a256c2` |
| Disassembly around an offset | `disasm eboot.bin.elf 0x1a256c2 [--base 0x400000]` |
| String at an offset (assert text, file name) | `str eboot.bin.elf 0x492e234` |

`xrefs`/`imports` scan bytes (`call/jmp rel32`, RIP-relative `lea/mov/cmp/call [rip+d]`), not a
full disassembly: confirm a hit with `disasm`. Indirect calls through vtables do not show up as
call sites — find the vtable slot with `xrefs --data`, then look for `call [reg+slot*8]` near the
object's construction.

## 4. IDA through the reverse_study MCP

```
reverse_study_workspace_open modules=["main=D:/.../eboot.analysis.elf"] workspaceId="<title>-<ver>"
```

- Auto-analysis of an ~80 MB eboot takes tens of minutes. The MCP call times out (~60 s) while the
  native database keeps building under `%TEMP%\SpatialDebugTool\IdaMcp\native\<id>\` (watch the
  `.id0` size). Repeat the **same** `workspace_open` later; it is idempotent.
- One native database per server process: opening a different file abandons the running analysis.
  Do not "retry" with another copy of the binary.
- Once open: `reverse_study_decompile`, `reverse_study_callers`, `reverse_study_xrefs`,
  `reverse_study_func_text` take `main+0x<offset>`.
- For symbols shared with the runtime tools, record names in `guest/games/<TITLE>/<ver>/symbols`.

## 5. Confirm at run time

- **Read guest memory without stopping the game** (Android DebugBus):
  `adb shell dumpsys activity service com.shadps4.android memory_describe <runtime addr> [bytes<=256]`
  — prints the mapping, physical address and bytes. Follow a global pointer: read the global
  (`base + offset`), then the object fields.
- **Guest debugger (RSP)**: before the session starts
  `setprop debug.shadps4.guest_debug_port 24680` and `setprop debug.shadps4.guest_debug_wait 0`,
  then `guest_debug_start_session serial=... port=24681 remotePort=24680
  expectedArchitecture=i386:x86-64 byteOrder=little dryRun=false`, `guest_debug_control pause`,
  `guest_debug_manage_breakpoints add <runtime addr> stopEpoch=<epoch>`, continue,
  `guest_debug_wait_for_event`. Always `guest_debug_stop_session` and clear both properties
  (`setprop ... ""`) afterwards. Details: [guest debugger](../../../docs/validation/android-native-host/guest-debugger-implementation-2026-09-15.md).
- `permission_denied` from the native-debugger MCP means the permission file is missing:
  `%APPDATA%\SpatialDebugTool\McpPermissions.yaml` (or `$SPATIAL_DEBUG_TOOL_MCP_PERMISSIONS`),
  read on every call, everything off by default. Guest debugging needs:

  ```yaml
  permissions:
    allowPortForward: true
    allowNativeDebugControl: true
    allowNativeMemoryRead: true
  ```

  Enable only what the task needs; leave device mutation, LLDB commands and process launch off.
- A software breakpoint on a per-frame function stops every frame: use it only to prove the
  mechanism, then remove it. A breakpoint that never hits proves only that the path did not run
  while you watched — make sure the triggering game action actually happened.

## Worked example (Bloodborne CUSA03023, controller vibration)

`imports --calls scePadSetVibration` → wrapper `0x269cca0` (pad device) ← per-frame player update
`0xf95eb0` (reads accumulated strengths, sends, clears) ← strengths written only by `0xf91c10`
← mixer `0x1571810`, fed by the rumble request queue (`[mgr+0x4010]`, TAE/event producers such as
`0x1a256c2`). `memory_describe` on the queue slots showed no request was ever enqueued while
swinging at air; being hit produced non-zero `scePadSetVibration` values (`pad_vibration status`).
The static chain told where to look; the runtime check told which case applied.
