// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

Id EmitContext::ConstU64(u64 value) {
    return profile.support_int64 ? Constant(U64, value) : ConstU32(u32(value), u32(value >> 32));
}
Id EmitContext::PackU64(Id pair) {
    return profile.support_int64 ? OpBitcast(U64, pair) : pair;
}
Id EmitContext::UnpackU64(Id value) {
    return profile.support_int64 ? OpBitcast(U32[2], value) : value;
}
Id EmitContext::WidenU32(Id value) {
    return profile.support_int64 ? OpUConvert(U64, value)
                                 : OpCompositeConstruct(U64, value, u32_zero_value);
}
Id EmitContext::NarrowU64(Id value) {
    return profile.support_int64 ? OpUConvert(U32[1], value)
                                 : OpCompositeExtract(U32[1], value, 0U);
}
Id EmitContext::AddU64(Id a, Id b) {
    if (profile.support_int64)
        return OpIAdd(U64, a, b);
    const auto lo_a = OpCompositeExtract(U32[1], a, 0U);
    const auto lo_b = OpCompositeExtract(U32[1], b, 0U);
    const auto lo = OpIAdd(U32[1], lo_a, lo_b);
    const auto carry =
        OpSelect(U32[1], OpULessThan(U1[1], lo, lo_a), u32_one_value, u32_zero_value);
    const auto hi =
        OpIAdd(U32[1],
               OpIAdd(U32[1], OpCompositeExtract(U32[1], a, 1U), OpCompositeExtract(U32[1], b, 1U)),
               carry);
    return OpCompositeConstruct(U64, lo, hi);
}
Id EmitContext::SubU64(Id a, Id b) {
    if (profile.support_int64)
        return OpISub(U64, a, b);
    const auto lo_a = OpCompositeExtract(U32[1], a, 0U);
    const auto lo_b = OpCompositeExtract(U32[1], b, 0U);
    const auto borrow =
        OpSelect(U32[1], OpULessThan(U1[1], lo_a, lo_b), u32_one_value, u32_zero_value);
    const auto hi =
        OpISub(U32[1],
               OpISub(U32[1], OpCompositeExtract(U32[1], a, 1U), OpCompositeExtract(U32[1], b, 1U)),
               borrow);
    return OpCompositeConstruct(U64, OpISub(U32[1], lo_a, lo_b), hi);
}
Id EmitContext::MulU64(Id a, Id b) {
    if (profile.support_int64)
        return OpIMul(U64, a, b);
    const auto al = OpCompositeExtract(U32[1], a, 0U);
    const auto bl = OpCompositeExtract(U32[1], b, 0U);
    const auto product = OpUMulExtended(full_result_u32x2, al, bl);
    const auto cross = OpIAdd(U32[1], OpIMul(U32[1], al, OpCompositeExtract(U32[1], b, 1U)),
                              OpIMul(U32[1], bl, OpCompositeExtract(U32[1], a, 1U)));
    return OpCompositeConstruct(U64, OpCompositeExtract(U32[1], product, 0U),
                                OpIAdd(U32[1], OpCompositeExtract(U32[1], product, 1U), cross));
}
Id EmitContext::ShiftU64(Id value, Id shift, bool left, bool arithmetic) {
    if (profile.support_int64) {
        if (left)
            return OpShiftLeftLogical(U64, value, shift);
        return arithmetic ? OpShiftRightArithmetic(U64, value, shift)
                          : OpShiftRightLogical(U64, value, shift);
    }
    const auto lo = OpCompositeExtract(U32[1], value, 0U);
    const auto hi = OpCompositeExtract(U32[1], value, 1U);
    const auto n = OpBitwiseAnd(U32[1], shift, ConstU32(31U));
    // Both arms of OpSelect execute. Never emit a shift by 32, even in an unused arm.
    const auto inverse = OpBitwiseAnd(U32[1], OpISub(U32[1], u32_zero_value, n), ConstU32(31U));
    const auto nonzero = OpINotEqual(U1[1], n, u32_zero_value);
    const auto large = OpUGreaterThanEqual(U1[1], shift, ConstU32(32U));
    if (left) {
        const auto carry =
            OpSelect(U32[1], nonzero, OpShiftRightLogical(U32[1], lo, inverse), u32_zero_value);
        const auto l = OpShiftLeftLogical(U32[1], lo, n);
        const auto h = OpBitwiseOr(U32[1], OpShiftLeftLogical(U32[1], hi, n), carry);
        return OpCompositeConstruct(U64, OpSelect(U32[1], large, u32_zero_value, l),
                                    OpSelect(U32[1], large, l, h));
    }
    const auto carry =
        OpSelect(U32[1], nonzero, OpShiftLeftLogical(U32[1], hi, inverse), u32_zero_value);
    const auto h =
        arithmetic ? OpShiftRightArithmetic(U32[1], hi, n) : OpShiftRightLogical(U32[1], hi, n);
    const auto fill =
        arithmetic ? OpShiftRightArithmetic(U32[1], hi, ConstU32(31U)) : u32_zero_value;
    const auto l = OpBitwiseOr(U32[1], OpShiftRightLogical(U32[1], lo, n), carry);
    return OpCompositeConstruct(U64, OpSelect(U32[1], large, h, l),
                                OpSelect(U32[1], large, fill, h));
}
Id EmitContext::EqualU64(Id a, Id b) {
    if (profile.support_int64)
        return OpIEqual(U1[1], a, b);
    return OpAll(U1[1], OpIEqual(U1[2], a, b));
}
Id EmitContext::LessU64(Id a, Id b, bool is_signed) {
    if (profile.support_int64)
        return is_signed ? OpSLessThan(U1[1], a, b) : OpULessThan(U1[1], a, b);
    const auto ah = OpCompositeExtract(U32[1], a, 1U);
    const auto bh = OpCompositeExtract(U32[1], b, 1U);
    const auto high_less = is_signed ? OpSLessThan(U1[1], ah, bh) : OpULessThan(U1[1], ah, bh);
    const auto low_less =
        OpULessThan(U1[1], OpCompositeExtract(U32[1], a, 0U), OpCompositeExtract(U32[1], b, 0U));
    return OpLogicalOr(U1[1], high_less, OpLogicalAnd(U1[1], OpIEqual(U1[1], ah, bh), low_less));
}

