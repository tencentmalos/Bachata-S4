// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/static_vector.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/microinstruction.h"

namespace Shader::Backend::SPIRV {

namespace {

bool SupportsScale(const EmitContext& ctx, u32 handle) {
    const auto& texture = ctx.images[handle & 0xffff];
    return ctx.profile.internal_scale && !texture.is_storage &&
        texture.scale_binding < PushData::MaxScaledBinding &&
        (texture.view_type == AmdGpu::ImageType::Color2D ||
         texture.view_type == AmdGpu::ImageType::Color2DArray);
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
    const Id dropped = ctx.OpIEqual(ctx.U1[1], code, ctx.ConstU32(1u));
    if (floating) {
        return ctx.OpSelect(ctx.F32[1], dropped,
            ctx.OpFMax(ctx.F32[1], ctx.OpFSub(ctx.F32[1], lod, ctx.ConstF32(1.f)),
                       ctx.f32_zero_value), lod);
    }
    return ctx.OpSelect(ctx.U32[1], dropped,
        ctx.OpISub(ctx.U32[1], ctx.OpUMax(ctx.U32[1], lod, ctx.ConstU32(1u)),
                   ctx.ConstU32(1u)), lod);
}

} // namespace

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

    void AddOffset(EmitContext& ctx, const IR::Value& offset,
                   bool can_use_runtime_offsets = false) {
        if (offset.IsEmpty()) {
            return;
        }
        if (offset.IsImmediate()) {
            const s32 operand = offset.U32();
            Add(spv::ImageOperandsMask::ConstOffset, ctx.ConstS32(operand));
            return;
        }
        IR::Inst* const inst{offset.Inst()};
        if (inst->AreAllArgsImmediates()) {
            switch (inst->GetOpcode()) {
            case IR::Opcode::CompositeConstructU32x2:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                 static_cast<s32>(inst->Arg(1).U32())));
                return;
            case IR::Opcode::CompositeConstructU32x3:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                 static_cast<s32>(inst->Arg(1).U32()),
                                 static_cast<s32>(inst->Arg(2).U32())));
                return;
            default:
                break;
            }
        }
        if (can_use_runtime_offsets) {
            Add(spv::ImageOperandsMask::Offset, ctx.Def(offset));
        } else {
            LOG_WARNING(Render_Vulkan,
                        "Runtime offset provided to unsupported image sample instruction");
        }
    }

    void AddDerivatives(EmitContext& ctx, Id derivatives_dx, Id derivatives_dy) {
        if (!Sirit::ValidId(derivatives_dx) || !Sirit::ValidId(derivatives_dy)) {
            return;
        }
        Add(spv::ImageOperandsMask::Grad, derivatives_dx, derivatives_dy);
    }

    spv::ImageOperandsMask mask{};
    boost::container::static_vector<Id, 4> operands;
};

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
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    operands.AddOffset(ctx, offset);
    const Id sample = ctx.OpImageSampleImplicitLod(result_type, sampled_image, coords,
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
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, PhysicalLod(ctx, handle, lod, true));
    operands.AddOffset(ctx, offset);
    const Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, coords,
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
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    operands.AddOffset(ctx, offset);
    const Id sample = ctx.OpImageSampleDrefImplicitLod(result_type, sampled_image, coords, dref,
                                                       operands.mask, operands.operands);
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
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, PhysicalLod(ctx, handle, lod, true));
    operands.AddOffset(ctx, offset);
    const Id sample = ctx.OpImageSampleDrefExplicitLod(result_type, sampled_image, coords, dref,
                                                       operands.mask, operands.operands);
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
    const u32 comp = inst->Flags<IR::TextureInstInfo>().gather_comp.Value();
    ImageOperands operands;
    operands.AddOffset(ctx, offset, true);
    const Id texels = ctx.OpImageGather(result_type, sampled_image, coords, ctx.ConstU32(comp),
                                        operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageGatherDref(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                       const IR::Value& offset, Id dref) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.AddOffset(ctx, offset, true);
    const Id texels = ctx.OpImageDrefGather(result_type, sampled_image, coords, dref, operands.mask,
                                            operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageQueryDimensions(EmitContext& ctx, IR::Inst* inst, u32 handle, Id lod, bool has_mips) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const auto sharp = ctx.info.images[handle & 0xFFFF].GetSharp(ctx.info);
    const Id zero = ctx.u32_zero_value;
    const auto mips{[&] { return has_mips ? ctx.OpImageQueryLevels(ctx.U32[1], image) : zero; }};
    const bool uses_lod{texture.view_type != AmdGpu::ImageType::Color2DMsaa && !texture.is_storage};
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
    case AmdGpu::ImageType::Color1D:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[1]), zero, zero, mips());
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[2]), zero, mips());
    case AmdGpu::ImageType::Color2DArray:
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
    const Id zero{ctx.f32_zero_value};
    return ctx.OpImageQueryLod(ctx.F32[2], sampled_image, coords);
}

Id EmitImageGradient(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id derivatives_dx,
                     Id derivatives_dy, const IR::Value& offset, const IR::Value& lod_clamp) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.AddDerivatives(ctx, derivatives_dx, derivatives_dy);
    operands.AddOffset(ctx, offset);
    const Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, coords,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageRead(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id color_type = texture.data_types->Get(4);
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
            const bool array = texture.view_type == AmdGpu::ImageType::Color2DArray;
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
        if (texture.view_type == AmdGpu::ImageType::Color2DMsaa) {
            // GCN hardware wraps out-of-range MSAA sample indices
            if (Sirit::ValidId(ms)) {
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
        texel = ctx.OpImageRead(color_type, image, coords, operands.mask, operands.operands);
    }
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texel) : texel;
}

void EmitImageWrite(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms,
                    Id color) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    Id image_ptr = texture.id;
    const Id color_type = texture.data_types->Get(4);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Sample, ms);
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
    ctx.OpImageWrite(image, coords, texel, operands.mask, operands.operands);
}

Id EmitCubeFaceIndex(EmitContext& ctx, IR::Inst* inst, Id cube_coords) {
    if (ctx.profile.supports_native_cube_calc) {
        return ctx.OpCubeFaceIndexAMD(ctx.F32[1], cube_coords);
    } else {
        UNREACHABLE_MSG("SPIR-V Instruction");
    }
}

} // namespace Shader::Backend::SPIRV
