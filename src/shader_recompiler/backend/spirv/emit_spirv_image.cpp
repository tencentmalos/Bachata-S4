// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/static_vector.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/microinstruction.h"
#include "shader_recompiler/ir/passes/resource_pass.h"

namespace Shader::Backend::SPIRV {

namespace {

bool SupportsScale(const EmitContext& ctx, u32 handle) {
    const auto& texture = ctx.images[handle & 0xffff];
    return ctx.profile.internal_scale && !texture.is_storage &&
        texture.scale_binding < PushData::MaxScaledBinding &&
        (texture.view_type == AmdGpu::ImageType::Color2D ||
         texture.view_type == AmdGpu::ImageType::Color2DArray ||
         texture.view_type == AmdGpu::ImageType::Cube);
}

Id SharpWord(EmitContext& ctx, u32 handle, u32 word) {
    const auto& fetch = ctx.info.images[handle & 0xffff].sharp_fetch;
    if ((fetch.load_mask & (1u << word)) && fetch.offsets[word] != UNKNOWN_LOCATION)
        return ctx.EmitFlatbufferLoad(ctx.ConstU32(u32(fetch.offsets[word])));
    return ctx.ConstU32(fetch.immediates[word]);
}

Id GuestDimensions(EmitContext& ctx, u32 handle, Id lod) {
    const Id sizes = SharpWord(ctx, handle, 2);
    const Id levels = SharpWord(ctx, handle, 3);
    const Id base = ctx.OpBitFieldUExtract(ctx.U32[1], levels, ctx.ConstU32(12u), ctx.ConstU32(4u));
    const Id level = ctx.OpUMin(ctx.U32[1],
        ctx.OpIAdd(ctx.U32[1], base, lod), ctx.ConstU32(31u));
    const auto dimension = [&](u32 shift) {
        const Id size = ctx.OpIAdd(ctx.U32[1], ctx.ConstU32(1u),
            ctx.OpBitFieldUExtract(ctx.U32[1], sizes, ctx.ConstU32(shift), ctx.ConstU32(14u)));
        return ctx.OpUMax(ctx.U32[1], ctx.ConstU32(1u),
                         ctx.OpShiftRightLogical(ctx.U32[1], size, level));
    };
    return ctx.OpCompositeConstruct(ctx.U32[2], dimension(0), dimension(14));
}

Id PhysicalLod(EmitContext& ctx, u32 handle, Id lod, bool floating) {
    if (!SupportsScale(ctx, handle)) return lod;
    const Id code = ctx.ImageScaleCode(ctx.images[handle & 0xffff].scale_binding);
    const Id drop = ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1[1], code, ctx.ConstU32(3u)),
        ctx.ConstU32(2u), ctx.OpSelect(ctx.U32[1],
            ctx.OpIEqual(ctx.U1[1], code, ctx.ConstU32(1u)), ctx.ConstU32(1u), ctx.u32_zero_value));
    if (floating) {
        const Id adjusted = ctx.OpFMax(ctx.F32[1], ctx.OpFSub(ctx.F32[1], lod,
            ctx.OpConvertUToF(ctx.F32[1], drop)), ctx.f32_zero_value);
        return ctx.OpSelect(ctx.F32[1], ctx.OpINotEqual(ctx.U1[1], drop, ctx.u32_zero_value),
                            adjusted, lod);
    }
    return ctx.OpISub(ctx.U32[1], ctx.OpUMax(ctx.U32[1], lod, drop), drop);
}

} // namespace

