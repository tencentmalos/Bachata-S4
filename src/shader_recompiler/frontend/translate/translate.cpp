// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/io_file.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/condition.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/reinterpret.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/resource.h"

#include <algorithm>
#include <array>
#include <bit>
#include <numbers>
#include <optional>
#include <magic_enum/magic_enum.hpp>

namespace Shader::Gcn {

static IR::VectorReg IterateBarycentrics(const RuntimeInfo& runtime_info, auto&& set_attribute) {
    if (runtime_info.hw_stage != HwStage::Fragment) {
        return IR::VectorReg::V0;
    }
    u32 dst_vreg{};
    const auto addr_flags = runtime_info.hw.fs.addr_flags;
    if (addr_flags.persp_sample_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmoothSample, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmoothSample, 1); // J
    }
    if (addr_flags.persp_center_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmooth, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmooth, 1); // J
    }
    if (addr_flags.persp_centroid_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmoothCentroid, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmoothCentroid, 1); // J
    }
    if (addr_flags.persp_pull_model_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordPullModel, 0); // I/W
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordPullModel, 1); // J/W
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordPullModel, 2); // 1/W
    }
    if (addr_flags.linear_sample_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPerspSample, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPerspSample, 1); // J
    }
    if (addr_flags.linear_center_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPersp, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPersp, 1); // J
    }
    if (addr_flags.linear_centroid_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPerspCentroid, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPerspCentroid, 1); // J
    }
    if (addr_flags.line_stipple_tex_ena) {
        ++dst_vreg;
    }
    return IR::VectorReg(dst_vreg);
}

static s8 UdRegFromShOffset(u16 sgpr_offset, HwStage hw_stage) {
    static constexpr std::array indirect_sgpr_offsets{0u, 0x4cu, 0u, 0xccu, 0u, 0x14cu};
    if (sgpr_offset) {
        const u32 ud_reg = sgpr_offset - indirect_sgpr_offsets[u32(hw_stage)];
        ASSERT_MSG(ud_reg < 16, "Out of bounds indirect SGPR copy");
        return ud_reg;
    }
    return -1;
}

Translator::Translator(Info& info_, const RuntimeInfo& runtime_info_, const Profile& profile_)
    : info{info_}, runtime_info{runtime_info_}, profile{profile_} {
    IterateBarycentrics(runtime_info, [this](u32 vreg, IR::Attribute attrib, u32) {
        vgpr_to_interp[vreg] = attrib;
    });
}

void Translator::EmitPrologue(IR::Block* first_block) {
    ir = IR::IREmitter(*first_block, first_block->begin());

    ir.Prologue();
    ir.SetExec(ir.Imm1(true));

    // Initialize user data.
    IR::ScalarReg dst_sreg = IR::ScalarReg::S0;
    for (u32 i = 0; i < runtime_info.props.num_user_data; i++) {
        ir.SetScalarReg(dst_sreg, ir.GetUserData(dst_sreg));
        ++dst_sreg;
    }

    IR::VectorReg dst_vreg = IR::VectorReg::V0;
    switch (info.sw_stage) {
    case SwStage::Vertex: {
        const s8 base_vertex_sgpr =
            UdRegFromShOffset(runtime_info.sw.vs.vertex_sgpr_offset, info.hw_stage);
        if (base_vertex_sgpr != -1) {
            ir.SetScalarReg(IR::ScalarReg(base_vertex_sgpr),
                            ir.GetAttributeU32(IR::Attribute::BaseVertex));
        }
        const s8 base_instance_sgpr =
            UdRegFromShOffset(runtime_info.sw.vs.instance_sgpr_offset, info.hw_stage);
        if (base_instance_sgpr != -1) {
            ir.SetScalarReg(IR::ScalarReg(base_instance_sgpr),
                            ir.GetAttributeU32(IR::Attribute::BaseInstance));
        }

        // v0: vertex ID, always present
        IR::U32 vertex_id = ir.GetAttributeU32(IR::Attribute::VertexId);
        if (base_vertex_sgpr != -1) {
            if (!fetch_data || fetch_data->vertex_offset_sgpr == -1) {
                vertex_id = ir.ISub(vertex_id, ir.GetAttributeU32(IR::Attribute::BaseVertex));
            } else {
                ASSERT_MSG(fetch_data->vertex_offset_sgpr == base_vertex_sgpr,
                           "Fetch shader in indirect draw uses wrong base vertex");
            }
        }
        ir.SetVectorReg(dst_vreg++, vertex_id);

        if (info.hw_stage == HwStage::Local) {
            // v1: rel patch ID
            if (runtime_info.props.num_input_vgprs > 0) {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
            }
            // v2: unknown
            if (runtime_info.props.num_input_vgprs > 1) {
                ++dst_vreg;
            }
        } else {
            // v1: instance ID, step rate 0
            if (runtime_info.props.num_input_vgprs > 0) {
                if (runtime_info.sw.vs.step_rate_0 != 0) {
                    ir.SetVectorReg(dst_vreg++,
                                    ir.IDiv(ir.GetAttributeU32(IR::Attribute::InstanceId),
                                            ir.Imm32(runtime_info.sw.vs.step_rate_0)));
                } else {
                    ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
                }
            }
            // v2: instance ID, step rate 1
            if (runtime_info.props.num_input_vgprs > 1) {
                if (runtime_info.sw.vs.step_rate_1 != 0) {
                    ir.SetVectorReg(dst_vreg++,
                                    ir.IDiv(ir.GetAttributeU32(IR::Attribute::InstanceId),
                                            ir.Imm32(runtime_info.sw.vs.step_rate_1)));
                } else {
                    ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
                }
            }
        }

        // v3: instance ID, plain
        if (runtime_info.props.num_input_vgprs > 2) {
            IR::U32 instance_id = ir.GetAttributeU32(IR::Attribute::InstanceId);
            if (base_instance_sgpr != -1) {
                if (!fetch_data || fetch_data->instance_offset_sgpr == -1) {
                    instance_id =
                        ir.ISub(instance_id, ir.GetAttributeU32(IR::Attribute::BaseInstance));
                } else {
                    ASSERT_MSG(fetch_data->instance_offset_sgpr == base_instance_sgpr,
                               "Fetch shader in indirect draw uses wrong base instance");
                }
            }
            ir.SetVectorReg(dst_vreg++, instance_id);
        }
        break;
    }
    case SwStage::Fragment: {
        dst_vreg =
            IterateBarycentrics(runtime_info, [this](u32 vreg, IR::Attribute attrib, u32 comp) {
                if (profile.supports_amd_shader_explicit_vertex_parameter ||
                    profile.supports_fragment_shader_barycentric ||
                    profile.emulate_fragment_interpolation) {
                    ir.SetVectorReg(IR::VectorReg(vreg), ir.GetAttribute(attrib, comp));
                }
            });
        const auto addr_flags = runtime_info.hw.fs.addr_flags;
        const auto en_flags = runtime_info.hw.fs.en_flags;
        if (addr_flags.pos_x_float_ena) {
            if (en_flags.pos_x_float_ena) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttribute(IR::Attribute::FragCoord, 0));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0.0f));
            }
        }
        if (addr_flags.pos_y_float_ena) {
            if (en_flags.pos_y_float_ena) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttribute(IR::Attribute::FragCoord, 1));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0.0f));
            }
        }
        if (addr_flags.pos_z_float_ena) {
            if (en_flags.pos_z_float_ena) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttribute(IR::Attribute::FragCoord, 2));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0.0f));
            }
        }
        if (addr_flags.pos_w_float_ena) {
            if (en_flags.pos_w_float_ena) {
                ir.SetVectorReg(dst_vreg++,
                                ir.FPRecip(ir.GetAttribute(IR::Attribute::FragCoord, 3)));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0.0f));
            }
        }
        if (addr_flags.front_face_ena) {
            if (en_flags.front_face_ena) {
                const IR::U1 front_face = ir.GetAttributeU1(IR::Attribute::IsFrontFace);
                if (runtime_info.hw.fs.front_face_all_bits) {
                    ir.SetVectorReg(dst_vreg++,
                                    IR::U32{ir.Select(front_face, ir.Imm32(1), ir.Imm32(0))});
                } else {
                    ir.SetVectorReg(dst_vreg++, IR::F32{ir.Select(front_face, ir.Imm32(1.0f),
                                                                  ir.Imm32(-1.0f))});
                }
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
            }
        }
        if (addr_flags.ancillary_ena) {
            if (en_flags.ancillary_ena) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::PackedAncillary));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
            }
        }
        if (addr_flags.sample_coverage_ena) {
            if (en_flags.sample_coverage_ena) {
                const u32 num_samples = std::clamp<u32>(runtime_info.hw.fs.num_samples, 1U, 16U);
                IR::U32 coverage = ir.Imm32(1U);
                if (num_samples > 1) {
                    const u32 valid_samples = (1U << num_samples) - 1U;
                    coverage = ir.BitwiseAnd(ir.GetAttributeU32(IR::Attribute::SampleMask),
                                             ir.Imm32(valid_samples));
                }
                const IR::U1 is_helper = ir.GetAttributeU1(IR::Attribute::IsHelperInvocation);
                ir.SetVectorReg(dst_vreg++, IR::U32{ir.Select(is_helper, ir.Imm32(0U), coverage)});
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
            }
        }
        break;
    }
    case SwStage::TessellationControl: {
        ir.SetVectorReg(IR::VectorReg::V0, ir.GetAttributeU32(IR::Attribute::PrimitiveId));
        // Should be laid out like:
        // [0:8]: patch id within VGT
        // [8:12]: output control point id
        ir.SetVectorReg(IR::VectorReg::V1,
                        ir.GetAttributeU32(IR::Attribute::PackedHullInvocationInfo));

        if (runtime_info.sw.tcs.offchip_lds_enable) {
            // No off-chip tessellation has been observed yet. If this survives dead code elim,
            // revisit
            ir.SetScalarReg(dst_sreg++, ir.GetAttributeU32(IR::Attribute::OffChipLdsBase));
        }
        ir.SetScalarReg(dst_sreg++, ir.GetAttributeU32(IR::Attribute::TessFactorsBufferBase));

        break;
    }
    case SwStage::TessellationEval:
        ir.SetVectorReg(IR::VectorReg::V0,
                        ir.GetAttribute(IR::Attribute::TessellationEvaluationPointU));
        ir.SetVectorReg(IR::VectorReg::V1,
                        ir.GetAttribute(IR::Attribute::TessellationEvaluationPointV));
        // V2 is similar to PrimitiveID but not the same. It seems to only be used in
        // compiler-generated address calculations. Its probably the patch id within the
        // patches running locally on a given VGT (or CU, whichever is the granularity of LDS
        // memory)
        // Set to 0. See explanation in comment describing hull/domain passes
        ir.SetVectorReg(IR::VectorReg::V2, ir.Imm32(0u));
        // V3 is the actual PrimitiveID as intended by the shader author.
        ir.SetVectorReg(IR::VectorReg::V3, ir.GetAttributeU32(IR::Attribute::PrimitiveId));
        break;
    case SwStage::Compute:
        ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0));
        ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 1));
        ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 2));

        if (runtime_info.hw.cs.tgid_enable[0]) {
            ir.SetScalarReg(dst_sreg++, ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0));
        }
        if (runtime_info.hw.cs.tgid_enable[1]) {
            ir.SetScalarReg(dst_sreg++, ir.GetAttributeU32(IR::Attribute::WorkgroupId, 1));
        }
        if (runtime_info.hw.cs.tgid_enable[2]) {
            ir.SetScalarReg(dst_sreg++, ir.GetAttributeU32(IR::Attribute::WorkgroupId, 2));
        }
        break;
    case SwStage::Geometry:
        // The GS wave's input VGPRs have a fixed layout, whatever the input primitive: ES
        // vertex offsets 0 and 1 in V0-V1, the primitive id in V2, vertex offsets 2 to 5 in
        // V3-V6 (3 to 5 only for adjacency) and the GS instance in V7, which instanced shaders
        // (one invocation per eye) pick their view by. Registers a shader does not read are
        // dropped by dead code elimination.
        ir.SetVectorReg(IR::VectorReg::V0, ir.Imm32(0u)); // vertex 0
        ir.SetVectorReg(IR::VectorReg::V1, ir.Imm32(1u)); // vertex 1
        ir.SetVectorReg(IR::VectorReg::V2, ir.GetAttributeU32(IR::Attribute::PrimitiveId));
        ir.SetVectorReg(IR::VectorReg::V3, ir.Imm32(2u)); // vertex 2
        ir.SetVectorReg(IR::VectorReg::V4, ir.Imm32(3u)); // vertex 3 (adjacency)
        ir.SetVectorReg(IR::VectorReg::V5, ir.Imm32(4u)); // vertex 4 (adjacency)
        ir.SetVectorReg(IR::VectorReg::V6, ir.Imm32(5u)); // vertex 5 (adjacency)
        ir.SetVectorReg(IR::VectorReg::V7, ir.GetAttributeU32(IR::Attribute::InvocationId));
        break;
    default:
        UNREACHABLE_MSG("Unknown shader stage");
    }
}

