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

Id EmitQuadShuffle(EmitContext& ctx, Id value, const IR::Value& index) {
    const Id scope = SubgroupScope(ctx);
    if (index.IsImmediate()) {
        return ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], scope, value, ctx.Def(index));
    }
    // DS_SWIZZLE's quad permutation can select a different source in every lane.
    // QuadBroadcast requires an index uniform within the derivative group. Read
    // each constant source before selecting, preserving quad layout/helper rules.
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

Id EmitReadLane(EmitContext& ctx, Id value, const IR::Value& lane) {
    // ReadLane also represents DS_SWIZZLE bitmask routing. Its index is generally
    // lane-dependent, whereas Broadcast requires a subgroup-uniform source ID.
    ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
    return ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, ctx.Def(lane));
}

Id EmitWriteLane(EmitContext& ctx, Id value, Id write_value, u32 lane) {
    return ctx.u32_zero_value;
}

Id EmitBallot(EmitContext& ctx, Id bit) {
    return ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), bit);
}

Id EmitBallotFindLsb(EmitContext& ctx, Id mask) {
    return ctx.OpGroupNonUniformBallotFindLSB(ctx.U32[1], SubgroupScope(ctx), mask);
}

Id EmitGroupAny(EmitContext& ctx, Id bit) {
    return ctx.OpGroupNonUniformAny(ctx.U1[1], SubgroupScope(ctx), bit);
}

} // namespace Shader::Backend::SPIRV