template <bool is_float>
static Id FixImageCoords(EmitContext& ctx, Id coords, AmdGpu::ImageType image_type) {
    const auto coord_type = is_float ? ctx.F32 : ctx.U32;
    const auto zero = is_float ? ctx.f32_zero_value : ctx.u32_zero_value;

    switch (image_type) {
    case AmdGpu::ImageType::Color1D: {
        // Lowered to 2D with height 1
        const auto x = coords;
        return ctx.OpCompositeConstruct(coord_type[2], x, zero);
    }
    case AmdGpu::ImageType::Color1DArray: {
        // Lowered to 2D array with height 1
        const auto x = ctx.OpCompositeExtract(coord_type[1], coords, 0U);
        const auto slice = ctx.OpCompositeExtract(coord_type[1], coords, 1U);
        return ctx.OpCompositeConstruct(coord_type[3], x, zero, slice);
    }
    case AmdGpu::ImageType::Cube: {
        // Lowered to 2D array
        if (is_float) {
            const auto x = ctx.OpCompositeExtract(coord_type[1], coords, 0U);
            const auto y = ctx.OpCompositeExtract(coord_type[1], coords, 1U);
            const auto face = ctx.OpCompositeExtract(coord_type[1], coords, 2U);

            // AMD cube math results in coordinates in the range [1.0, 2.0]. We need
            // to convert this to the range [0.0, 1.0] to get correct results.
            const auto one = ctx.ConstF32(1.f);
            const auto fixed_x = ctx.OpFSub(coord_type[1], x, one);
            const auto fixed_y = ctx.OpFSub(coord_type[1], y, one);
            const auto fixed_face = ctx.OpFma(
                coord_type[1],
                ctx.OpFloor(coord_type[1], ctx.OpFDiv(coord_type[1], face, ctx.ConstF32(8.f))),
                ctx.ConstF32(-2.f), face);
            return ctx.OpCompositeConstruct(coord_type[3], fixed_x, fixed_y, fixed_face);
        }
        return coords;
    }
    default:
        return coords;
    }
}

Id FixImageTexelCoords(EmitContext& ctx, u32 handle, Id coords) {
    return FixImageCoords<false>(ctx, coords, ctx.images[handle & 0xFFFF].view_type);
}

static bool IsImage1D(AmdGpu::ImageType image_type) {
    return image_type == AmdGpu::ImageType::Color1D ||
           image_type == AmdGpu::ImageType::Color1DArray;
}

template <bool is_float>
static Id PadImageOperand(EmitContext& ctx, Id operand, AmdGpu::ImageType image_type) {
    if (IsImage1D(image_type)) {
        const auto coord_type = is_float ? ctx.F32 : ctx.U32;
        const auto zero = is_float ? ctx.f32_zero_value : ctx.u32_zero_value;
        return ctx.OpCompositeConstruct(coord_type[2], operand, zero);
    }
    return operand;
}

struct ImageOperands {
    void Add(spv::ImageOperandsMask new_mask, Id value) {
        if (!Sirit::ValidId(value)) {
            return;
        }
        mask = static_cast<spv::ImageOperandsMask>(static_cast<u32>(mask) |
                                                   static_cast<u32>(new_mask));
        operands.push_back(value);
    }
    void Add(spv::ImageOperandsMask new_mask, Id value1, Id value2) {
        mask = static_cast<spv::ImageOperandsMask>(static_cast<u32>(mask) |
                                                   static_cast<u32>(new_mask));
        operands.push_back(value1);
        operands.push_back(value2);
    }

    void AddOffset(EmitContext& ctx, AmdGpu::ImageType image_type, const IR::Value& offset,
                   bool can_use_runtime_offsets = false) {
        if (offset.IsEmpty()) {
            return;
        }
        spv::ImageOperandsMask op_mask{};
        Id value{};
        if (offset.IsImmediate()) {
            // A ConstOffset operand must be a constant of one integer type, so a 1D offset is
            // padded here instead of by PadImageOperand.
            const s32 x = static_cast<s32>(offset.U32());
            op_mask = spv::ImageOperandsMask::ConstOffset;
            value = IsImage1D(image_type) ? ctx.ConstS32(x, 0) : ctx.ConstS32(x);
            Add(op_mask, value);
            return;
        } else {
            IR::Inst* const inst{offset.Inst()};
            if (inst->AreAllArgsImmediates()) {
                switch (inst->GetOpcode()) {
                case IR::Opcode::CompositeConstructU32x2:
                    op_mask = spv::ImageOperandsMask::ConstOffset;
                    value = ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                         static_cast<s32>(inst->Arg(1).U32()));
                    break;
                case IR::Opcode::CompositeConstructU32x3:
                    op_mask = spv::ImageOperandsMask::ConstOffset;
                    value = ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                         static_cast<s32>(inst->Arg(1).U32()),
                                         static_cast<s32>(inst->Arg(2).U32()));
                    break;
                default:
                    break;
                }
            }
        }

        if (op_mask != spv::ImageOperandsMask::ConstOffset) {
            if (!can_use_runtime_offsets) {
                LOG_WARNING(Render_Vulkan,
                            "Runtime offset provided to unsupported image sample instruction");
                return;
            }
            op_mask = spv::ImageOperandsMask::Offset;
            value = PadImageOperand<false>(ctx, ctx.Def(offset), image_type);
        }
        Add(op_mask, value);
    }

    void AddDerivatives(EmitContext& ctx, AmdGpu::ImageType image_type, Id derivatives_dx,
                        Id derivatives_dy) {
        if (!Sirit::ValidId(derivatives_dx) || !Sirit::ValidId(derivatives_dy)) {
            return;
        }
        derivatives_dx = PadImageOperand<true>(ctx, derivatives_dx, image_type);
        derivatives_dy = PadImageOperand<true>(ctx, derivatives_dy, image_type);
        Add(spv::ImageOperandsMask::Grad, derivatives_dx, derivatives_dy);
    }

    spv::ImageOperandsMask mask{};
    boost::container::static_vector<Id, 4> operands;
};

