# gcn-disasm

C# disassembler for PS4 GCN2 (Sea Islands / Liverpool) shader machine code, including the PS4 Pro
extensions the emulator decodes (the extra VOP3 opcode bit, SDWA/DPP, VOP3P).

Opcode names and operand counts are generated from the emulator's own decoder tables
(`src/shader_recompiler/frontend/opcodes.h`, `format.cpp`) by `gen_opcodes.py`, so the listing
names every encoding value the way the recompiler does. Operand widths, encoding fields and
special operands follow the Sea Islands ISA (see `Operands.cs`, `Decoder.cs`); `format.cpp`
records a single type per instruction, which is not precise for mixed-width instructions such as
`v_lshl_b64`, `v_mad_u64_u32` or `s_bfe_u64`.

## Build and run

Requires the .NET 9 SDK.

```sh
dotnet build -c Release tools/gcn-disasm
dotnet tools/gcn-disasm/bin/Release/net9.0/gcn-disasm.dll <code.bin>
```

- `<code.bin>`: raw little-endian machine code, e.g. a shader dump (`user/shader/dumps/*.bin`,
  written with `dump_shaders` / `debug.shadps4.shader_dump=1`) or
  `tools/ps4-gpu-trace/ps4_gpu_trace.py shader <trace> <hash> --out shader.bin`.
- `--hex "beea287e 000c08c1"`: decode dwords given on the command line.
- `--all`: keep decoding after `s_endpgm` / `s_setpc_b64` (decoding otherwise stops there unless a
  later branch target follows).
- `--no-raw`, `--no-labels`, `--base <hex>`, `--skip <bytes>`.
- `--scan <dir> [--pattern *.bin]`: decode every file and report instructions missing from the
  tables and programs that do not end; exit code 1 if any.
- `--selftest`: encodings from real shaders with their expected text.

Output is LLVM-style syntax with the byte offset and raw dwords; branch targets become labels,
backward branches are marked `loop back-edge`, SMRD immediate offsets are shown in dwords (as in
LLVM's SI/CI syntax) with the byte offset in a comment, and `ds_swizzle_b32` offsets are decoded.

```
  0008c: 7da8000f                   v_cmpx_gt_u32 vcc, s15, v0
  00090: bf88001d                   s_cbranch_execz label_0108
  ...
  00104: bf82ffe1                   s_branch label_008c  // loop back-edge
```

## Limits

- `image_*` address operands print only the first VGPR: the count follows from the image
  dimensions in the resource descriptor, not from the instruction.
- FLAT instructions are not decoded (the emulator does not decode them either).

After `opcodes.h` or `format.cpp` change, run `python tools/gcn-disasm/gen_opcodes.py` and commit
the regenerated `Opcodes.g.cs`. Check with `--selftest` and `--scan` over a dump directory.