template <typename T>
T Translator::GetSrc(const InstOperand& operand) {
    constexpr bool is_float = std::is_same_v<T, IR::F32>;

    const auto get_imm = [&](auto value) -> T {
        if constexpr (is_float) {
            return ir.Imm32(std::bit_cast<float>(value));
        } else {
            return ir.Imm32(std::bit_cast<u32>(value));
        }
    };

    T value{};
    switch (operand.field) {
    case OperandField::ScalarGPR:
        value = ir.GetScalarReg<T>(IR::ScalarReg(operand.code));
        break;
    case OperandField::VectorGPR:
        value = ir.GetVectorReg<T>(IR::VectorReg(operand.code));
        break;
    case OperandField::ConstZero:
        value = get_imm(0U);
        break;
    case OperandField::SignedConstIntPos:
        value = get_imm(operand.code - SignedConstIntPosMin + 1);
        break;
    case OperandField::SignedConstIntNeg:
        value = get_imm(-s32(operand.code) + SignedConstIntNegMin - 1);
        break;
    case OperandField::LiteralConst:
        value = get_imm(operand.code);
        break;
    case OperandField::ConstFloatPos_1_0:
        value = get_imm(1.f);
        break;
    case OperandField::ConstFloatPos_0_5:
        value = get_imm(0.5f);
        break;
    case OperandField::ConstFloatPos_2_0:
        value = get_imm(2.0f);
        break;
    case OperandField::ConstFloatPos_4_0:
        value = get_imm(4.0f);
        break;
    case OperandField::ConstFloatNeg_0_5:
        value = get_imm(-0.5f);
        break;
    case OperandField::ConstFloatNeg_1_0:
        value = get_imm(-1.0f);
        break;
    case OperandField::ConstFloatNeg_2_0:
        value = get_imm(-2.0f);
        break;
    case OperandField::ConstFloatNeg_4_0:
        value = get_imm(-4.0f);
        break;
    case OperandField::Inv2Pi:
        value = get_imm(static_cast<float>(1.0f / (2.0f * std::numbers::pi)));
        break;
    case OperandField::Sdwa:
        UNREACHABLE_MSG("unhandled SDWA");
    case OperandField::Dpp:
        UNREACHABLE_MSG("unhandled DPP");
    case OperandField::VccLo:
        if constexpr (is_float) {
            value = ir.BitCast<IR::F32>(ir.GetVccLo());
        } else {
            value = ir.GetVccLo();
        }
        break;
    case OperandField::VccHi:
        if constexpr (is_float) {
            value = ir.BitCast<IR::F32>(ir.GetVccHi());
        } else {
            value = ir.GetVccHi();
        }
        break;
    case OperandField::M0:
        if constexpr (is_float) {
            value = ir.BitCast<IR::F32>(ir.GetM0());
        } else {
            value = ir.GetM0();
        }
        break;
    case OperandField::Scc:
        if constexpr (is_float) {
            UNREACHABLE();
        } else {
            value = IR::U32{ir.Select(ir.GetScc(), ir.Imm32(1u), ir.Imm32(0u))};
        }
        break;
    case OperandField::ExecLo:
        if constexpr (is_float) {
            value = ir.BitCast<IR::F32>(
                IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(ir.GetExec())), 0)});
        } else {
            value = IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(ir.GetExec())), 0)};
        }
        break;
    case OperandField::ExecHi:
        if constexpr (is_float) {
            value = ir.BitCast<IR::F32>(
                IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(ir.GetExec())), 1)});
        } else {
            value = IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(ir.GetExec())), 1)};
        }
        break;
    default:
        UNREACHABLE_MSG("Unexpected operand: {}", std::to_underlying(operand.field));
    }

    if constexpr (is_float) {
        if (operand.input_modifier.abs) {
            value = ir.FPAbs(value);
        }
        if (operand.input_modifier.neg) {
            value = ir.FPNeg(value);
        }
    } else {
        if (operand.input_modifier.abs) {
            value = ir.BitwiseAnd(value, ir.Imm32(0x7FFFFFFFu));
        }
        if (operand.input_modifier.neg) {
            value = ir.BitwiseXor(value, ir.Imm32(0x80000000u));
        }
    }
    return value;
}

