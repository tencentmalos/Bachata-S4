---
name: ps4-guest-crash
description: Take apart a PS4 guest crash from a shadPS4 log — map the faulting address to a module offset, pull the module out of the game (.zar or folder), disassemble the function and its callers, read assert strings, and read the Windows crash report (registers, stack, pointed-to memory, mapping aliases, recent mapping changes). Use when a log shows "Unhandled Exception", a game panic/assert (e.g. "Dantelion2 Panic", a write to address 0), an access violation in guest code, or when asked to disassemble game code, extract a file from a .zar, or convert a SELF to an ELF.
---

# PS4 guest crash triage

Tool: [`tools/ps4-guest-code/ps4_guest_code.py`](../../../tools/ps4-guest-code/ps4_guest_code.py).
Python 3.10+, `llvm-objdump` (PATH, `--objdump`, `$LLVM_OBJDUMP`; found automatically in Visual
Studio's LLVM, Homebrew LLVM and the NDK), and zstd for `.zar`: Python 3.14's `compression.zstd`
or `pip install zstandard`. Check the environment with `selftest` first.

## One step: log → disassembly

```
python tools/ps4-guest-code/ps4_guest_code.py crash <log> \
    --game <CUSAxxxxx.zar | game dir> [--game <update .zar/dir>] \
    [--sys-modules <user dir>/sys_modules] --frames 4 --save <scratch dir>
```

It takes the last crash line (`Unhandled Exception code … at 0x…`, or a POSIX signal line; override
with `--address`), finds the loaded module from the `Loading module X to 0xBASE` lines, extracts
that module, prints the containing function (from the module's `.eh_frame_hdr`) with the faulting
instruction marked `==>`, and with `--frames N` the call site of each reported caller frame.
`--save` keeps the module and an analysis ELF for further work. Put outputs in the session
scratchpad; never commit game binaries.

Offsets printed are module offsets (the module's vaddr; PS4 modules start at 0), which is also what
[guest symbol indexes](../../../guest/games) use. Runtime address = base + offset.

## Other commands

| Command | Use |
|---|---|
| `ls <zar/dir> [path]` | list a directory inside an archive |
| `extract <zar/dir> <path> -o out` | one file out of a `.zar` (no full unpack) |
| `elf <self/elf> [-o out]` | SELF → analysis ELF with sections and `fn_<offset>` symbols (for IDA, objdump, gdb) |
| `func <module> <offset>` | function range containing an offset |
| `disasm <module> <offset> [--base B]` | disassemble around an offset |
| `str <module> <offset>… [--hex]` | C string / bytes at offsets, e.g. an assert message loaded by `lea rdi, [rip+…]` |

Only decrypted / fake-signed SELFs are supported; encrypted or compressed segments are refused. The
analysis ELF only rewrites `e_type` and adds a section table and symbols — loadable bytes are the
original.

## Reading the Windows crash report

`src/core/signals.cpp` writes, for an exception nothing handled:

- `Crash: exception … reading/writing <addr> at <module>+<off>: <instruction>` and the fault
  address's mapping.
- All general registers; then `[rsp+…]` qwords of the innermost frame (up to `rbp+0x10`, ≤0x400
  bytes).
- For each register or stack value pointing at readable non-stack, non-module memory:
  0x10 bytes before `|` 0x20 after, plus `type start-end prot phys P[, also at A (…)]` — other
  mappings of the same physical byte. Aliases are legal but explain corruption far from its writer.
- `frame #N <module>+<off>` — frame-pointer chain (guest code keeps `rbp` frames).
- `mapping #seq (k ago) <op> start-end detail … by <thread>` — recent map/unmap/protect/pool changes
  covering any dumped address.

Things that routinely mislead:

- **Registers in a helper are not the caller's.** A panic/assert helper saves callee-saved
  registers and reuses them. Read its prologue (`push r12` …) — the caller's values are in the
  innermost frame's stack dump just below `rbp` (`rbp-8` = first pushed). Registers the helper
  never pushes (e.g. `r13` if absent) still hold the caller's values.
- **A panic writes to address 0 on purpose** (`mov dword ptr [0], 0xdeadba`). The fault is frame #0's
  caller; read its assert string with `str`.
- **Compressed pointers.** Game objects may store `address >> 8` in 32 bits (`mov eax,[rdi]` then
  `shl rdi, 8`). A wrong value there frees or dereferences a different block.
- An address that is not an instruction boundary in the decode (`note:` line) means the module or
  base is wrong, not that the CPU executed mid-instruction.

## After locating the crash

A heap that the game itself reports as corrupted ("improper or freed already", bad block header)
is usually a symptom: find the writer. Useful checks, in order:

1. Did an emulator path write that memory late? GPU write-backs to guest memory
   (`TextureCache::DownloadImageMemory`, `BufferCache::DownloadBufferMemory`), PM4
   `WRITE_DATA`/EOP labels, HLE output buffers. Write-backs must happen while the pages are still
   tracked; an image freed before its deferred write-back lands on reused memory.
2. Did the mapping change? The `mapping` lines show map/unmap/protect over the block; unmapping
   flexible memory zeroes its physical backing, so a stale `phys_areas` entry zeroes live data
   elsewhere. `also at` aliases show double-mapped physical memory.
3. Is it reproducible with a feature off (`--upload-diag "fill_clear off"`, texture quality, render
   scale)? Record every run, including those that did not crash; one run is not a correlation.

Record findings (log path, module SHA, offsets, what was ruled out) in the validation doc for the
title; keep failed attempts.