Id EmitGuardedImageSample(EmitContext& ctx, IR::Inst* inst) {
    // Selecting a dynamic T# introduces per-lane divergence. Compute derivatives
    // at the original sample site, before that selection, while the quad is intact.
    // Leave the actual texture access guarded (other candidates can be incompatible).
    ASSERT(ctx.info.sw_stage == SwStage::Fragment);
    const auto* guard = inst->Arg(0).Inst();
    const u32 handle = Optimization::ResourceBinding(inst->Arg(0));
    const auto& texture = ctx.images[handle & 0xffff];
    const Id coords = FixImageCoords<true>(ctx, ctx.Def(inst->Arg(1)), texture.view_type);
    const bool dref = inst->GetOpcode() == IR::Opcode::ImageSampleDrefImplicitLod;
    const bool volume = texture.view_type == AmdGpu::ImageType::Color3D;
    // Cube is already lowered to a 2D array. Array layer / cube face must never
    // contribute to the footprint, even when neighboring lanes use different layers.
    const bool array = texture.view_type == AmdGpu::ImageType::Color1DArray ||
                       texture.view_type == AmdGpu::ImageType::Color2DArray ||
                       texture.view_type == AmdGpu::ImageType::Cube;
    const Id gradient_coords =
        array ? ctx.OpVectorShuffle(ctx.F32[2], coords, coords, 0U, 1U) : coords;
    const Id gradient_type = ctx.F32[volume ? 3 : 2];
    Id dx = ctx.OpDPdx(gradient_type, gradient_coords);
    Id dy = ctx.OpDPdy(gradient_type, gradient_coords);
    const auto& bias = inst->Arg(dref ? 3 : 2);
    if (!bias.IsEmpty()) {
        // A one-level bias doubles the footprint. Sampler bias remains applied
        // by the texture unit; scaling gradients preserves anisotropic sampling.
        const Id scale = ctx.OpExp2(ctx.F32[1], ctx.Def(bias));
        dx = ctx.OpVectorTimesScalar(gradient_type, dx, scale);
        dy = ctx.OpVectorTimesScalar(gradient_type, dy, scale);
    }
    const Id before = ctx.last_label;
    const Id body = ctx.OpLabel();
    const Id merge = ctx.OpLabel();
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(ctx.Def(guard->Arg(1)), body, merge);
    ctx.AddLabel(body);
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    // Coordinates are already lowered, including 1D padding.
    operands.Add(spv::ImageOperandsMask::Grad, dx, dy);
    operands.AddOffset(ctx, texture.view_type, inst->Arg(dref ? 4 : 3));
    const Id type = texture.data_types->Get(dref ? 1 : 4);
    Id value = dref ? ctx.OpImageSampleDrefExplicitLod(type, sampled, coords, ctx.Def(inst->Arg(2)),
                                                       operands.mask, operands.operands)
                    : ctx.OpImageSampleExplicitLod(type, sampled, coords, operands.mask,
                                                   operands.operands);
    if (texture.is_integer)
        value = ctx.OpBitcast(ctx.F32[dref ? 1 : 4], value);
    if (dref)
        value = ctx.OpCompositeConstruct(ctx.F32[4], value, ctx.f32_zero_value, ctx.f32_zero_value,
                                         ctx.f32_zero_value);
    const Id after = ctx.last_label;
    ctx.OpBranch(merge);
    ctx.AddLabel(merge);
    return ctx.OpPhi(ctx.F32[4], ctx.ConstantNull(ctx.F32[4]), before, value, after);
}

