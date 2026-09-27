using System.Globalization;
using System.Text.RegularExpressions;

namespace GcnDisasm;

/// Register, constant and operand-width formatting for GCN2 (Sea Islands / PS4) operands.
internal static partial class Operands {
    public const uint Literal = 255;
    public const uint Sdwa = 249;
    public const uint Dpp = 250;

    /// An 8-bit scalar operand code (SGPRs, special registers, inline constants).
    public static string Scalar(uint code, int width = 1, uint? literal = null) {
        if (code <= 103) {
            return Range("s", code, width);
        }
        return code switch {
            104 or 105 => Pair("flat_scratch", code - 104, width),
            106 or 107 => Pair("vcc", code - 106, width),
            108 or 109 => Pair("tba", code - 108, width),
            110 or 111 => Pair("tma", code - 110, width),
            >= 112 and <= 123 => Range("ttmp", code - 112, width),
            124 => "m0",
            126 or 127 => Pair("exec", code - 126, width),
            _ => Constant(code, literal),
        };
    }

    /// A 9-bit vector source operand: scalar codes below 256, VGPRs from 256.
    public static string Source(uint code, int width = 1, uint? literal = null) =>
        code >= 256 ? Vgpr(code - 256, width) : Scalar(code, width, literal);

    public static string Vgpr(uint index, int width = 1) => Range("v", index, width);

    public static string Sgpr(uint index, int width = 1) => Range("s", index, width);

    public static bool IsLiteral(uint code) => code == Literal;

    static string Range(string prefix, uint first, int width) =>
        width <= 1 ? $"{prefix}{first}" : $"{prefix}[{first}:{first + (uint)width - 1}]";

    static string Pair(string name, uint half, int width) =>
        width >= 2 && half == 0 ? name : $"{name}_{(half == 0 ? "lo" : "hi")}";

    static string Constant(uint code, uint? literal) => code switch {
        128 => "0",
        >= 129 and <= 192 => (code - 128).ToString(CultureInfo.InvariantCulture),
        >= 193 and <= 208 => (-(int)(code - 192)).ToString(CultureInfo.InvariantCulture),
        240 => "0.5",
        241 => "-0.5",
        242 => "1.0",
        243 => "-1.0",
        244 => "2.0",
        245 => "-2.0",
        246 => "4.0",
        247 => "-4.0",
        248 => "0.15915494",
        249 => "sdwa",
        250 => "dpp",
        251 => "vccz",
        252 => "execz",
        253 => "scc",
        254 => "lds_direct",
        255 => literal is { } value ? $"0x{value:x}" : "literal?",
        _ => $"src{code}",
    };

    /// Operand widths in dwords for the destination and sources of an ALU instruction.
    public readonly record struct Widths(int Dst, int Src0, int Src1, int Src2);

    // Instructions whose operands do not all share the type in their name.
    static readonly Dictionary<string, Widths> MixedWidths = new() {
        ["s_lshl_b64"] = new(2, 2, 1, 1),
        ["s_lshr_b64"] = new(2, 2, 1, 1),
        ["s_ashr_i64"] = new(2, 2, 1, 1),
        ["s_bfe_u64"] = new(2, 2, 1, 1),
        ["s_bfe_i64"] = new(2, 2, 1, 1),
        ["s_bfm_b64"] = new(2, 1, 1, 1),
        ["s_bitset0_b64"] = new(2, 1, 1, 1),
        ["s_bitset1_b64"] = new(2, 1, 1, 1),
        ["s_bitcmp0_b64"] = new(1, 2, 1, 1),
        ["s_bitcmp1_b64"] = new(1, 2, 1, 1),
        ["s_cbranch_g_fork"] = new(1, 2, 2, 1),
        ["s_cbranch_join"] = new(1, 1, 1, 1),
        ["v_lshl_b64"] = new(2, 2, 1, 1),
        ["v_lshr_b64"] = new(2, 2, 1, 1),
        ["v_ashr_i64"] = new(2, 2, 1, 1),
        ["v_lshlrev_b64"] = new(2, 1, 2, 1),
        ["v_lshrrev_b64"] = new(2, 1, 2, 1),
        ["v_ashrrev_i64"] = new(2, 1, 2, 1),
        ["v_ldexp_f64"] = new(2, 2, 1, 1),
        ["v_trig_preop_f64"] = new(2, 2, 1, 1),
        ["v_cmp_class_f64"] = new(2, 2, 1, 1),
        ["v_cmpx_class_f64"] = new(2, 2, 1, 1),
        ["v_mad_u64_u32"] = new(2, 1, 1, 2),
        ["v_mad_i64_i32"] = new(2, 1, 1, 2),
        ["v_mqsad_u32_u8"] = new(4, 2, 1, 4),
        ["v_qsad_pk_u16_u8"] = new(2, 2, 1, 2),
        ["v_mqsad_pk_u16_u8"] = new(2, 2, 1, 2),
    };

    [GeneratedRegex(@"^[fiub](8|16|24|32|64)$")]
    private static partial Regex TypeToken();

    public static Widths For(string mnemonic) {
        if (MixedWidths.TryGetValue(mnemonic, out var widths)) {
            return widths;
        }
        var types = mnemonic.Split('_').Where(t => TypeToken().IsMatch(t))
                            .Select(t => t.EndsWith("64", StringComparison.Ordinal) ? 2 : 1)
                            .ToList();
        if (types.Count == 0) {
            return new(1, 1, 1, 1);
        }
        if (types.Count >= 2) {
            // DST_SRC naming: v_cvt_f64_i32, s_bcnt1_i32_b64, v_frexp_exp_i32_f64.
            return new(types[0], types[1], types[1], types[1]);
        }
        return new(types[0], types[0], types[0], types[0]);
    }

    public static string Hex(uint value) => value < 10 ? value.ToString(CultureInfo.InvariantCulture)
                                                       : $"0x{value:x}";

    public static string FloatComment(uint bits) =>
        BitConverter.Int32BitsToSingle((int)bits).ToString("G9", CultureInfo.InvariantCulture);
}
