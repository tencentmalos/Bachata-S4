# Vulkan subgroup routing / MHW (2026-09-22)

## Official public sources

- [Khronos GL_KHR_shader_subgroup](https://docs.vulkan.org/glslext/latest/glslext/khr/GL_KHR_shader_subgroup.html): subgroup broadcast IDs must be constant for older SPIR-V, or dynamically uniform within the subgroup for newer SPIR-V. Shuffle accepts per-invocation source IDs. Quad broadcast requires a source index uniform within the derivative group; an arbitrary lane-dependent permutation is not one broadcast.
- [Vulkan shader subgroup size rules](https://docs.vulkan.org/spec/latest/chapters/shaders.html#shaders-subgroup-size): subgroup sizes can vary; a requested size requires subgroupSizeControl, the inclusive min/max range, and the matching requiredSubgroupSizeStages bit. SPIR-V 1.6 alone does not promise the PS4's 64-lane grouping.
- [SPIR-V registry](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html): OpGroupNonUniformBroadcast, OpGroupNonUniformShuffle, OpGroupNonUniformQuadBroadcast. Static validation cannot generally prove runtime uniformity of an operand.

Accessed 2026-09-22. These are paraphrased notes, not Sony SDK ABI documentation.

## Local evidence and repair

MHW captured GCN shader 0x15e44dcc has five DS_SWIZZLE bitmask operations at +0xab8/+0xac8/+0xad8/+0xae8/+0xaf8, routing lane XOR 16/8/4/2/1, then fixed V_READLANE 31 and 63. V4 SPIR-V contains seven broadcasts: five source-ID expressions evaluate to 64 different values across 64 lanes, two are constants. This establishes invalid operand uniformity in the generated shader, independent of whether it caused the observed ErrorDeviceLost. Original byte/SHA and symbolic audit are local build evidence, not published game code.

ReadLane now emits Shuffle for both dynamic and constant IDs. The initial fixed lane31/63 Broadcast path failed 2048 checks on pinned Turnip; the same 263-mode production-decoder probe passes on both drivers after routing those reads through Shuffle too. This is a measured driver-path compatibility change, not a claim that constant Broadcast is forbidden by Vulkan. Dynamic quad permutations read four constant quad sources before selecting a result. The shuffle capability is declared only when emitted. Graphics pipelines request 64 for fragment shaders using lane/wave operations when the device advertises that stage and size; lack of this support does not constitute general guest wave emulation. Compute size checks now also check minimum size and stage support. Shader binary cache version is 15; metadata versions 10 (x86) / 11 (portable) persist lane/ballot flags so cached pipeline creation has the same requirements.

Previously recorded GPU probes report Qualcomm subgroup64 and pinned Turnip subgroup128. A new production-decoder probe covers the five bitmask patterns, all 256 quad permutations, and fixed lane31/63 reads. It separately marks inactive source lanes and demands substantial active coverage; inactive-lane results are not treated as defined values. Device tests and MHW acceptance are recorded in the validation document, not implied by this reference.

## Remaining semantic boundary

[AMD GCN3 ISA §12.7](https://gpuopen.com/download/AMD_GCN3_Instruction_Set_Architecture_rev1.1.pdf) defines zero for an invalid DS_SWIZZLE source thread. Vulkan subgroup source activity/helper handling is not a complete model of guest EXEC/WQM. This change corrects source routing and supported wave width; the probe validates active sources, not inactive-source zero behavior or full divergent guest-wave emulation. The actual SICI opcode is 53 (captured word 0xd8d4401f), unlike GCN3 opcode61; the probe uses captured/production SICI decoding.