Id EmitGuardedResource(EmitContext& ctx, u32 binding, Id) {
    // The consumer emits the selection branch. Keep the condition as an SSA dependency.
    return ctx.ConstU32(binding);
}

Id EmitImageHandle(EmitContext& ctx, Id, Id) {
    UNREACHABLE_MSG("Unreachable instruction");
}

Id EmitImageSampleRaw(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address1, Id address2,
                      Id address3, Id address4) {
    UNREACHABLE_MSG("Unreachable instruction");
}

Id EmitImageSampleImplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id bias,
                              const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    operands.AddOffset(ctx, texture.view_type, offset);
    const Id sample = ctx.OpImageSampleImplicitLod(result_type, sampled_image, fixed_coords,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageSampleExplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod,
                              const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, PhysicalLod(ctx, handle, lod, true));
    operands.AddOffset(ctx, texture.view_type, offset);
    const Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, fixed_coords,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageSampleDrefImplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id dref,
                                  Id bias, const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(1);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    operands.AddOffset(ctx, texture.view_type, offset);
    const Id sample = ctx.OpImageSampleDrefImplicitLod(result_type, sampled_image, fixed_coords,
                                                       dref, operands.mask, operands.operands);
    const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
    return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, ctx.f32_zero_value,
                                    ctx.f32_zero_value, ctx.f32_zero_value);
}

Id EmitImageSampleDrefExplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id dref,
                                  Id lod, const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(1);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, PhysicalLod(ctx, handle, lod, true));
    operands.AddOffset(ctx, texture.view_type, offset);
    const Id sample = ctx.OpImageSampleDrefExplicitLod(result_type, sampled_image, fixed_coords,
                                                       dref, operands.mask, operands.operands);
    const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
    return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, ctx.f32_zero_value,
                                    ctx.f32_zero_value, ctx.f32_zero_value);
}