Id EmitContext::FloatToU64(Id value, bool is_signed, bool fp64) {
    if (profile.support_int64)
        return is_signed ? OpConvertFToS(U64, value) : OpConvertFToU(U64, value);
    const auto type = fp64 ? F64[1] : F32[1];
    const auto zero = fp64 ? Constant(type, 0.0) : ConstF32(0.0f);
    const auto negative = is_signed ? OpFOrdLessThan(U1[1], value, zero) : false_value;
    const auto magnitude = is_signed ? OpFAbs(type, value) : value;
    const auto divisor = fp64 ? Constant(type, 4294967296.0) : ConstF32(4294967296.0f);
    // Power-of-two split is exact for in-range finite inputs. Out-of-range and NaN
    // conversions have undefined results in SPIR-V, as on the native path.
    const auto lo = OpConvertFToU(U32[1], OpFRem(type, magnitude, divisor));
    const auto hi = OpConvertFToU(U32[1], OpFDiv(type, magnitude, divisor));
    const auto pair = OpCompositeConstruct(U64, lo, hi);
    return OpSelect(U64, negative, SubU64(u64_zero_value, pair), pair);
}

Id EmitContext::U64ToFloat(Id value, bool is_signed, bool fp64) {
    const auto type = fp64 ? F64[1] : F32[1];
    if (profile.support_int64)
        return is_signed ? OpConvertSToF(type, value) : OpConvertUToF(type, value);
    const auto negative = is_signed ? LessU64(value, u64_zero_value, true) : false_value;
    const auto magnitude = OpSelect(U64, negative, SubU64(u64_zero_value, value), value);
    const auto hi = OpCompositeExtract(U32[1], magnitude, 1U);
    const auto lo = OpCompositeExtract(U32[1], magnitude, 0U);
    const auto msb =
        OpSelect(U32[1], OpINotEqual(U1[1], hi, u32_zero_value),
                 OpIAdd(U32[1], OpFindUMsb(U32[1], hi), ConstU32(32U)), OpFindUMsb(U32[1], lo));
    const auto fraction_bits = ConstU32(fp64 ? 52U : 23U);
    const auto discard = OpSMax(U32[1], OpISub(U32[1], msb, fraction_bits), u32_zero_value);
    auto significand = ShiftU64(magnitude, discard, false);
    const auto unit = ShiftU64(u64_one_value, discard, true);
    const auto remainder = OpBitwiseAnd(U64, magnitude, SubU64(unit, u64_one_value));
    const auto half = ShiftU64(unit, u32_one_value, false);
    const auto odd = OpINotEqual(U1[1], OpBitwiseAnd(U32[1], NarrowU64(significand), u32_one_value),
                                 u32_zero_value);
    const auto halfway =
        OpLogicalAnd(U1[1], EqualU64(remainder, half), OpINotEqual(U1[1], discard, u32_zero_value));
    auto round_up = OpLogicalOr(U1[1], LessU64(half, remainder), OpLogicalAnd(U1[1], halfway, odd));
    const auto mode = fp64 ? runtime_info.fp_round_mode16_64 : runtime_info.fp_round_mode32;
    if (mode == AmdGpu::FpRoundMode::ToZero) {
        round_up = false_value;
    } else if (mode == AmdGpu::FpRoundMode::PlusInf || mode == AmdGpu::FpRoundMode::MinInf) {
        const auto direction =
            mode == AmdGpu::FpRoundMode::MinInf ? negative : OpLogicalNot(U1[1], negative);
        round_up = OpLogicalAnd(U1[1], direction,
                                OpLogicalNot(U1[1], EqualU64(remainder, u64_zero_value)));
    }
    significand =
        AddU64(significand, WidenU32(OpSelect(U32[1], round_up, u32_one_value, u32_zero_value)));
    const auto normalize = OpSMax(U32[1], OpISub(U32[1], fraction_bits, msb), u32_zero_value);
    significand = ShiftU64(significand, normalize, true);
    const auto top = fp64 ? OpCompositeExtract(U32[1], significand, 1U) : NarrowU64(significand);
    const auto carry =
        OpSelect(U32[1], OpUGreaterThanEqual(U1[1], top, ConstU32(fp64 ? (1U << 21) : (1U << 24))),
                 u32_one_value, u32_zero_value);
    significand = ShiftU64(significand, carry, false);
    const auto exponent =
        OpSelect(U32[1], EqualU64(magnitude, u64_zero_value), u32_zero_value,
                 OpIAdd(U32[1], OpIAdd(U32[1], msb, carry), ConstU32(fp64 ? 1023U : 127U)));
    const auto mantissa =
        fp64 ? OpCompositeExtract(U32[1], significand, 1U) : NarrowU64(significand);
    auto bits =
        OpBitwiseOr(U32[1], OpBitwiseAnd(U32[1], mantissa, ConstU32(fp64 ? 0xfffffU : 0x7fffffU)),
                    OpShiftLeftLogical(U32[1], exponent, ConstU32(fp64 ? 20U : 23U)));
    bits = OpBitwiseOr(U32[1], bits,
                       OpSelect(U32[1], negative, ConstU32(0x80000000U), u32_zero_value));
    return OpBitcast(type,
                     fp64 ? OpCompositeConstruct(U32[2], NarrowU64(significand), bits) : bits);
}
} // namespace Shader::Backend::SPIRV