template IR::U32 Translator::GetSrc<IR::U32>(const InstOperand&);
template IR::F32 Translator::GetSrc<IR::F32>(const InstOperand&);

template <typename T, bool is_signed>
T Translator::GetSrc16(const InstOperand& operand) {
    constexpr bool is_float = std::is_same_v<T, IR::F32>;

    const auto get_imm = [&](auto value) -> T {
        if constexpr (is_float) {
            return ir.Imm32(std::bit_cast<float>(value));
        } else {
            return ir.Imm32(std::bit_cast<u32>(value));
        }
    };

    const auto number_format = []() -> AmdGpu::NumberFormat {
        if constexpr (is_float) {
            return AmdGpu::NumberFormat::Float;
        } else {
            return AmdGpu::NumberFormat::Uint;
        }
    }();

    const auto bitcast_to_u = [&](auto value) -> IR::U32 {
        if constexpr (is_float) {
            return ir.BitCast<IR::U32>(value);
        } else {
            return value;
        }
    };

    const auto cast = [&](auto value) -> T {
        if constexpr (is_float) {
            return value;
        } else {
            return ir.BitFieldExtract(ir.BitCast<IR::U32>(value), ir.Imm32(0), ir.Imm32(16),
                                      is_signed);
        }
    };

    const auto op_sel = operand.op_sel.op_sel;

    T value{};
    switch (operand.field) {
    case OperandField::ScalarGPR: {
        const auto f = ir.GetScalarReg<T>(IR::ScalarReg(operand.code));
        value = cast(IR::F32{
            ir.CompositeExtract(ir.Unpack2x16(number_format, bitcast_to_u(f)), op_sel ? 1 : 0)});
        break;
    }
    case OperandField::VectorGPR: {
        const auto v = ir.GetVectorReg<T>(IR::VectorReg(operand.code));
        value = cast(IR::F32{
            ir.CompositeExtract(ir.Unpack2x16(number_format, bitcast_to_u(v)), op_sel ? 1 : 0)});
        break;
    }
    case OperandField::ConstZero:
        value = get_imm(0U);
        break;
    case OperandField::SignedConstIntPos:
        value = get_imm(operand.code - SignedConstIntPosMin + 1);
        break;
    case OperandField::SignedConstIntNeg:
        value = get_imm(-s32(operand.code) + SignedConstIntNegMin - 1);
        break;
    case OperandField::LiteralConst:
        value = get_imm(operand.code);
        break;
    case OperandField::ConstFloatPos_1_0:
        value = get_imm(1.f);
        break;
    case OperandField::ConstFloatPos_0_5:
        value = get_imm(0.5f);
        break;
    case OperandField::ConstFloatPos_2_0:
        value = get_imm(2.0f);
        break;
    case OperandField::ConstFloatPos_4_0:
        value = get_imm(4.0f);
        break;
    case OperandField::ConstFloatNeg_0_5:
        value = get_imm(-0.5f);
        break;
    case OperandField::ConstFloatNeg_1_0:
        value = get_imm(-1.0f);
        break;
    case OperandField::ConstFloatNeg_2_0:
        value = get_imm(-2.0f);
        break;
    case OperandField::ConstFloatNeg_4_0:
        value = get_imm(-4.0f);
        break;
    case OperandField::Inv2Pi:
        value = get_imm(static_cast<float>(1.0f / (2.0f * std::numbers::pi)));
        break;
    case OperandField::Sdwa:
        LOG_ERROR(Render_Recompiler, "unhandled SDWA");
        value = get_imm(0U);
        break;
    case OperandField::Dpp:
        LOG_ERROR(Render_Recompiler, "unhandled DPP");
        value = get_imm(0U);
        break;
    case OperandField::VccLo:
        if constexpr (is_float) {
            value = IR::F32{
                ir.CompositeExtract(ir.Unpack2x16(number_format, ir.GetVccLo()), op_sel ? 1 : 0)};
        } else {
            value = cast(IR::F32{ir.CompositeExtract(
                ir.Unpack2x16(number_format, bitcast_to_u(ir.GetVccLo())), op_sel ? 1 : 0)});
        }
        break;
    default:
        UNREACHABLE_MSG("Unexpected operand: {}", std::to_underlying(operand.field));
    }

    if constexpr (is_float) {
        if (operand.input_modifier.abs) {
            value = ir.FPAbs(value);
        }
        if (operand.input_modifier.neg) {
            value = ir.FPNeg(value);
        }
    } else {
        if (operand.input_modifier.abs) {
            value = ir.BitwiseAnd(value, ir.Imm32(0x7FFFFFFFu));
        }
        if (operand.input_modifier.neg) {
            value = ir.BitwiseXor(value, ir.Imm32(0x80000000u));
        }
    }
    return value;
}

template IR::U32 Translator::GetSrc16<IR::U32, false>(const InstOperand&);
template IR::U32 Translator::GetSrc16<IR::U32, true>(const InstOperand&);
template IR::F32 Translator::GetSrc16<IR::F32, false>(const InstOperand&);

IR::F32 Translator::GetSrcMix(const InstOperand& operand) {
    const auto get_imm = [&](auto value) -> IR::F32 {
        return ir.Imm32(std::bit_cast<float>(value));
    };

    const auto extract = [&](auto value) -> IR::F32 {
        const auto getter_u = [&]() {
            if constexpr (std::same_as<decltype(value), IR::ScalarReg>) {
                return ir.GetScalarReg<IR::U32>(value);
            } else {
                return ir.GetVectorReg<IR::U32>(value);
            }
        }();
        if (!operand.op_sel.op_sel_hi) {
            if constexpr (std::same_as<decltype(value), IR::ScalarReg>) {
                return ir.GetScalarReg<IR::F32>(value);
            } else {
                return ir.GetVectorReg<IR::F32>(value);
            }
        } else if (operand.op_sel.op_sel) {
            return IR::F32{
                ir.CompositeExtract(ir.Unpack2x16(AmdGpu::NumberFormat::Float, getter_u), 1)};
        } else {
            return IR::F32{
                ir.CompositeExtract(ir.Unpack2x16(AmdGpu::NumberFormat::Float, getter_u), 0)};
        }
    };

    IR::F32 value{};
    switch (operand.field) {
    case OperandField::ScalarGPR:
        value = extract(IR::ScalarReg(operand.code));
        break;
    case OperandField::VectorGPR:
        value = extract(IR::VectorReg(operand.code));
        break;
    case OperandField::ConstZero:
        value = get_imm(0U);
        break;
    case OperandField::SignedConstIntPos:
        value = get_imm(operand.code - SignedConstIntPosMin + 1);
        break;
    case OperandField::SignedConstIntNeg:
        value = get_imm(-s32(operand.code) + SignedConstIntNegMin - 1);
        break;
    case OperandField::LiteralConst:
        value = get_imm(operand.code);
        break;
    case OperandField::ConstFloatPos_1_0:
        value = get_imm(1.f);
        break;
    case OperandField::ConstFloatPos_0_5:
        value = get_imm(0.5f);
        break;
    case OperandField::ConstFloatPos_2_0:
        value = get_imm(2.0f);
        break;
    case OperandField::ConstFloatPos_4_0:
        value = get_imm(4.0f);
        break;
    case OperandField::ConstFloatNeg_0_5:
        value = get_imm(-0.5f);
        break;
    case OperandField::ConstFloatNeg_1_0:
        value = get_imm(-1.0f);
        break;
    case OperandField::ConstFloatNeg_2_0:
        value = get_imm(-2.0f);
        break;
    case OperandField::ConstFloatNeg_4_0:
        value = get_imm(-4.0f);
        break;
    case OperandField::VccLo: {
        if (!operand.op_sel.op_sel_hi) {
            value = ir.BitCast<IR::F32>(ir.GetVccLo());
        } else if (operand.op_sel.op_sel) {
            value = IR::F32{
                ir.CompositeExtract(ir.Unpack2x16(AmdGpu::NumberFormat::Float, ir.GetVccLo()), 1)};
        } else {
            value = IR::F32{
                ir.CompositeExtract(ir.Unpack2x16(AmdGpu::NumberFormat::Float, ir.GetVccLo()), 0)};
        }
        break;
    }
    case OperandField::VccHi:
        UNREACHABLE();
        break;
    case OperandField::M0:
        UNREACHABLE();
        break;
    case OperandField::Scc:
        UNREACHABLE();
        break;
    case OperandField::Inv2Pi:
        value = get_imm(static_cast<float>(1.0f / (2.0f * std::numbers::pi)));
        break;
    default:
        UNREACHABLE_MSG("Unexpected operand: {}", std::to_underlying(operand.field));
    }

    if (operand.input_modifier.neg_hi) {
        value = ir.FPAbs(value);
    }
    if (operand.input_modifier.neg) {
        value = ir.FPNeg(value);
    }
    return value;
}