Id EmitImageGather(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                   const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    const u32 comp = inst->Flags<IR::TextureInstInfo>().gather_comp.Value();
    ImageOperands operands;
    operands.AddOffset(ctx, texture.view_type, offset, true);
    const Id texels = ctx.OpImageGather(result_type, sampled_image, fixed_coords,
                                        ctx.ConstU32(comp), operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageGatherDref(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                       const IR::Value& offset, Id dref) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.AddOffset(ctx, texture.view_type, offset, true);
    const Id texels = ctx.OpImageDrefGather(result_type, sampled_image, fixed_coords, dref,
                                            operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageQueryDimensions(EmitContext& ctx, IR::Inst* inst, u32 handle, Id lod, bool has_mips) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id zero = ctx.u32_zero_value;
    const bool multisampled = texture.view_type == AmdGpu::ImageType::Color2DMsaa ||
                              texture.view_type == AmdGpu::ImageType::Color2DMsaaArray;
    const auto mips{[&] { return !has_mips ? zero : multisampled ? ctx.ConstU32(1u)
                                                      : ctx.OpImageQueryLevels(ctx.U32[1], image); }};
    const bool uses_lod{(!multisampled || ctx.profile.force_disable_msaa) && !texture.is_storage};
    if (multisampled && ctx.profile.force_disable_msaa) lod = zero;
    const auto query{[&](Id type) {
        Id physical_lod = lod;
        if (uses_lod && SupportsScale(ctx, handle)) {
            const Id clamped = ctx.OpUMin(ctx.U32[1], PhysicalLod(ctx, handle, lod, false),
                ctx.OpISub(ctx.U32[1], ctx.OpImageQueryLevels(ctx.U32[1], image), ctx.ConstU32(1u)));
            physical_lod = ctx.OpSelect(ctx.U32[1],
                ctx.OpINotEqual(ctx.U1[1], ctx.ImageScaleCode(texture.scale_binding), zero), clamped, lod);
        }
        return uses_lod ? ctx.OpImageQuerySizeLod(type, image, physical_lod)
                        : ctx.OpImageQuerySize(type, image);
    }};
    const Id original = [&]() -> Id {
    switch (texture.view_type) {
    case AmdGpu::ImageType::Color1D: {
        const auto width = ctx.OpCompositeExtract(ctx.U32[1], query(ctx.U32[2]), 0U);
        return ctx.OpCompositeConstruct(ctx.U32[4], width, zero, zero, mips());
    }
    case AmdGpu::ImageType::Color1DArray: {
        const auto base = query(ctx.U32[3]);
        const auto width = ctx.OpCompositeExtract(ctx.U32[1], base, 0U);
        const auto slices = ctx.OpCompositeExtract(ctx.U32[1], base, 2U);
        return ctx.OpCompositeConstruct(ctx.U32[4], width, slices, zero, mips());
    }
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[2]), zero, mips());
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaaArray:
    case AmdGpu::ImageType::Cube:
    case AmdGpu::ImageType::Color3D:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[3]), mips());
    default:
        UNREACHABLE_MSG("SPIR-V Instruction");
    }
    }();
    if (!SupportsScale(ctx, handle)) return original;
    const Id sizes = GuestDimensions(ctx, handle, lod);
    const Id levels = SharpWord(ctx, handle, 3);
    const Id base = ctx.OpBitFieldUExtract(ctx.U32[1], levels, ctx.ConstU32(12u), ctx.ConstU32(4u));
    const Id last = ctx.OpBitFieldUExtract(ctx.U32[1], levels, ctx.ConstU32(16u), ctx.ConstU32(4u));
    const Id count = has_mips ? ctx.OpIAdd(ctx.U32[1], ctx.ConstU32(1u),
        ctx.OpISub(ctx.U32[1], ctx.OpUMax(ctx.U32[1], last, base), base)) : zero;
    const Id logical = ctx.OpCompositeConstruct(ctx.U32[4], sizes,
        ctx.OpCompositeExtract(ctx.U32[1], original, 2), count);
    const Id scaled = ctx.OpINotEqual(ctx.U1[1], ctx.ImageScaleCode(texture.scale_binding), zero);
    return ctx.OpSelect(ctx.U32[4], ctx.OpCompositeConstruct(ctx.TypeVector(ctx.U1[1], 4), scaled, scaled, scaled, scaled),
                        logical, original);
}

Id EmitImageQueryLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    return ctx.OpImageQueryLod(ctx.F32[2], sampled_image, fixed_coords);
}

