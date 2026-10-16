# KH3 GnmRHI semantic reference

Selected semantics recovered from KH3 CUSA15072 01.10 (`++UE4+Release-4.17`). This is an analysis aid, not a replacement RHI or an emulator change. The synthetic arena does not reproduce allocation lifetime or GPU scheduling.

Evidence, module offsets, partial layouts and limits: [Chinese analysis](../../docs/gnm/kh3-gnmrhi-binding-semantics.md). Names and layouts: [SHA-pinned v2 symbol index](../../guest/games/CUSA15072/01.10/symbols/index.json).

**C++ implementation:** [cpp/README.md](cpp/README.md) describes the C++20 library, backend interfaces, PM4 emitter, lifetime/query helpers, executable demo and 13 scenario tests. Start with [gnm_rhi_semantic.h](cpp/gnm_rhi_semantic.h) and [gnm_rhi_semantic.cpp](cpp/gnm_rhi_semantic.cpp). The Python model below remains a compact inspection/decoding tool.

**UE-connected reference:** [ue4-reference/README.md](ue4-reference/README.md) uses real UE RHI headers, `FGnmCommandContext : IRHICommandContext`, `FGnmDynamicRHI : FDynamicRHI`, and `FRHIResource` derived objects. It adds reconstructed CS shader/parameter/UB setters and a real command-list call-site example. KH3 4.17 semantics, signatures compared with the cached 4.18 public source; not a complete or engine-build-verified PS4 module.

**Graphics continuation:** [graphics analysis](../../docs/gnm/kh3-gnmrhi-graphics.md) and [GnmGraphics.cpp](ue4-reference/GnmGraphics.cpp) add VS/PS BSS, resource binding, vertex fetch, graphics LCUE, semantic linking and direct draws. [Stage analysis](../../docs/gnm/kh3-gnmrhi-stages.md) extends this to UE GS/ES, copy shader, ring tables and on-chip state; [GnmStagesCore.h](ue4-reference/GnmStagesCore.h) also replays the recovered LS/HS commit. CTest runs 13 original, 6 graphics and 8 stage scenarios. The KH3 UE HS/DS selection bridge, patch draw, tess factor backing and full UE/SDK integration remain explicit boundaries.

Run from the repository root with Python 3.10 or later; no dependencies:

```powershell
python tools/gnmrhi-semantics/model.py demo
python tools/gnmrhi-semantics/model.py decode-entry 0x03012a07
python tools/gnmrhi-semantics/model.py decode-binding 0x8004
python -m unittest discover -s tools/gnmrhi-semantics -v
```

`model.py` can also be imported for captured resource-map arrays and reflection values. It implements loose constant uploads, packed UB resource traversal, descriptor routing, compute LCUE table snapshots, buffer SRV base refresh and symbolic compute ordering. The demo emits JSON with synthetic addresses and the actual packet encoding for the modeled SGPR writes.

Unknown descriptor words remain `None`. Upload padding is unspecified, not zero-filled. Missing capture terminators and unsupported graphics commit are rejected. Input checks protect the analysis model; they do not imply matching checks exist in the original binary.

Tests cover semantic scenarios and known instruction-derived encodings. They do not execute or differentially test the game binary. Transfer to another game requires its own identity, offsets, layouts and confirming evidence; do not use the KH3 symbol package for a different executable.