template <typename T>
T Translator::GetSrc64(const InstOperand& operand) {
    constexpr bool is_float = std::is_same_v<T, IR::F64>;

    const auto get_imm = [&](auto value) -> T {
        if constexpr (is_float) {
            return ir.Imm64(std::bit_cast<double>(value));
        } else {
            return ir.Imm64(std::bit_cast<u64>(value));
        }
    };

    T value{};
    switch (operand.field) {
    case OperandField::ScalarGPR: {
        const auto value_lo = ir.GetScalarReg(IR::ScalarReg(operand.code));
        const auto value_hi = ir.GetScalarReg(IR::ScalarReg(operand.code + 1));
        if constexpr (is_float) {
            value = ir.PackDouble2x32(ir.CompositeConstruct(value_lo, value_hi));
        } else {
            value = ir.PackUint2x32(ir.CompositeConstruct(value_lo, value_hi));
        }
        break;
    }
    case OperandField::VectorGPR: {
        const auto value_lo = ir.GetVectorReg(IR::VectorReg(operand.code));
        const auto value_hi = ir.GetVectorReg(IR::VectorReg(operand.code + 1));
        if constexpr (is_float) {
            value = ir.PackDouble2x32(ir.CompositeConstruct(value_lo, value_hi));
        } else {
            value = ir.PackUint2x32(ir.CompositeConstruct(value_lo, value_hi));
        }
        break;
    }
    case OperandField::ConstZero:
        value = get_imm(0ULL);
        break;
    case OperandField::SignedConstIntPos:
        value = get_imm(s64(operand.code) - SignedConstIntPosMin + 1);
        break;
    case OperandField::SignedConstIntNeg:
        value = get_imm(-s64(operand.code) + SignedConstIntNegMin - 1);
        break;
    case OperandField::LiteralConst:
        if constexpr (is_float) {
            value = get_imm(u64(operand.code) << 32);
        } else {
            value = get_imm(u64(operand.code));
        }
        break;
    case OperandField::ConstFloatPos_1_0:
        value = get_imm(1.0);
        break;
    case OperandField::ConstFloatPos_0_5:
        value = get_imm(0.5);
        break;
    case OperandField::ConstFloatPos_2_0:
        value = get_imm(2.0);
        break;
    case OperandField::ConstFloatPos_4_0:
        value = get_imm(4.0);
        break;
    case OperandField::ConstFloatNeg_0_5:
        value = get_imm(-0.5);
        break;
    case OperandField::ConstFloatNeg_1_0:
        value = get_imm(-1.0);
        break;
    case OperandField::ConstFloatNeg_2_0:
        value = get_imm(-2.0);
        break;
    case OperandField::ConstFloatNeg_4_0:
        value = get_imm(-4.0);
        break;
    case OperandField::VccLo:
        if constexpr (is_float) {
            value = ir.PackDouble2x32(ir.CompositeConstruct(ir.GetVccLo(), ir.GetVccHi()));
        } else {
            value = ir.PackUint2x32(ir.CompositeConstruct(ir.GetVccLo(), ir.GetVccHi()));
        }
        break;
    case OperandField::ExecLo:
        if constexpr (is_float) {
            UNREACHABLE();
        } else {
            value = ir.Ballot(ir.GetExec());
        }
        break;
    default:
        UNREACHABLE_MSG("Unexpected operand: {}", std::to_underlying(operand.field));
    }

    if constexpr (is_float) {
        if (operand.input_modifier.abs) {
            value = ir.FPAbs(value);
        }
        if (operand.input_modifier.neg) {
            value = ir.FPNeg(value);
        }
    } else {
        // GCN VOP3 abs/neg modifier bits operate on the sign bit (bit 63 for
        // 64-bit values). Unpack, modify the high dword's bit 31, repack.
        if (operand.input_modifier.abs) {
            const auto unpacked = ir.UnpackUint2x32(value);
            const auto lo = IR::U32{ir.CompositeExtract(unpacked, 0)};
            const auto hi = IR::U32{ir.CompositeExtract(unpacked, 1)};
            const auto hi_abs = ir.BitwiseAnd(hi, ir.Imm32(0x7FFFFFFFu));
            value = ir.PackUint2x32(ir.CompositeConstruct(lo, hi_abs));
        }
        if (operand.input_modifier.neg) {
            const auto unpacked = ir.UnpackUint2x32(value);
            const auto lo = IR::U32{ir.CompositeExtract(unpacked, 0)};
            const auto hi = IR::U32{ir.CompositeExtract(unpacked, 1)};
            const auto hi_neg = ir.BitwiseXor(hi, ir.Imm32(0x80000000u));
            value = ir.PackUint2x32(ir.CompositeConstruct(lo, hi_neg));
        }
    }
    return value;
}

template IR::U64 Translator::GetSrc64<IR::U64>(const InstOperand&);
template IR::F64 Translator::GetSrc64<IR::F64>(const InstOperand&);

template <typename T, bool is_signed>
pk_type<T> Translator::GetSrcPk(const InstOperand& operand) {
    constexpr bool is_float = std::is_same_v<T, IR::F32>;

    const auto get_imm = [&](auto value) -> pk_type<T> {
        if constexpr (is_float) {
            auto imm = ir.Imm32(std::bit_cast<float>(value));
            return {operand.op_sel.op_sel ? ir.Imm32(0.f) : imm,
                    operand.op_sel.op_sel_hi ? ir.Imm32(0.f) : imm};
        } else {
            auto imm = ir.Imm32(std::bit_cast<u32>(value));
            return {operand.op_sel.op_sel ? ir.Imm32(0U) : imm,
                    operand.op_sel.op_sel_hi ? ir.Imm32(0U) : imm};
        }
    };

    constexpr auto number_format = [&]() {
        if constexpr (is_float) {
            return AmdGpu::NumberFormat::Float;
        } else {
            return AmdGpu::NumberFormat::Uint;
        }
    }();

    const auto cast = [&](auto value) -> T {
        if constexpr (is_float) {
            return value;
        } else {
            return ir.BitFieldExtract(ir.BitCast<IR::U32>(value), ir.Imm32(0), ir.Imm32(16),
                                      is_signed);
        }
    };

    const auto extract = [&](auto value) -> pk_type<T> {
        auto v_unpacked = ir.Unpack2x16(number_format, value);
        return {cast(IR::F32{ir.CompositeExtract(v_unpacked, operand.op_sel.op_sel)}),
                cast(IR::F32{ir.CompositeExtract(v_unpacked, operand.op_sel.op_sel_hi)})};
    };

    pk_type<T> value{};
    switch (operand.field) {
    case OperandField::ScalarGPR: {
        value = extract(ir.GetScalarReg<IR::U32>(IR::ScalarReg(operand.code)));
        break;
    }
    case OperandField::VectorGPR: {
        value = extract(ir.GetVectorReg<IR::U32>(IR::VectorReg(operand.code)));
        break;
    }
    case OperandField::ConstZero: {
        value = get_imm(0U);
        break;
    }
    case OperandField::SignedConstIntPos: {
        value = get_imm(operand.code - SignedConstIntPosMin + 1);
        break;
    }
    case OperandField::SignedConstIntNeg: {
        value = get_imm(-s32(operand.code) + SignedConstIntNegMin - 1);
        break;
    }
    case OperandField::LiteralConst: {
        value = get_imm(operand.code);
        break;
    }
    case OperandField::ConstFloatPos_1_0: {
        value = get_imm(1.f);
        break;
    }
    case OperandField::ConstFloatPos_0_5: {
        value = get_imm(0.5f);
        break;
    }
    case OperandField::ConstFloatPos_2_0: {
        value = get_imm(2.0f);
        break;
    }
    case OperandField::ConstFloatPos_4_0: {
        value = get_imm(4.0f);
        break;
    }
    case OperandField::ConstFloatNeg_0_5: {
        value = get_imm(-0.5f);
        break;
    }
    case OperandField::ConstFloatNeg_1_0: {
        value = get_imm(-1.0f);
        break;
    }
    case OperandField::ConstFloatNeg_2_0: {
        value = get_imm(-2.0f);
        break;
    }
    case OperandField::ConstFloatNeg_4_0: {
        value = get_imm(-4.0f);
        break;
    }
    case OperandField::Inv2Pi: {
        value = get_imm(1.0f / (2.0f * std::numbers::pi_v<float>));
        break;
    }
    case OperandField::VccLo:
        value = extract(ir.GetVccLo());
        break;
    default:
        UNREACHABLE_MSG("Unexpected operand: {}", std::to_underlying(operand.field));
    }

    if constexpr (is_float) {
        if (operand.input_modifier.neg) {
            value.first = ir.FPNeg(value.first);
        }
        if (operand.input_modifier.neg_hi) {
            value.second = ir.FPNeg(value.second);
        }
    } else {
        if (operand.input_modifier.neg) {
            value.first = ir.INeg(value.first);
        }
        if (operand.input_modifier.neg_hi) {
            value.second = ir.INeg(value.second);
        }
    }
    return value;
}