Id EmitImageGradient(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id derivatives_dx,
                     Id derivatives_dy, const IR::Value& offset, const IR::Value& lod_clamp) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.AddDerivatives(ctx, texture.view_type, derivatives_dx, derivatives_dy);
    operands.AddOffset(ctx, texture.view_type, offset);
    const Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, fixed_coords,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageRead(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id color_type = texture.data_types->Get(4);
    // Lower first: the render-scale remap below rewrites the lowered coordinates.
    coords = FixImageCoords<false>(ctx, coords, texture.view_type);
    ImageOperands operands;
    Id texel;
    if (!texture.is_storage) {
        const Id image = ctx.OpLoad(texture.image_type, texture.id);
        if (SupportsScale(ctx, handle)) {
            const Id code = ctx.ImageScaleCode(texture.scale_binding);
            const Id scaled = ctx.OpINotEqual(ctx.U1[1], code, ctx.u32_zero_value);
            const Id logical_lod = Sirit::ValidId(lod) ? lod : ctx.u32_zero_value;
            const Id host_lod = ctx.OpUMin(ctx.U32[1], PhysicalLod(ctx, handle, logical_lod, false),
                ctx.OpISub(ctx.U32[1], ctx.OpImageQueryLevels(ctx.U32[1], image), ctx.ConstU32(1u)));
            const Id guest_size = GuestDimensions(ctx, handle, logical_lod);
            const bool array = texture.view_type == AmdGpu::ImageType::Color2DArray ||
                               texture.view_type == AmdGpu::ImageType::Cube;
            const Id host_size = ctx.OpImageQuerySizeLod(array ? ctx.U32[3] : ctx.U32[2], image, host_lod);
            const auto coordinate = [&](u32 component) {
                const Id original = ctx.OpCompositeExtract(ctx.U32[1], coords, component);
                const Id guest = ctx.OpConvertUToF(ctx.F32[1],
                    ctx.OpCompositeExtract(ctx.U32[1], guest_size, component));
                const Id host = ctx.OpConvertUToF(ctx.F32[1],
                    ctx.OpCompositeExtract(ctx.U32[1], host_size, component));
                const Id value = ctx.OpConvertSToF(ctx.F32[1], ctx.OpBitcast(ctx.S32[1], original));
                const Id mapped = ctx.OpConvertFToS(ctx.S32[1], ctx.OpFloor(ctx.F32[1],
                    ctx.OpFMul(ctx.F32[1], ctx.OpFAdd(ctx.F32[1], value, ctx.ConstF32(0.5f)),
                               ctx.OpFDiv(ctx.F32[1], host, guest))));
                return ctx.OpSelect(ctx.U32[1], scaled, ctx.OpBitcast(ctx.U32[1], mapped), original);
            };
            coords = array ? ctx.OpCompositeConstruct(ctx.U32[3], coordinate(0), coordinate(1),
                                ctx.OpCompositeExtract(ctx.U32[1], coords, 2))
                           : ctx.OpCompositeConstruct(ctx.U32[2], coordinate(0), coordinate(1));
            lod = ctx.OpSelect(ctx.U32[1], scaled, host_lod, logical_lod);
        }
        if (texture.view_type == AmdGpu::ImageType::Color2DMsaa ||
            texture.view_type == AmdGpu::ImageType::Color2DMsaaArray) {
            // Off mode collapses all logical samples to the sole physical texel.
            // A non-MS OpTypeImage must never carry a Sample operand.
            if (ctx.profile.force_disable_msaa) {
                operands.Add(spv::ImageOperandsMask::Lod, ctx.u32_zero_value);
            } else if (Sirit::ValidId(ms)) {
                // GCN hardware wraps out-of-range MSAA sample indices.
                const Id sample_count = ctx.OpImageQuerySamples(ctx.U32[1], image);
                const Id wrapped_ms = ctx.OpUMod(ctx.U32[1], ms, sample_count);
                operands.Add(spv::ImageOperandsMask::Sample, wrapped_ms);
            }
        } else {
            if (Sirit::ValidId(ms)) {
                LOG_ERROR(Render_Recompiler, "image is not MS but ms operand is provided");
            }
            operands.Add(spv::ImageOperandsMask::Lod, lod);
            operands.Add(spv::ImageOperandsMask::Sample, ms);
        }
        texel = ctx.OpImageFetch(color_type, image, coords, operands.mask, operands.operands);
    } else {
        Id image_ptr = texture.id;
        if (ctx.profile.supports_image_load_store_lod) {
            operands.Add(spv::ImageOperandsMask::Lod, lod);
        } else if (Sirit::ValidId(lod)) {
#if 1
            // It's confusing what interactions will cause this code path so leave it as
            // unreachable until a case is found.
            // Normally IMAGE_LOAD_MIP should translate -> OpImageFetch
            UNREACHABLE_MSG("Unsupported ImageRead with Lod");
#else
            LOG_WARNING(Render, "Fallback for ImageRead with LOD");
            ASSERT(texture.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex);
            const Id single_image_ptr_type =
                ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type);
            image_ptr = ctx.OpAccessChain(single_image_ptr_type, image_ptr, std::array{lod});
#endif
        }
        const Id image = ctx.OpLoad(texture.image_type, image_ptr);
        if (!ctx.profile.force_disable_msaa && Sirit::ValidId(ms)) {
            operands.Add(spv::ImageOperandsMask::Sample,
                         ctx.OpUMod(ctx.U32[1], ms, ctx.OpImageQuerySamples(ctx.U32[1], image)));
        }
        texel = ctx.OpImageRead(color_type, image, coords, operands.mask, operands.operands);
    }
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texel) : texel;
}

