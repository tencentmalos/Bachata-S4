// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

Id SubgroupScope(EmitContext& ctx) {
    return ctx.ConstU32(static_cast<u32>(spv::Scope::Subgroup));
}

Id EmitWarpId(EmitContext& ctx) {
    UNREACHABLE();
}

Id EmitLaneId(EmitContext& ctx) {
    return ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
}

Id EmitQuadBroadcast(EmitContext& ctx, Id value, const IR::Value& index) {
    const Id scope = SubgroupScope(ctx);
    if (index.IsImmediate()) {
        return ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], scope, value, ctx.Def(index));
    }
    // A DS_SWIZZLE quad permutation in a fragment shader selects a different source in every
    // lane. QuadBroadcast requires an index uniform within the derivative group. Read each
    // constant source before selecting, preserving quad layout/helper invocation rules.
    const Id selector = ctx.Def(index);
    Id result = ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], scope, value, ctx.ConstU32(3u));
    for (u32 source = 0; source < 3; ++source) {
        const Id candidate =
            ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], scope, value, ctx.ConstU32(source));
        const Id selected = ctx.OpIEqual(ctx.U1[1], selector, ctx.ConstU32(source));
        result = ctx.OpSelect(ctx.U32[1], selected, candidate, result);
    }
    return result;
}

Id EmitReadFirstLane(EmitContext& ctx, Id value) {
    return ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], SubgroupScope(ctx), value);
}

Id EmitShuffle(EmitContext& ctx, Id value, Id index) {
    return ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, index);
}

Id EmitShuffleXor(EmitContext& ctx, Id value, Id mask) {
    // Info collection only flags plain Shuffle; ShuffleXor needs the same capability.
    ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
    return ctx.OpGroupNonUniformShuffleXor(ctx.U32[1], SubgroupScope(ctx), value, mask);
}

Id EmitReadLane(EmitContext& ctx, Id value, Id lane) {
    // V_READLANE_B32's lane is subgroup-uniform, but fragment probes on the pinned Turnip
    // failed with OpGroupNonUniformBroadcast from constant lanes 31/63 while Shuffle passed
    // on Turnip and the Qualcomm driver. Shuffle is valid for uniform and per-lane indices.
    ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
    return ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, lane);
}

Id EmitWriteLane(EmitContext& ctx, Id value, Id write_value, u32 lane) {
    return ctx.u32_zero_value;
}

Id EmitBallot(EmitContext& ctx, Id bit) {
    const Id ballot{ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), bit)};
    // U64 is a {lo, hi} u32 pair when shaderInt64 is unavailable; PackU64 handles both.
    return ctx.PackU64(ctx.OpVectorShuffle(ctx.U32[2], ballot, ballot, 0, 1));
}

Id EmitBallotFindLsb(EmitContext& ctx, Id mask) {
    const Id value{ctx.OpCompositeConstruct(ctx.U32[4], ctx.UnpackU64(mask),
                                            ctx.u32_zero_value, ctx.u32_zero_value)};
    return ctx.OpGroupNonUniformBallotFindLSB(ctx.U32[1], SubgroupScope(ctx), value);
}

Id EmitInverseBallot(EmitContext& ctx, Id mask) {
    const Id value{ctx.OpCompositeConstruct(ctx.U32[4], ctx.UnpackU64(mask),
                                            ctx.u32_zero_value, ctx.u32_zero_value)};
    return ctx.OpGroupNonUniformInverseBallot(ctx.U1[1], SubgroupScope(ctx), value);
}

Id EmitGroupAny(EmitContext& ctx, Id bit) {
    return ctx.OpGroupNonUniformAny(ctx.U1[1], SubgroupScope(ctx), bit);
}

} // namespace Shader::Backend::SPIRV