template pk_type<IR::U32> Translator::GetSrcPk<IR::U32, true>(const InstOperand&);
template pk_type<IR::U32> Translator::GetSrcPk<IR::U32, false>(const InstOperand&);
template pk_type<IR::F32> Translator::GetSrcPk<IR::F32, false>(const InstOperand&);

void Translator::SetDst(const InstOperand& operand, const IR::U32F32& value) {
    IR::U32F32 result = value;
    if (value.Type() == IR::Type::F32) {
        if (operand.output_modifier.multiplier != 0.f) {
            result = ir.FPMul(result, ir.Imm32(operand.output_modifier.multiplier));
        }
        if (operand.output_modifier.clamp) {
            result = ir.FPSaturate(result);
        }
    }

    switch (operand.field) {
    case OperandField::ScalarGPR:
        return ir.SetScalarReg(IR::ScalarReg(operand.code), result);
    case OperandField::VectorGPR:
        return ir.SetVectorReg(IR::VectorReg(operand.code), result);
    case OperandField::VccLo:
        return ir.SetVccLo(result);
    case OperandField::VccHi:
        return ir.SetVccHi(result);
    case OperandField::M0:
        return ir.SetM0(result);
    default:
        UNREACHABLE_MSG("Unknown field {}", u32(operand.field));
    }
}

template <bool is_signed>
void Translator::SetDst16(const InstOperand& operand, const IR::U32F32& value) {
    IR::U32F32 result = value;
    if (value.Type() == IR::Type::F32) {
        if (operand.output_modifier.multiplier != 0.f) {
            result = ir.FPMul(result, ir.Imm32(operand.output_modifier.multiplier));
        }
        if (operand.output_modifier.clamp) {
            result = ir.FPSaturate(result);
        }
    } else {
        if (operand.output_modifier.clamp) {
            if constexpr (is_signed) {
                result = ir.SClamp(result, ir.Imm32(-32768), ir.Imm32(32767));
            } else {
                result = ir.UMin(result, ir.Imm32(0xFFFF));
            }
        }
    }

    const auto cast = [&](auto value) -> IR::U32 {
        if (value.Type() == IR::Type::F32) {
            return ir.UConvert(32, ir.BitCast<IR::U16>(IR::F16{ir.FPConvert(16, value)}));
        } else if (value.Type() == IR::Type::U32) {
            return value;
        } else {
            UNREACHABLE();
        }
    };

    const auto op_sel = operand.op_sel.op_sel;

    switch (operand.field) {
    case OperandField::ScalarGPR: {
        const auto prev_dst = ir.GetScalarReg<IR::U32>(IR::ScalarReg(operand.code));
        const auto result_16 = cast(result);
        const auto new_dst =
            ir.BitFieldInsert(prev_dst, result_16, ir.Imm32(op_sel ? 16 : 0), ir.Imm32(16));
        return ir.SetScalarReg(IR::ScalarReg(operand.code), new_dst);
    }
    case OperandField::VectorGPR: {
        const auto prev_dst = ir.GetVectorReg<IR::U32>(IR::VectorReg(operand.code));
        const auto result_16 = cast(result);
        const auto new_dst =
            ir.BitFieldInsert(prev_dst, result_16, ir.Imm32(op_sel ? 16 : 0), ir.Imm32(16));
        return ir.SetVectorReg(IR::VectorReg(operand.code), new_dst);
    }
    default:
        UNREACHABLE_MSG("Unexpected operand: {}", std::to_underlying(operand.field));
    }
}

template void Translator::SetDst16<false>(const InstOperand&, const IR::U32F32& value);
template void Translator::SetDst16<true>(const InstOperand&, const IR::U32F32& value);

void Translator::SetDst64(const InstOperand& operand, const IR::U64F64& value_raw) {
    IR::U64F64 value_untyped = value_raw;

    const bool is_float = value_raw.Type() == IR::Type::F64 || value_raw.Type() == IR::Type::F32;
    if (is_float) {
        if (operand.output_modifier.multiplier != 0.f) {
            value_untyped =
                ir.FPMul(value_untyped, ir.Imm64(f64(operand.output_modifier.multiplier)));
        }
        if (operand.output_modifier.clamp) {
            value_untyped = ir.FPSaturate(value_untyped);
        }
    }

    const auto split = [&] -> std::pair<IR::U32, IR::U32> {
        const IR::Value unpacked{is_float ? ir.UnpackDouble2x32(IR::F64{value_untyped})
                                          : ir.UnpackUint2x32(IR::U64{value_untyped})};
        const IR::U32 lo{ir.CompositeExtract(unpacked, 0U)};
        const IR::U32 hi{ir.CompositeExtract(unpacked, 1U)};
        return {lo, hi};
    };
    switch (operand.field) {
    case OperandField::ScalarGPR: {
        const auto [lo, hi] = split();
        ir.SetScalarReg(IR::ScalarReg(operand.code + 1), hi);
        ir.SetScalarReg(IR::ScalarReg(operand.code), lo);
        break;
    }
    case OperandField::VectorGPR: {
        const auto [lo, hi] = split();
        ir.SetVectorReg(IR::VectorReg(operand.code + 1), hi);
        ir.SetVectorReg(IR::VectorReg(operand.code), lo);
        break;
    }
    case OperandField::VccLo: {
        const auto [lo, hi] = split();
        ir.SetVccLo(lo);
        ir.SetVccHi(hi);
        break;
    }
    case OperandField::ExecLo:
        ir.SetExec(ir.InverseBallot(value_untyped));
        break;
    default:
        UNREACHABLE_MSG("Unexpected operand: {}", std::to_underlying(operand.field));
    }
}

template <typename T, bool is_signed>
void Translator::SetDstPk(const InstOperand& operand, const pk_type<T>& value) {
    pk_type<T> v = value;

    if constexpr (std::is_same_v<T, IR::F32>) {
        if (operand.output_modifier.clamp) {
            v = {ir.FPSaturate(v.first), ir.FPSaturate(v.second)};
        }
    } else {
        if (operand.output_modifier.clamp) {
            if constexpr (is_signed) {
                auto lower = ir.Imm32(-32768);
                auto upper = ir.Imm32(32767);
                v = {ir.SClamp(v.first, lower, upper), ir.SClamp(v.second, lower, upper)};
            } else {
                auto imm = ir.Imm32(0xFFFF);
                v = {ir.UMin(v.first, imm), ir.UMin(v.second, imm)};
            }
        }
    }

    IR::U32 value_raw{};
    if constexpr (std::is_same_v<T, IR::F32>) {
        value_raw =
            ir.Pack2x16(AmdGpu::NumberFormat::Float, ir.CompositeConstruct(v.first, v.second));
    } else {
        value_raw = ir.Pack2x16(AmdGpu::NumberFormat::Uint,
                                ir.CompositeConstruct(ir.BitCast<IR::F32, IR::U32>(v.first),
                                                      ir.BitCast<IR::F32, IR::U32>(v.second)));
    }
    SetDst(operand, value_raw);
}