void EmitImageWrite(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms,
                    Id color) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    Id image_ptr = texture.id;
    const Id color_type = texture.data_types->Get(4);
    const Id fixed_coords = FixImageCoords<false>(ctx, coords, texture.view_type);
    ImageOperands operands;
    if (!ctx.profile.force_disable_msaa) operands.Add(spv::ImageOperandsMask::Sample, ms);
    if (ctx.profile.supports_image_load_store_lod) {
        operands.Add(spv::ImageOperandsMask::Lod, lod);
    } else if (Sirit::ValidId(lod)) {
        LOG_WARNING(Render, "Fallback for ImageWrite with LOD");
        ASSERT(texture.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex);
        const Id single_image_ptr_type =
            ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type);
        image_ptr = ctx.OpAccessChain(single_image_ptr_type, image_ptr, std::array{lod});
    }
    const Id image = ctx.OpLoad(texture.image_type, image_ptr);
    const Id texel = texture.is_integer ? ctx.OpBitcast(color_type, color) : color;
    ctx.OpImageWrite(image, fixed_coords, texel, operands.mask, operands.operands);
}

static Id SelectCubeResult(EmitContext& ctx, Id x, Id y, Id z, Id x_res, Id y_res, Id z_res) {
    const auto abs_x = ctx.OpFAbs(ctx.F32[1], x);
    const auto abs_y = ctx.OpFAbs(ctx.F32[1], y);
    const auto abs_z = ctx.OpFAbs(ctx.F32[1], z);

    const auto z_face_cond{ctx.OpLogicalAnd(ctx.U1[1],
                                            ctx.OpFOrdGreaterThanEqual(ctx.U1[1], abs_z, abs_x),
                                            ctx.OpFOrdGreaterThanEqual(ctx.U1[1], abs_z, abs_y))};
    const auto y_face_cond{ctx.OpFOrdGreaterThanEqual(ctx.U1[1], abs_y, abs_x)};

    return ctx.OpSelect(ctx.F32[1], z_face_cond, z_res,
                        ctx.OpSelect(ctx.F32[1], y_face_cond, y_res, x_res));
}

Id EmitCubeFaceIndex(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    if (ctx.profile.supports_native_cube_calc) {
        return ctx.OpCubeFaceIndexAMD(ctx.F32[1], ctx.OpCompositeConstruct(ctx.F32[3], x, y, z));
    }

    const auto x_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], x, ctx.f32_zero_value)};
    const auto y_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], y, ctx.f32_zero_value)};
    const auto z_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], z, ctx.f32_zero_value)};
    const auto x_face{ctx.OpSelect(ctx.F32[1], x_neg_cond, ctx.ConstF32(1.f), ctx.ConstF32(0.f))};
    const auto y_face{ctx.OpSelect(ctx.F32[1], y_neg_cond, ctx.ConstF32(3.f), ctx.ConstF32(2.f))};
    const auto z_face{ctx.OpSelect(ctx.F32[1], z_neg_cond, ctx.ConstF32(5.f), ctx.ConstF32(4.f))};

    return SelectCubeResult(ctx, x, y, z, x_face, y_face, z_face);
}

Id EmitCubeFaceCoordS(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    const auto x_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], x, ctx.f32_zero_value)};
    const auto z_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], z, ctx.f32_zero_value)};
    const auto x_sc{ctx.OpSelect(ctx.F32[1], x_neg_cond, z, ctx.OpFNegate(ctx.F32[1], z))};
    const auto y_sc{x};
    const auto z_sc{ctx.OpSelect(ctx.F32[1], z_neg_cond, ctx.OpFNegate(ctx.F32[1], x), x)};

    return SelectCubeResult(ctx, x, y, z, x_sc, y_sc, z_sc);
}

Id EmitCubeFaceCoordT(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    const auto y_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], y, ctx.f32_zero_value)};
    const auto x_z_tc{ctx.OpFNegate(ctx.F32[1], y)};
    const auto y_tc{ctx.OpSelect(ctx.F32[1], y_neg_cond, ctx.OpFNegate(ctx.F32[1], z), z)};

    return SelectCubeResult(ctx, x, y, z, x_z_tc, y_tc, x_z_tc);
}

Id EmitCubeFaceMajorAxis(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    const auto two{ctx.ConstF32(2.f)};
    const auto x_major_axis{ctx.OpFMul(ctx.F32[1], x, two)};
    const auto y_major_axis{ctx.OpFMul(ctx.F32[1], y, two)};
    const auto z_major_axis{ctx.OpFMul(ctx.F32[1], z, two)};

    return SelectCubeResult(ctx, x, y, z, x_major_axis, y_major_axis, z_major_axis);
}

} // namespace Shader::Backend::SPIRV