template void Translator::SetDstPk<IR::U32, false>(const InstOperand& operand,
                                                   const pk_type<IR::U32>& value);
template void Translator::SetDstPk<IR::U32, true>(const InstOperand& operand,
                                                  const pk_type<IR::U32>& value);
template void Translator::SetDstPk<IR::F32, false>(const InstOperand& operand,
                                                   const pk_type<IR::F32>& value);

void Translator::EmitFetch(const GcnInst& inst) {
    const auto code_sgpr_base = inst.src[0].code;

#if 0
    // Translate fetch shader inline using regular buffer bindings; useful for debugging.
    const auto* code = GetFetchShaderCode(info, code_sgpr_base);
    GcnCodeSlice slice(code, code + std::numeric_limits<u32>::max());
    GcnDecodeContext decoder;

    // Decode and save instructions
    while (!slice.atEnd()) {
        const auto sub_inst = decoder.decodeInstruction(slice);
        if (sub_inst.opcode == Opcode::S_SETPC_B64) {
            // Assume we're swapping back to the main shader.
            break;
        }
        TranslateInstruction(sub_inst);
    }
    return;
#endif

    info.has_fetch_shader = true;
    info.fetch_shader_sgpr_base = code_sgpr_base;

    fetch_data = ParseFetchShader(info);
    ASSERT(fetch_data.has_value());

    if (EmulatorSettings.IsDumpShaders()) {
        using namespace Common::FS;
        const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
        if (!std::filesystem::exists(dump_dir)) {
            std::filesystem::create_directories(dump_dir);
        }
        const auto filename = fmt::format("vs_{:#018x}.fetch.bin", info.pgm_hash);
        const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
        const auto* code = GetFetchShaderCode(info, code_sgpr_base);
        file.WriteRaw<u8>(code, fetch_data->size);
    }

    for (const auto& attrib : fetch_data->attributes) {
        const IR::Attribute attr{IR::Attribute::Param0 + attrib.semantic};
        IR::VectorReg dst_reg{attrib.dest_vgpr};

        // Read the V# of the attribute to figure out component number and type.
        const auto buffer = attrib.GetSharp(info);
        const auto values =
            ir.CompositeConstruct(ir.GetAttribute(attr, 0), ir.GetAttribute(attr, 1),
                                  ir.GetAttribute(attr, 2), ir.GetAttribute(attr, 3));
        const auto converted =
            IR::ApplyReadNumberConversionVec4(ir, values, buffer.GetNumberConversion());
        const auto swizzled = ApplySwizzle(ir, converted, buffer.DstSelect());
        for (u32 i = 0; i < 4; i++) {
            ir.SetVectorReg(dst_reg++, IR::F32{ir.CompositeExtract(swizzled, i)});
        }
    }
}

void Translator::LogMissingOpcode(const GcnInst& inst) {
    LOG_ERROR(Render_Recompiler, "Unknown opcode {} ({}, category = {})",
              magic_enum::enum_name(inst.opcode), u32(inst.opcode),
              magic_enum::enum_name(inst.category));
    info.translation_failed = true;
}

namespace {

bool WritesExec(const GcnInst& inst) {
    for (u32 i = 0; i < inst.dst_count; ++i) {
        const OperandField field = inst.dst[i].field;
        if (field == OperandField::ExecLo || field == OperandField::ExecHi) {
            return true;
        }
    }
    switch (inst.opcode) {
    case Opcode::S_AND_SAVEEXEC_B64:
    case Opcode::S_OR_SAVEEXEC_B64:
    case Opcode::S_XOR_SAVEEXEC_B64:
    case Opcode::S_ANDN2_SAVEEXEC_B64:
    case Opcode::S_ORN2_SAVEEXEC_B64:
    case Opcode::S_NAND_SAVEEXEC_B64:
    case Opcode::S_NOR_SAVEEXEC_B64:
    case Opcode::S_XNOR_SAVEEXEC_B64:
        return true;
    default:
        return inst.IsCmpx();
    }
}

bool IsWqmOfExec(const GcnInst& inst) {
    return inst.opcode == Opcode::S_WQM_B64 && inst.dst[0].field == OperandField::ExecLo &&
           inst.src[0].field == OperandField::ExecLo;
}

/// Number of SGPRs an instruction writes from a scalar destination operand on.
u32 ScalarWriteCount(const GcnInst& inst, const InstOperand& dst) {
    switch (inst.opcode) {
    case Opcode::S_LOAD_DWORD:
    case Opcode::S_BUFFER_LOAD_DWORD:
    case Opcode::V_READLANE_B32:
    case Opcode::V_READFIRSTLANE_B32:
        return 1;
    case Opcode::S_LOAD_DWORDX2:
    case Opcode::S_BUFFER_LOAD_DWORDX2:
        return 2;
    case Opcode::S_LOAD_DWORDX4:
    case Opcode::S_BUFFER_LOAD_DWORDX4:
        return 4;
    case Opcode::S_LOAD_DWORDX8:
    case Opcode::S_BUFFER_LOAD_DWORDX8:
        return 8;
    case Opcode::S_LOAD_DWORDX16:
    case Opcode::S_BUFFER_LOAD_DWORDX16:
        return 16;
    default:
        break;
    }
    if (inst.category == InstCategory::ScalarMemory) {
        return 16;
    }
    switch (dst.type) {
    case ScalarType::Uint16:
    case ScalarType::Sint16:
    case ScalarType::Float16:
    case ScalarType::Uint32:
    case ScalarType::Sint32:
    case ScalarType::Float32:
        // Apart from the lane reads above, vector instructions write SGPR pairs (lane masks).
        return inst.category == InstCategory::ScalarALU ||
                       inst.category == InstCategory::FlowControl
                   ? 1
                   : 2;
    default:
        return 2;
    }
}

} // namespace

// A pixel shader that kills pixels and still needs derivatives afterwards keeps the live pixels
// in an SGPR pair and runs in whole quad mode, where a killed pixel goes on as a helper of its quad:
//   s_mov_b64 mask, exec
//   s_wqm_b64 exec, exec
//   ...
//   s_andn2_b64 mask, mask, killed    ; SCC: any pixel of the wave still live
//   s_cbranch_scc0 exports
//   s_and_b64 exec, exec, mask
//   s_wqm_b64 exec, exec              ; killed pixels of partly live quads run on as helpers
//   ... implicit LOD samples ...
//   s_mov_b64 exec, mask
//   exp ... vm                         ; pixels outside EXEC are discarded
// EXEC is translated per invocation, and S_WQM_B64 does nothing, so a killed pixel would skip what
// follows the branch and leave the derivatives of its quad undefined. When the mask is only
// changed by such kills and every export with the valid mask bit takes EXEC from it, a killed pixel
// is demoted to a helper invocation where it is killed and runs on, as on GCN.
void Translator::FindLiveMask(std::span<const GcnInst> inst_list) {
    if (info.hw_stage != HwStage::Fragment) {
        return;
    }
    std::vector<u32> pcs(inst_list.size());
    std::vector<u32> targets;
    u32 next_pc = 0;
    for (size_t i = 0; i < inst_list.size(); ++i) {
        const GcnInst& inst = inst_list[i];
        pcs[i] = next_pc;
        if (inst.opcode == Opcode::S_SETPC_B64 || inst.opcode == Opcode::S_SWAPPC_B64 ||
            inst.opcode == Opcode::S_MOVRELD_B32 || inst.opcode == Opcode::S_MOVRELD_B64 ||
            inst.IsFork()) {
            return;
        }
        if (inst.opcode == Opcode::S_BRANCH || inst.IsConditionalBranch()) {
            targets.push_back(inst.BranchTarget(next_pc));
        }
        next_pc += inst.length;
    }
    const auto is_target = [&](size_t index) {
        return std::ranges::find(targets, pcs[index]) != targets.end();
    };

    // The mask is saved from EXEC before the shader first changes EXEC, which it does by entering
    // whole quad mode.
    std::optional<u32> found;
    size_t save_index{};
    size_t wqm_index = inst_list.size();
    for (size_t i = 0; i < inst_list.size(); ++i) {
        const GcnInst& inst = inst_list[i];
        if ((i != 0 && is_target(i)) || inst.IsTerminateInstruction()) {
            return;
        }
        if (WritesExec(inst)) {
            wqm_index = i;
            break;
        }
        if (inst.opcode == Opcode::S_MOV_B64 && inst.dst[0].field == OperandField::ScalarGPR &&
            inst.src[0].field == OperandField::ExecLo) {
            found = inst.dst[0].code;
            save_index = i;
        }
    }
    if (!found || wqm_index == inst_list.size() || !IsWqmOfExec(inst_list[wqm_index])) {
        return;
    }
    const u32 mask = *found;
    const auto is_mask = [mask](const InstOperand& operand) {
        return operand.field == OperandField::ScalarGPR && operand.code == mask;
    };
    const auto writes_mask = [mask](const GcnInst& inst) {
        for (u32 i = 0; i < inst.dst_count; ++i) {
            const InstOperand& dst = inst.dst[i];
            if (dst.field == OperandField::ScalarGPR && dst.code < mask + 2 &&
                mask < dst.code + ScalarWriteCount(inst, dst)) {
                return true;
            }
        }
        return false;
    };
    // EXEC at an export with the valid mask bit is the set of pixels written; it has to come from
    // the mask so that the killed pixels are the ones discarded.
    const auto export_from_mask = [&](size_t export_index) {
        for (size_t i = export_index; i-- > 0;) {
            if (is_target(i + 1)) {
                return false;
            }
            const GcnInst& inst = inst_list[i];
            if (writes_mask(inst)) {
                return false;
            }
            if (!WritesExec(inst)) {
                continue;
            }
            if (inst.opcode == Opcode::S_MOV_B64 && inst.dst[0].field == OperandField::ExecLo) {
                return is_mask(inst.src[0]) || inst.src[0].field == OperandField::ConstZero;
            }
            return inst.opcode == Opcode::S_AND_B64 &&
                   inst.dst[0].field == OperandField::ExecLo &&
                   (is_mask(inst.src[0]) || is_mask(inst.src[1]));
        }
        return false;
    };

    u32 kills{};
    u32 valid_mask_exports{};
    std::vector<u32> reentries;
    for (size_t i = 0; i < inst_list.size(); ++i) {
        const GcnInst& inst = inst_list[i];
        if (i != save_index && writes_mask(inst)) {
            // Kills only: mask &= ~killed, or mask &= kept.
            const bool kill = is_mask(inst.dst[0]) &&
                              ((inst.opcode == Opcode::S_ANDN2_B64 && is_mask(inst.src[0])) ||
                               (inst.opcode == Opcode::S_AND_B64 &&
                                (is_mask(inst.src[0]) || is_mask(inst.src[1]))));
            if (!kill) {
                return;
            }
            ++kills;
        }
        if (inst.opcode == Opcode::S_AND_B64 && inst.dst[0].field == OperandField::ExecLo &&
            ((inst.src[0].field == OperandField::ExecLo && is_mask(inst.src[1])) ||
             (is_mask(inst.src[0]) && inst.src[1].field == OperandField::ExecLo)) &&
            i + 1 < inst_list.size() && IsWqmOfExec(inst_list[i + 1]) && !is_target(i + 1)) {
            reentries.push_back(pcs[i]);
        }
        if (inst.category == InstCategory::Export && inst.control.exp.vm) {
            if (!export_from_mask(i)) {
                return;
            }
            ++valid_mask_exports;
        }
    }
    if (kills == 0 || valid_mask_exports == 0) {
        return;
    }
    live_mask = mask;
    wqm_reentry_pcs = std::move(reentries);
    LOG_DEBUG(Render_Recompiler,
              "Shader {:#x}: live pixel mask s[{}:{}], {} kills demoted to helpers, {} returns to "
              "whole quad mode",
              info.pgm_hash, mask, mask + 1, kills, wqm_reentry_pcs.size());
}

void Translator::Translate(IR::Block* block, u32 start_pc, IR::Condition cond,
                           std::span<const GcnInst> inst_list) {
    if (inst_list.empty()) {
        return;
    }
    ir = IR::IREmitter{*block, block->begin()};
    pc = start_pc;
    for (size_t index = 0; index < inst_list.size(); ++index) {
        if (const size_t count = TranslateWaveReduction(inst_list, index); count != 0) {
            index += count - 1;
            continue;
        }
        const auto& inst = inst_list[index];
        pc += inst.length;

        // Special case for emitting fetch shader.
        if (inst.opcode == Opcode::S_SWAPPC_B64) {
            ASSERT(info.hw_stage == HwStage::Vertex || info.hw_stage == HwStage::Export ||
                   info.hw_stage == HwStage::Local);
            EmitFetch(inst);
            continue;
        }

        TranslateInstruction(inst);
    }
    if (cond != IR::Condition::True && cond != IR::Condition::False) {
        block->branch_cond = ir.ConditionRef(ir.Condition(cond));
    }
}

namespace {

bool IsSchedulingHint(const GcnInst& inst) {
    return inst.opcode == Opcode::S_WAITCNT || inst.opcode == Opcode::S_NOP;
}

std::optional<u32> InlineInteger(const InstOperand& operand) {
    switch (operand.field) {
    case OperandField::ConstZero:
        return 0u;
    case OperandField::SignedConstIntPos:
        return operand.code - SignedConstIntPosMin + 1;
    case OperandField::SignedConstIntNeg:
        return static_cast<u32>(-static_cast<s32>(operand.code) + SignedConstIntNegMin - 1);
    case OperandField::LiteralConst:
        return operand.code;
    default:
        return std::nullopt;
    }
}

bool IsPlainOperand(const InstOperand& operand) {
    return !operand.input_modifier.neg && !operand.input_modifier.abs &&
           !operand.input_modifier.sext && !operand.dpp &&
           operand.sdwa_sel == SdwaSelector::Invalid;
}

bool IsPlainVgpr(const InstOperand& operand, u32 reg) {
    return operand.field == OperandField::VectorGPR && operand.code == reg &&
           IsPlainOperand(operand);
}

bool WritesPlainVgpr(const GcnInst& inst) {
    const auto& dst = inst.dst[0];
    return dst.field == OperandField::VectorGPR && !dst.output_modifier.clamp &&
           dst.output_modifier.multiplier == 0.f && dst.sdwa_sel == SdwaSelector::Invalid;
}

struct WaveReduction {
    IR::ReduceOp op;
    u32 identity;
};

std::optional<WaveReduction> ReductionOf(Opcode opcode) {
    switch (opcode) {
    case Opcode::V_MIN_U32:
        return WaveReduction{IR::ReduceOp::UMin, 0xFFFFFFFFu};
    case Opcode::V_MAX_U32:
        return WaveReduction{IR::ReduceOp::UMax, 0u};
    case Opcode::V_MIN_I32:
        return WaveReduction{IR::ReduceOp::SMin, 0x7FFFFFFFu};
    case Opcode::V_MAX_I32:
        return WaveReduction{IR::ReduceOp::SMax, 0x80000000u};
    case Opcode::V_AND_B32:
        return WaveReduction{IR::ReduceOp::And, 0xFFFFFFFFu};
    case Opcode::V_OR_B32:
        return WaveReduction{IR::ReduceOp::Or, 0u};
    case Opcode::V_XOR_B32:
        return WaveReduction{IR::ReduceOp::Xor, 0u};
    default:
        return std::nullopt;
    }
}

} // Anonymous namespace

// The PS4 shader compiler reduces a value across a wave by enabling every lane
// (s_orn2_saveexec_b64 SAVE, exec), filling the lanes outside SAVE with the operation's
// identity (v_cndmask_b32 V, identity, X, SAVE), combining each 32-lane half with a
// DS_SWIZZLE xor butterfly (16, 8, 4, 2, 1) and reading lanes such as 31 and 63.
// Invocations that left the SPIR-V control flow earlier (lanes the guest had disabled)
// cannot take part in that butterfly: a shuffle from them is undefined, and an undefined
// minimum stalls the scalarization loops built on it forever. Lanes outside SAVE only ever
// contribute the identity, so the same results are the reductions over the invocations
// still present. Returns the number of instructions translated, or 0 when the sequence
// does not match exactly.
size_t Translator::TranslateWaveReduction(std::span<const GcnInst> list, size_t start) {
    if (!profile.supports_subgroup_clustered_reduce || profile.subgroup_size < 64) {
        return 0;
    }
    const GcnInst& enable = list[start];
    if (enable.opcode != Opcode::S_ORN2_SAVEEXEC_B64 ||
        enable.src[0].field != OperandField::ExecLo) {
        return 0;
    }
    const InstOperand& saved = enable.dst[0];

    size_t index = start + 1;
    const auto skip_hints = [&] {
        while (index < list.size() && IsSchedulingHint(list[index])) {
            ++index;
        }
        return index < list.size();
    };

    if (!skip_hints()) {
        return 0;
    }
    const GcnInst& fill = list[index++];
    const bool fill_uses_saved =
        fill.src[2].field == OperandField::Undefined
            ? saved.field == OperandField::VccLo
            : fill.src[2].field == saved.field && fill.src[2].code == saved.code;
    // RE Engine also intersects EXEC with an earlier coverage mask before
    // enabling the wave. The fill then uses that subset, rather than SAVE.
    // Prove the nearby producer and permit only instructions that cannot change
    // EXEC, SCC, VCC or the scalar pair in between. An arbitrary mask is unsafe:
    // absent Vulkan invocations might otherwise contribute non-identity values.
    bool fill_uses_exec_subset = false;
    const auto& mask = fill.src[2];
    const bool scalar_mask =
        mask.field == OperandField::ScalarGPR || mask.field == OperandField::VccLo;
    const bool overlaps_saved = mask.field == saved.field &&
                                (mask.field == OperandField::ScalarGPR
                                     ? (mask.code < saved.code + 2 && saved.code < mask.code + 2)
                                     : true);
    if (!fill_uses_saved && scalar_mask && !overlaps_saved && IsPlainOperand(mask)) {
        size_t previous = start;
        while (previous > 0 && start - previous < 8) {
            const auto& producer = list[--previous];
            if (IsSchedulingHint(producer) ||
                (producer.opcode == Opcode::V_MUL_U32_U24 && WritesPlainVgpr(producer))) {
                continue;
            }
            fill_uses_exec_subset = producer.opcode == Opcode::S_AND_B64 &&
                                    producer.dst[0].field == mask.field &&
                                    producer.dst[0].code == mask.code &&
                                    (producer.src[0].field == OperandField::ExecLo ||
                                     producer.src[1].field == OperandField::ExecLo);
            break;
        }
    }
    const auto fill_identity = InlineInteger(fill.src[0]);
    if (fill.opcode != Opcode::V_CNDMASK_B32 || !WritesPlainVgpr(fill) ||
        (!fill_uses_saved && !fill_uses_exec_subset) || !fill_identity ||
        !IsPlainOperand(fill.src[1])) {
        return 0;
    }
    const u32 value_reg = fill.dst[0].code;

    std::optional<WaveReduction> reduction;
    std::optional<u32> temp_reg;
    u32 xor_masks = 0;
    for (u32 step = 0; step < 5; ++step) {
        if (!skip_hints()) {
            return 0;
        }
        const GcnInst& swizzle = list[index++];
        if (swizzle.opcode != Opcode::DS_SWIZZLE_B32 || swizzle.control.ds.gds) {
            return 0;
        }
        const u32 offset0 = swizzle.control.ds.offset0;
        const u32 offset1 = swizzle.control.ds.offset1;
        const u32 and_mask = offset0 & 0x1f;
        const u32 or_mask = (offset0 >> 5) | ((offset1 & 0x3) << 3);
        const u32 xor_mask = offset1 >> 2;
        const bool bit_mode = (offset1 & 0x80) == 0;
        if (!bit_mode || and_mask != 0x1f || or_mask != 0 || !std::has_single_bit(xor_mask) ||
            xor_mask > 16 || (xor_masks & xor_mask) != 0 ||
            !IsPlainVgpr(swizzle.src[0], value_reg) ||
            swizzle.dst[0].field != OperandField::VectorGPR ||
            swizzle.dst[0].code == value_reg || (temp_reg && *temp_reg != swizzle.dst[0].code)) {
            return 0;
        }
        xor_masks |= xor_mask;
        temp_reg = swizzle.dst[0].code;

        if (!skip_hints()) {
            return 0;
        }
        const GcnInst& combine = list[index++];
        const auto combine_reduction = ReductionOf(combine.opcode);
        const bool combines_pair =
            (IsPlainVgpr(combine.src[0], value_reg) && IsPlainVgpr(combine.src[1], *temp_reg)) ||
            (IsPlainVgpr(combine.src[0], *temp_reg) && IsPlainVgpr(combine.src[1], value_reg));
        if (!combine_reduction || (reduction && reduction->op != combine_reduction->op) ||
            !WritesPlainVgpr(combine) || combine.dst[0].code != value_reg || !combines_pair) {
            return 0;
        }
        reduction = combine_reduction;
    }
    if (xor_masks != 0x1f || reduction->identity != *fill_identity) {
        return 0;
    }
    const size_t butterfly_end = index;

    // Constant lane reads of the reduced value that follow the butterfly.
    size_t reads_end = index;
    while (skip_hints()) {
        const GcnInst& read = list[index];
        const auto lane = InlineInteger(read.src[1]);
        if (read.opcode != Opcode::V_READLANE_B32 || !IsPlainVgpr(read.src[0], value_reg) ||
            !lane || *lane > 63) {
            break;
        }
        reads_end = ++index;
    }

    // The enable, the fill and the butterfly still translate as before: they are exact for
    // the present invocations and keep defining the swizzle temporary. Only the reduced
    // value and the lane reads are replaced.
    std::optional<IR::U32> fill_value;
    std::optional<IR::U32> lane_id;
    std::array<std::optional<IR::U32>, 2> half_values{};
    for (size_t i = start; i < reads_end; ++i) {
        const GcnInst& inst = list[i];
        pc += inst.length;
        if (i >= butterfly_end && inst.opcode == Opcode::V_READLANE_B32) {
            const u32 half = *InlineInteger(inst.src[1]) >> 5;
            auto& half_value = half_values[half];
            if (!half_value) {
                if (!lane_id) {
                    lane_id = ir.LaneId();
                }
                const IR::U1 in_half =
                    ir.IEqual(ir.BitwiseAnd(*lane_id, ir.Imm32(32u)), ir.Imm32(half << 5));
                const IR::U32 contribution{
                    ir.Select(in_half, *fill_value, ir.Imm32(reduction->identity))};
                half_value = ir.ClusteredReduce(reduction->op, contribution, 64);
            }
            SetDst(inst.dst[0], *half_value);
            continue;
        }
        TranslateInstruction(inst);
        if (&inst == &fill) {
            fill_value = GetSrc(fill.dst[0]);
        }
        if (i + 1 == butterfly_end) {
            SetDst(fill.dst[0], ir.ClusteredReduce(reduction->op, *fill_value, 32));
        }
    }
    return reads_end - start;
}

void Translator::TranslateInstruction(const GcnInst& inst) {
    // Emit instructions for each category.
    switch (inst.category) {
    case InstCategory::DataShare:
        EmitDataShare(inst);
        break;
    case InstCategory::VectorInterpolation:
        EmitVectorInterpolation(inst);
        break;
    case InstCategory::ScalarMemory:
        EmitScalarMemory(inst);
        break;
    case InstCategory::VectorMemory:
        EmitVectorMemory(inst);
        break;
    case InstCategory::Export:
        EmitExport(inst);
        break;
    case InstCategory::FlowControl:
        EmitFlowControl(inst);
        break;
    case InstCategory::ScalarALU:
        EmitScalarAlu(inst);
        break;
    case InstCategory::VectorALU:
        EmitVectorAlu(inst);
        break;
    case InstCategory::DebugProfile:
        break;
    default:
        UNREACHABLE();
    }
}

} // namespace Shader::Gcn
