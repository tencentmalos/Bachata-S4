using System.Numerics;

namespace GcnDisasm;

/// Decodes GCN2 (Sea Islands, PS4 Liverpool) machine code, including the PS4 Pro (neo)
/// extensions the emulator decodes: the extra VOP3 opcode bit, SDWA/DPP and VOP3P.
public static class Decoder {
    public static Encoding GetEncoding(uint word) {
        switch (word >> 23) {
        case 0x17D: return Encoding.SOP1;
        case 0x17F: return Encoding.SOPP;
        case 0x17E: return Encoding.SOPC;
        }
        switch (word >> 25) {
        case 0x3F: return Encoding.VOP1;
        case 0x3E: return Encoding.VOPC;
        }
        switch (word >> 26) {
        case 0x34: return Encoding.VOP3;
        case 0x33: return Encoding.VOP3P;
        case 0x3E: return Encoding.EXP;
        case 0x32: return Encoding.VINTRP;
        case 0x36: return Encoding.DS;
        case 0x38: return Encoding.MUBUF;
        case 0x3A: return Encoding.MTBUF;
        case 0x3C: return Encoding.MIMG;
        }
        if (word >> 27 == 0x18) return Encoding.SMRD;
        if (word >> 28 == 0xB) return Encoding.SOPK;
        if (word >> 30 == 0x2) return Encoding.SOP2;
        if (word >> 31 == 0) return Encoding.VOP2;
        return Encoding.Illegal;
    }

    /// Decodes the instruction at dword index. Never reads past code; truncated instructions are
    /// reported as .word.
    public static Instruction Decode(ReadOnlySpan<uint> code, int index) {
        var word = code[index];
        var encoding = GetEncoding(word);
        var inst = new Instruction { Offset = index * 4, Encoding = encoding };
        var reader = new Reader(code, index);
        try {
            switch (encoding) {
            case Encoding.SOP2: DecodeSop2(inst, ref reader); break;
            case Encoding.SOPK: DecodeSopk(inst, ref reader); break;
            case Encoding.SOP1: DecodeSop1(inst, ref reader); break;
            case Encoding.SOPC: DecodeSopc(inst, ref reader); break;
            case Encoding.SOPP: DecodeSopp(inst, ref reader); break;
            case Encoding.SMRD: DecodeSmrd(inst, ref reader); break;
            case Encoding.VOP2: DecodeVop2(inst, ref reader); break;
            case Encoding.VOP1: DecodeVop1(inst, ref reader); break;
            case Encoding.VOPC: DecodeVopc(inst, ref reader); break;
            case Encoding.VOP3: DecodeVop3(inst, ref reader); break;
            case Encoding.VOP3P: DecodeVop3p(inst, ref reader); break;
            case Encoding.VINTRP: DecodeVintrp(inst, ref reader); break;
            case Encoding.DS: DecodeDs(inst, ref reader); break;
            case Encoding.MUBUF: DecodeMubuf(inst, ref reader); break;
            case Encoding.MTBUF: DecodeMtbuf(inst, ref reader); break;
            case Encoding.MIMG: DecodeMimg(inst, ref reader); break;
            case Encoding.EXP: DecodeExp(inst, ref reader); break;
            default: return Word(inst, word);
            }
        } catch (IndexOutOfRangeException) {
            return Word(new Instruction { Offset = index * 4, Encoding = Encoding.Illegal }, word);
        }
        inst.Words = reader.Consumed();
        return inst;
    }

    static Instruction Word(Instruction inst, uint word) {
        inst.Mnemonic = ".word";
        inst.Operands.Clear();
        inst.Operands.Add($"0x{word:x8}");
        inst.Words = [word];
        inst.Unknown = true;
        return inst;
    }

    ref struct Reader(ReadOnlySpan<uint> code, int start) {
        readonly ReadOnlySpan<uint> code = code;
        int count;

        public uint Next() => code[start + count++];
        public uint[] Consumed() => code.Slice(start, count).ToArray();
    }

    static string Name(Instruction inst, string?[] table, uint op, string encoding) {
        inst.Op = op;
        if (op < table.Length && table[op] is { } name) {
            return name;
        }
        inst.Unknown = true;
        return $"{encoding.ToLowerInvariant()}_op{op}";
    }

    static (int Src, int Dst)? Counts(byte[] table, uint op) =>
        op < table.Length && table[op] != 0xff ? (table[op] & 0xf, table[op] >> 4) : null;

    // Scalar ALU

    static void DecodeSop2(Instruction inst, ref Reader reader) {
        var word = reader.Next();
        var op = (word >> 23) & 0x7f;
        var sdst = (word >> 16) & 0x7f;
        var ssrc1 = (word >> 8) & 0xff;
        var ssrc0 = word & 0xff;
        uint? literal = Operands.IsLiteral(ssrc0) || Operands.IsLiteral(ssrc1) ? reader.Next() : null;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.SOP2, op, "sop2");
        var w = Operands.For(name);
        if (name != "s_cbranch_g_fork") {
            inst.Operands.Add(Operands.Scalar(sdst, w.Dst));
        }
        inst.Operands.Add(Operands.Scalar(ssrc0, w.Src0, literal));
        inst.Operands.Add(Operands.Scalar(ssrc1, w.Src1, literal));
    }

    static readonly string[] HwRegs = [
        "0", "HW_REG_MODE", "HW_REG_STATUS", "HW_REG_TRAPSTS", "HW_REG_HW_ID", "HW_REG_GPR_ALLOC",
        "HW_REG_LDS_ALLOC", "HW_REG_IB_STS", "HW_REG_PC_LO", "HW_REG_PC_HI", "HW_REG_INST_DW0",
        "HW_REG_INST_DW1", "HW_REG_IB_DBG0",
    ];

    static string HwReg(uint simm) {
        var id = simm & 0x3f;
        var offset = (simm >> 6) & 0x1f;
        var size = ((simm >> 11) & 0x1f) + 1;
        var name = id < HwRegs.Length && id != 0 ? HwRegs[id] : id.ToString();
        return offset == 0 && size == 32 ? $"hwreg({name})" : $"hwreg({name}, {offset}, {size})";
    }

    static void DecodeSopk(Instruction inst, ref Reader reader) {
        var word = reader.Next();
        var op = (word >> 23) & 0x1f;
        var sdst = (word >> 16) & 0x7f;
        var simm = word & 0xffff;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.SOPK, op, "sopk");
        switch (name) {
        case "s_getreg_b32":
            inst.Operands.Add(Operands.Scalar(sdst));
            inst.Operands.Add(HwReg(simm));
            break;
        case "s_setreg_b32":
            inst.Operands.Add(HwReg(simm));
            inst.Operands.Add(Operands.Scalar(sdst));
            break;
        case "s_setreg_imm32_b32":
            inst.Operands.Add(HwReg(simm));
            inst.Operands.Add($"0x{reader.Next():x}");
            break;
        case "s_cbranch_i_fork":
            inst.Operands.Add(Operands.Scalar(sdst, 2));
            SetBranch(inst, simm, conditional: true);
            break;
        default:
            inst.Operands.Add(Operands.Scalar(sdst));
            inst.Operands.Add($"0x{simm:x}");
            break;
        }
    }

    static void DecodeSop1(Instruction inst, ref Reader reader) {
        var word = reader.Next();
        var op = (word >> 8) & 0xff;
        var sdst = (word >> 16) & 0x7f;
        var ssrc0 = word & 0xff;
        uint? literal = Operands.IsLiteral(ssrc0) ? reader.Next() : null;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.SOP1, op, "sop1");
        var w = Operands.For(name);
        var counts = Counts(OpcodeTables.SOP1Counts, op);
        var hasDst = name is not ("s_setpc_b64" or "s_rfe_b64" or "s_cbranch_join" or
                                  "s_set_gpr_idx_idx") && (counts?.Dst ?? 1) > 0;
        var hasSrc = name != "s_getpc_b64" && (counts?.Src ?? 1) > 0;
        if (hasDst) inst.Operands.Add(Operands.Scalar(sdst, w.Dst));
        if (hasSrc) inst.Operands.Add(Operands.Scalar(ssrc0, w.Src0, literal));
        // Fetch shaders and other subroutines return with s_setpc_b64.
        inst.EndsProgram = name == "s_setpc_b64";
    }

    static void DecodeSopc(Instruction inst, ref Reader reader) {
        var word = reader.Next();
        var op = (word >> 16) & 0x7f;
        var ssrc1 = (word >> 8) & 0xff;
        var ssrc0 = word & 0xff;
        uint? literal = Operands.IsLiteral(ssrc0) || Operands.IsLiteral(ssrc1) ? reader.Next() : null;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.SOPC, op, "sopc");
        var w = Operands.For(name);
        inst.Operands.Add(Operands.Scalar(ssrc0, w.Src0, literal));
        inst.Operands.Add(Operands.Scalar(ssrc1, w.Src1, literal));
    }

    static readonly string[] Messages = [
        "0", "MSG_INTERRUPT", "MSG_GS", "MSG_GS_DONE", "4", "5", "6", "7", "8", "9", "10", "11",
        "12", "13", "14", "MSG_SYSMSG",
    ];
    static readonly string[] GsOps = ["GS_OP_NOP", "GS_OP_CUT", "GS_OP_EMIT", "GS_OP_EMIT_CUT"];

    static void DecodeSopp(Instruction inst, ref Reader reader) {
        var word = reader.Next();
        var op = (word >> 16) & 0x7f;
        var simm = word & 0xffff;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.SOPP, op, "sopp");
        switch (name) {
        case "s_endpgm":
            inst.EndsProgram = true;
            break;
        case "s_branch":
            SetBranch(inst, simm, conditional: false);
            break;
        case var branch when branch.StartsWith("s_cbranch_", StringComparison.Ordinal):
            SetBranch(inst, simm, conditional: true);
            break;
        case "s_waitcnt": {
            var vm = simm & 0xf;
            var exp = (simm >> 4) & 0x7;
            var lgkm = (simm >> 8) & 0xf;
            var parts = new List<string>();
            if (vm != 0xf) parts.Add($"vmcnt({vm})");
            if (exp != 0x7) parts.Add($"expcnt({exp})");
            if (lgkm != 0xf) parts.Add($"lgkmcnt({lgkm})");
            inst.Operands.Add(parts.Count == 0 ? $"0x{simm:x}" : string.Join(" ", parts));
            break;
        }
        case "s_sendmsg" or "s_sendmsghalt": {
            var msg = simm & 0xf;
            var gsOp = (simm >> 4) & 0x7;
            var stream = (simm >> 8) & 0x3;
            inst.Operands.Add(msg is 2 or 3
                ? $"sendmsg({Messages[msg]}, {GsOps[gsOp & 3]}, {stream})"
                : $"sendmsg({Messages[msg]})");
            break;
        }
        case "s_barrier" or "s_icache_inv" or "s_ttracedata" or "s_incperflevel" or
             "s_decperflevel" when simm == 0:
            break;
        default:
            inst.Operands.Add(Operands.Hex(simm));
            break;
        }
    }

    static void SetBranch(Instruction inst, uint simm, bool conditional) {
        var target = inst.Offset + 4 + (short)simm * 4;
        inst.BranchTarget = target;
        inst.IsConditionalBranch = conditional;
        inst.Operands.Add($"0x{target:x}");
    }

    // Scalar memory

    static void DecodeSmrd(Instruction inst, ref Reader reader) {
        var word = reader.Next();
        var op = (word >> 22) & 0x1f;
        var sdst = (word >> 15) & 0x7f;
        var sbase = ((word >> 9) & 0x3f) * 2;
        var imm = (word >> 8) & 1;
        var offset = word & 0xff;
        uint? literal = imm == 0 && Operands.IsLiteral(offset) ? reader.Next() : null;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.SMRD, op, "smrd");
        if (name is "s_dcache_inv" or "s_dcache_inv_vol") {
            return;
        }
        if (name == "s_memtime") {
            inst.Operands.Add(Operands.Scalar(sdst, 2));
            return;
        }
        var width = name.EndsWith("x16", StringComparison.Ordinal) ? 16
                  : name.EndsWith("x8", StringComparison.Ordinal) ? 8
                  : name.EndsWith("x4", StringComparison.Ordinal) ? 4
                  : name.EndsWith("x2", StringComparison.Ordinal) ? 2 : 1;
        inst.Operands.Add(Operands.Scalar(sdst, width));
        inst.Operands.Add(Operands.Sgpr(sbase, name.StartsWith("s_buffer", StringComparison.Ordinal) ? 4 : 2));
        // Immediate offsets are in dwords, as in LLVM's SI/CI syntax.
        inst.Operands.Add(imm != 0 ? $"0x{offset:x}" : Operands.Scalar(offset, 1, literal));
        if (imm != 0) {
            inst.Comments.Add($"+{offset * 4} bytes");
        }
    }

    // Vector ALU

    /// Reads the extra dword of an SDWA/DPP/literal source and returns the real src0 code.
    static uint Src0Extension(Instruction inst, ref Reader reader, uint src0, bool vopc,
                              out uint? literal, out uint extension) {
        literal = null;
        extension = 0;
        switch (src0) {
        case Operands.Literal:
            literal = reader.Next();
            return src0;
        case Operands.Sdwa: {
            var sdwa = extension = reader.Next();
            string[] sel = ["BYTE_0", "BYTE_1", "BYTE_2", "BYTE_3", "WORD_0", "WORD_1", "DWORD", "7"];
            if (!vopc) {
                inst.Modifiers.Add($"dst_sel:{sel[(sdwa >> 8) & 7]}");
                inst.Modifiers.Add($"dst_unused:{new[] { "UNUSED_PAD", "UNUSED_SEXT", "UNUSED_PRESERVE", "3" }[(sdwa >> 11) & 3]}");
                if (((sdwa >> 13) & 1) != 0) inst.Modifiers.Add("clamp");
            }
            inst.Modifiers.Add($"src0_sel:{sel[(sdwa >> 16) & 7]}");
            inst.Modifiers.Add($"src1_sel:{sel[(sdwa >> 24) & 7]}");
            if (((sdwa >> 19) & 1) != 0) inst.Modifiers.Add("src0_sext");
            if (((sdwa >> 20) & 1) != 0) inst.Modifiers.Add("src0_neg");
            if (((sdwa >> 21) & 1) != 0) inst.Modifiers.Add("src0_abs");
            if (((sdwa >> 27) & 1) != 0) inst.Modifiers.Add("src1_sext");
            if (((sdwa >> 28) & 1) != 0) inst.Modifiers.Add("src1_neg");
            if (((sdwa >> 29) & 1) != 0) inst.Modifiers.Add("src1_abs");
            return 256 + (sdwa & 0xff);
        }
        case Operands.Dpp: {
            var dpp = extension = reader.Next();
            var ctrl = (dpp >> 8) & 0x1ff;
            inst.Modifiers.Add(ctrl < 0x100
                ? $"quad_perm:[{ctrl & 3},{(ctrl >> 2) & 3},{(ctrl >> 4) & 3},{(ctrl >> 6) & 3}]"
                : $"dpp_ctrl:0x{ctrl:x}");
            inst.Modifiers.Add($"row_mask:0x{(dpp >> 28) & 0xf:x}");
            inst.Modifiers.Add($"bank_mask:0x{(dpp >> 24) & 0xf:x}");
            if (((dpp >> 19) & 1) != 0) inst.Modifiers.Add("bound_ctrl:0");
            if (((dpp >> 20) & 1) != 0) inst.Modifiers.Add("src0_neg");
            if (((dpp >> 21) & 1) != 0) inst.Modifiers.Add("src0_abs");
            if (((dpp >> 22) & 1) != 0) inst.Modifiers.Add("src1_neg");
            if (((dpp >> 23) & 1) != 0) inst.Modifiers.Add("src1_abs");
            return 256 + (dpp & 0xff);
        }
        default:
            return src0;
        }
    }

    static void LiteralComment(Instruction inst, uint? literal, string name) {
        if (literal is { } value && name.Contains("_f32", StringComparison.Ordinal)) {
            inst.Comments.Add($"0x{value:x} = {Operands.FloatComment(value)}");
        }
    }

    static bool IsCarryOut(string name) =>
        name is "v_add_i32" or "v_sub_i32" or "v_subrev_i32" or "v_addc_u32" or "v_subb_u32" or
                "v_subbrev_u32" or "v_add_co_u32" or "v_sub_co_u32" or "v_subrev_co_u32" or
                "v_addc_co_u32" or "v_subb_co_u32" or "v_subbrev_co_u32";

    static bool IsCarryIn(string name) =>
        name is "v_addc_u32" or "v_subb_u32" or "v_subbrev_u32" or "v_addc_co_u32" or
                "v_subb_co_u32" or "v_subbrev_co_u32";

    static void DecodeVop2(Instruction inst, ref Reader reader) {
        var word = reader.Next();
        var op = (word >> 25) & 0x3f;
        var vdst = (word >> 17) & 0xff;
        var vsrc1 = (word >> 9) & 0xff;
        var src0 = Src0Extension(inst, ref reader, word & 0x1ff, false, out var literal, out _);
        var name = inst.Mnemonic = Name(inst, OpcodeTables.VOP2, op, "vop2");
        var w = Operands.For(name);
        switch (name) {
        case "v_readlane_b32":
            inst.Operands.Add(Operands.Scalar(vdst));
            inst.Operands.Add(Operands.Source(src0));
            inst.Operands.Add(Operands.Scalar(vsrc1));
            return;
        case "v_writelane_b32":
            inst.Operands.Add(Operands.Vgpr(vdst));
            inst.Operands.Add(Operands.Source(src0, 1, literal));
            inst.Operands.Add(Operands.Scalar(vsrc1));
            return;
        case "v_madmk_f32" or "v_madmk_f16": {
            var k = reader.Next();
            inst.Operands.Add(Operands.Vgpr(vdst));
            inst.Operands.Add(Operands.Source(src0, 1, literal));
            inst.Operands.Add($"0x{k:x}");
            inst.Operands.Add(Operands.Vgpr(vsrc1));
            LiteralComment(inst, k, name);
            return;
        }
        case "v_madak_f32" or "v_madak_f16": {
            var k = reader.Next();
            inst.Operands.Add(Operands.Vgpr(vdst));
            inst.Operands.Add(Operands.Source(src0, 1, literal));
            inst.Operands.Add(Operands.Vgpr(vsrc1));
            inst.Operands.Add($"0x{k:x}");
            LiteralComment(inst, k, name);
            return;
        }
        }
        inst.Operands.Add(Operands.Vgpr(vdst, w.Dst));
        if (IsCarryOut(name)) inst.Operands.Add("vcc");
        inst.Operands.Add(Operands.Source(src0, w.Src0, literal));
        inst.Operands.Add(Operands.Vgpr(vsrc1, w.Src1));
        if (name == "v_cndmask_b32" || IsCarryIn(name)) inst.Operands.Add("vcc");
        LiteralComment(inst, literal, name);
    }

    static void DecodeVop1(Instruction inst, ref Reader reader) {
        var word = reader.Next();
        var op = (word >> 9) & 0xff;
        var vdst = (word >> 17) & 0xff;
        var src0 = Src0Extension(inst, ref reader, word & 0x1ff, false, out var literal, out _);
        var name = inst.Mnemonic = Name(inst, OpcodeTables.VOP1, op, "vop1");
        if (name is "v_nop" or "v_clrexcp") {
            return;
        }
        var w = Operands.For(name);
        inst.Operands.Add(name == "v_readfirstlane_b32" ? Operands.Scalar(vdst)
                                                        : Operands.Vgpr(vdst, w.Dst));
        inst.Operands.Add(Operands.Source(src0, w.Src0, literal));
        LiteralComment(inst, literal, name);
    }

    static void DecodeVopc(Instruction inst, ref Reader reader) {
        var word = reader.Next();
        var op = (word >> 17) & 0xff;
        var vsrc1 = (word >> 9) & 0xff;
        var raw0 = word & 0x1ff;
        var src0 = Src0Extension(inst, ref reader, raw0, true, out var literal, out var sdwa);
        var name = inst.Mnemonic = Name(inst, OpcodeTables.VOPC, op, "vopc");
        var w = Operands.For(name);
        // SDWA compares may name their destination SGPR pair instead of vcc.
        var dst = raw0 == Operands.Sdwa && ((sdwa >> 15) & 1) != 0
                      ? Operands.Scalar((sdwa >> 8) & 0x7f, 2) : "vcc";
        inst.Operands.Add(dst);
        inst.Operands.Add(Operands.Source(src0, w.Src0, literal));
        inst.Operands.Add(Operands.Vgpr(vsrc1, w.Src1));
        LiteralComment(inst, literal, name);
    }

    static bool IsVop3b(string name) =>
        IsCarryOut(name) || name is "v_div_scale_f32" or "v_div_scale_f64" or "v_mad_u64_u32" or
                                    "v_mad_i64_i32";

    static void DecodeVop3(Instruction inst, ref Reader reader) {
        var w0 = reader.Next();
        var w1 = reader.Next();
        var vdst = w0 & 0xff;
        var abs = (w0 >> 8) & 7;
        var clamp = (w0 >> 11) & 1;
        var sdst = (w0 >> 8) & 0x7f;
        var op = ((w0 >> 17) & 0x1ff) | (((w0 >> 16) & 1) << 9);
        uint[] src = [w1 & 0x1ff, (w1 >> 9) & 0x1ff, (w1 >> 18) & 0x1ff];
        var omod = (w1 >> 27) & 3;
        var neg = (w1 >> 29) & 7;

        var name = inst.Mnemonic = Name(inst, OpcodeTables.VOP3, op, "vop3");
        if (inst.Unknown) {
            // Fall back to the VOPC/VOP2/VOP1 names the VOP3 encoding promotes.
            var promoted = op < 256 ? OpcodeTables.VOPC[op]
                         : op is >= 256 and < 320 ? OpcodeTables.VOP2[op - 256]
                         : op is >= 384 and < 512 ? OpcodeTables.VOP1[op - 384] : null;
            if (promoted is not null) {
                name = inst.Mnemonic = promoted;
                inst.Unknown = false;
            }
        }
        var w = Operands.For(name);
        var vop3b = IsVop3b(name);
        var srcCount = Counts(OpcodeTables.VOP3Counts, op)?.Src
                       ?? (op < 256 ? 2 : op < 320 ? 2 : op < 384 ? 3 : op < 512 ? 1 : 3);
        int[] widths = [w.Src0, w.Src1, w.Src2];

        if (name is "v_nop" or "v_clrexcp") {
            return;
        }
        if (op < 256) {
            // Compares write a lane mask to an SGPR pair.
            inst.Operands.Add(Operands.Scalar(vdst, 2));
        } else if (name is "v_readlane_b32" or "v_readfirstlane_b32") {
            inst.Operands.Add(Operands.Scalar(vdst));
        } else {
            inst.Operands.Add(Operands.Vgpr(vdst, w.Dst));
        }
        if (vop3b) {
            inst.Operands.Add(Operands.Scalar(sdst, 2));
        }
        for (var i = 0; i < srcCount; ++i) {
            var width = widths[i];
            // Lane masks: v_cndmask's selector and the carry-in of v_addc/v_subb.
            if (i == 2 && (name == "v_cndmask_b32" || IsCarryIn(name))) {
                width = 2;
            }
            var text = Operands.Source(src[i], width);
            if (!vop3b && ((abs >> i) & 1) != 0) text = $"|{text}|";
            if (((neg >> i) & 1) != 0) text = $"-{text}";
            inst.Operands.Add(text);
        }
        if (!vop3b && clamp != 0) inst.Modifiers.Add("clamp");
        if (omod != 0) inst.Modifiers.Add(omod switch { 1 => "mul:2", 2 => "mul:4", _ => "div:2" });
    }

    static void DecodeVop3p(Instruction inst, ref Reader reader) {
        var w0 = reader.Next();
        var w1 = reader.Next();
        var vdst = w0 & 0xff;
        var negHi = (w0 >> 8) & 7;
        var opSel = (w0 >> 11) & 7;
        var opSelHi2 = (w0 >> 14) & 1;
        var clamp = (w0 >> 15) & 1;
        var op = (w0 >> 16) & 0x7f;
        uint[] src = [w1 & 0x1ff, (w1 >> 9) & 0x1ff, (w1 >> 18) & 0x1ff];
        var opSelHi = ((w1 >> 27) & 3) | (opSelHi2 << 2);
        var neg = (w1 >> 29) & 7;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.VOP3P, op, "vop3p");
        var srcCount = Counts(OpcodeTables.VOP3PCounts, op)?.Src ?? 3;
        inst.Operands.Add(Operands.Vgpr(vdst));
        for (var i = 0; i < srcCount; ++i) {
            inst.Operands.Add(Operands.Source(src[i]));
        }
        string Bits(uint value) => "[" + string.Join(",", Enumerable.Range(0, srcCount).Select(i => (value >> i) & 1)) + "]";
        if (opSel != 0) inst.Modifiers.Add($"op_sel:{Bits(opSel)}");
        if (opSelHi != 7) inst.Modifiers.Add($"op_sel_hi:{Bits(opSelHi)}");
        if (neg != 0) inst.Modifiers.Add($"neg_lo:{Bits(neg)}");
        if (negHi != 0) inst.Modifiers.Add($"neg_hi:{Bits(negHi)}");
        if (clamp != 0) inst.Modifiers.Add("clamp");
    }

    static void DecodeVintrp(Instruction inst, ref Reader reader) {
        var word = reader.Next();
        var vsrc = word & 0xff;
        var chan = (word >> 8) & 3;
        var attr = (word >> 10) & 0x3f;
        var op = (word >> 16) & 3;
        var vdst = (word >> 18) & 0xff;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.VINTRP, op, "vintrp");
        inst.Operands.Add(Operands.Vgpr(vdst));
        inst.Operands.Add(name == "v_interp_mov_f32"
            ? vsrc switch { 0 => "p10", 1 => "p20", 2 => "p0", _ => $"param{vsrc}" }
            : Operands.Vgpr(vsrc));
        inst.Operands.Add($"attr{attr}.{"xyzw"[(int)chan]}");
    }

    // Data share

    static int DataWidth(string name) =>
        name.Contains("_b128", StringComparison.Ordinal) ? 4
        : name.Contains("_b96", StringComparison.Ordinal) ? 3
        : name.EndsWith("64", StringComparison.Ordinal) || name.Contains("64_", StringComparison.Ordinal) ? 2
        : 1;

    static string Swizzle(uint offset) {
        if ((offset & 0x8000) != 0) {
            return $"swizzle(QUAD_PERM,{offset & 3},{(offset >> 2) & 3},{(offset >> 4) & 3},{(offset >> 6) & 3})";
        }
        var and = offset & 0x1f;
        var or = (offset >> 5) & 0x1f;
        var xor = (offset >> 10) & 0x1f;
        if (and == 0x1f && or == 0) return $"swizzle(SWAP,{xor})";
        return $"swizzle(BITMASK_PERM,and:0x{and:x},or:0x{or:x},xor:0x{xor:x})";
    }

    static void DecodeDs(Instruction inst, ref Reader reader) {
        var w0 = reader.Next();
        var w1 = reader.Next();
        var offset0 = w0 & 0xff;
        var offset1 = (w0 >> 8) & 0xff;
        var gds = (w0 >> 17) & 1;
        var op = (w0 >> 18) & 0xff;
        var addr = w1 & 0xff;
        var data0 = (w1 >> 8) & 0xff;
        var data1 = (w1 >> 16) & 0xff;
        var vdst = (w1 >> 24) & 0xff;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.DS, op, "ds");
        var width = DataWidth(name);
        var pair = name.Contains("read2", StringComparison.Ordinal) ||
                   name.Contains("write2", StringComparison.Ordinal) ||
                   name.Contains("wrxchg2", StringComparison.Ordinal);
        var twoData = pair && !name.Contains("read2", StringComparison.Ordinal) ||
                      name.Contains("cmpst", StringComparison.Ordinal) ||
                      name.Contains("mskor", StringComparison.Ordinal);
        var returns = name.StartsWith("ds_read", StringComparison.Ordinal) ||
                      name.Contains("_rtn", StringComparison.Ordinal) ||
                      name is "ds_swizzle_b32" or "ds_append" or "ds_consume" or "ds_ordered_count";
        var hasAddr = name is not ("ds_append" or "ds_consume" or "ds_nop") &&
                      !name.StartsWith("ds_gws_", StringComparison.Ordinal);
        var hasData = !name.StartsWith("ds_read", StringComparison.Ordinal) &&
                      name is not ("ds_swizzle_b32" or "ds_append" or "ds_consume" or "ds_nop" or
                                   "ds_ordered_count" or "ds_gws_sema_v" or "ds_gws_sema_p");

        if (returns) {
            var returned = name.Contains("cmpst", StringComparison.Ordinal) ||
                           name.Contains("mskor", StringComparison.Ordinal) ? width
                           : pair ? width * 2 : width;
            inst.Operands.Add(Operands.Vgpr(vdst, returned));
        }
        if (hasAddr) inst.Operands.Add(Operands.Vgpr(addr));
        if (hasData) inst.Operands.Add(Operands.Vgpr(data0, width));
        if (hasData && twoData) inst.Operands.Add(Operands.Vgpr(data1, width));

        var offset = offset1 << 8 | offset0;
        if (name == "ds_swizzle_b32") {
            inst.Modifiers.Add($"offset:{Swizzle(offset)}");
        } else if (pair) {
            if (offset0 != 0) inst.Modifiers.Add($"offset0:{offset0}");
            if (offset1 != 0) inst.Modifiers.Add($"offset1:{offset1}");
        } else if (offset != 0) {
            inst.Modifiers.Add($"offset:{offset}");
        }
        if (gds != 0) inst.Modifiers.Add("gds");
    }

    // Vector memory

    static int BufferDataWidth(string name) {
        if (name.Contains("cmpswap", StringComparison.Ordinal)) {
            return name.EndsWith("_x2", StringComparison.Ordinal) ? 4 : 2;
        }
        if (name.EndsWith("_x2", StringComparison.Ordinal) || name.EndsWith("dwordx2", StringComparison.Ordinal) ||
            name.EndsWith("format_xy", StringComparison.Ordinal)) return 2;
        if (name.EndsWith("dwordx3", StringComparison.Ordinal) || name.EndsWith("format_xyz", StringComparison.Ordinal)) return 3;
        if (name.EndsWith("dwordx4", StringComparison.Ordinal) || name.EndsWith("format_xyzw", StringComparison.Ordinal)) return 4;
        return 1;
    }

    static void BufferOperands(Instruction inst, string name, uint w0, uint w1, bool offen, bool idxen,
                               bool addr64, bool tfe) {
        var vaddr = w1 & 0xff;
        var vdata = (w1 >> 8) & 0xff;
        var srsrc = ((w1 >> 16) & 0x1f) * 4;
        var soffset = (w1 >> 24) & 0xff;
        var load = name.Contains("load", StringComparison.Ordinal);
        var width = BufferDataWidth(name) + (tfe && load ? 1 : 0);
        inst.Operands.Add(Operands.Vgpr(vdata, width));
        inst.Operands.Add(offen || idxen || addr64
            ? Operands.Vgpr(vaddr, (offen && idxen) || addr64 ? 2 : 1) : "off");
        inst.Operands.Add(Operands.Sgpr(srsrc, 4));
        inst.Operands.Add(Operands.Scalar(soffset));
        _ = w0;
    }

    static void BufferModifiers(Instruction inst, uint offset, bool offen, bool idxen, bool addr64,
                                bool glc, bool slc, bool lds, bool tfe) {
        if (offen) inst.Modifiers.Add("offen");
        if (idxen) inst.Modifiers.Add("idxen");
        if (addr64) inst.Modifiers.Add("addr64");
        if (offset != 0) inst.Modifiers.Add($"offset:{offset}");
        if (glc) inst.Modifiers.Add("glc");
        if (slc) inst.Modifiers.Add("slc");
        if (lds) inst.Modifiers.Add("lds");
        if (tfe) inst.Modifiers.Add("tfe");
    }

    static void DecodeMubuf(Instruction inst, ref Reader reader) {
        var w0 = reader.Next();
        var w1 = reader.Next();
        var offset = w0 & 0xfff;
        var offen = ((w0 >> 12) & 1) != 0;
        var idxen = ((w0 >> 13) & 1) != 0;
        var glc = ((w0 >> 14) & 1) != 0;
        var addr64 = ((w0 >> 15) & 1) != 0;
        var lds = ((w0 >> 16) & 1) != 0;
        var op = (w0 >> 18) & 0x7f;
        var slc = ((w1 >> 22) & 1) != 0;
        var tfe = ((w1 >> 23) & 1) != 0;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.MUBUF, op, "mubuf");
        if (name.StartsWith("buffer_wbinvl1", StringComparison.Ordinal)) {
            return;
        }
        BufferOperands(inst, name, w0, w1, offen, idxen, addr64, tfe);
        BufferModifiers(inst, offset, offen, idxen, addr64, glc, slc, lds, tfe);
    }

    static readonly string[] DataFormats = [
        "INVALID", "8", "16", "8_8", "32", "16_16", "10_11_11", "11_11_10", "10_10_10_2",
        "2_10_10_10", "8_8_8_8", "32_32", "16_16_16_16", "32_32_32", "32_32_32_32", "RESERVED_15",
    ];
    static readonly string[] NumFormats = [
        "UNORM", "SNORM", "USCALED", "SSCALED", "UINT", "SINT", "SNORM_OGL", "FLOAT",
    ];

    static void DecodeMtbuf(Instruction inst, ref Reader reader) {
        var w0 = reader.Next();
        var w1 = reader.Next();
        var offset = w0 & 0xfff;
        var offen = ((w0 >> 12) & 1) != 0;
        var idxen = ((w0 >> 13) & 1) != 0;
        var glc = ((w0 >> 14) & 1) != 0;
        var addr64 = ((w0 >> 15) & 1) != 0;
        var op = (w0 >> 16) & 0x7;
        var dfmt = (w0 >> 19) & 0xf;
        var nfmt = (w0 >> 23) & 0x7;
        var slc = ((w1 >> 22) & 1) != 0;
        var tfe = ((w1 >> 23) & 1) != 0;
        inst.Mnemonic = Name(inst, OpcodeTables.MTBUF, op, "mtbuf");
        BufferOperands(inst, inst.Mnemonic, w0, w1, offen, idxen, addr64, tfe);
        inst.Modifiers.Add($"format:[BUF_DATA_FORMAT_{DataFormats[dfmt]},BUF_NUM_FORMAT_{NumFormats[nfmt]}]");
        BufferModifiers(inst, offset, offen, idxen, addr64, glc, slc, false, tfe);
    }

    static void DecodeMimg(Instruction inst, ref Reader reader) {
        var w0 = reader.Next();
        var w1 = reader.Next();
        var dmask = (w0 >> 8) & 0xf;
        var unorm = (w0 >> 12) & 1;
        var glc = (w0 >> 13) & 1;
        var da = (w0 >> 14) & 1;
        var r128 = (w0 >> 15) & 1;
        var tfe = (w0 >> 16) & 1;
        var lwe = (w0 >> 17) & 1;
        var op = (w0 >> 18) & 0x7f;
        var slc = (w0 >> 25) & 1;
        var vaddr = w1 & 0xff;
        var vdata = (w1 >> 8) & 0xff;
        var srsrc = ((w1 >> 16) & 0x1f) * 4;
        var ssamp = ((w1 >> 21) & 0x1f) * 4;
        var name = inst.Mnemonic = Name(inst, OpcodeTables.MIMG, op, "mimg");
        var width = name.StartsWith("image_gather4", StringComparison.Ordinal) ? 4
                  : name.Contains("cmpswap", StringComparison.Ordinal) ? 2
                  : Math.Max(1, BitOperations.PopCount(dmask));
        width += (int)tfe;
        inst.Operands.Add(Operands.Vgpr(vdata, width));
        // The number of address VGPRs follows from the image dimensions, not from the encoding.
        inst.Operands.Add(Operands.Vgpr(vaddr));
        inst.Operands.Add(Operands.Sgpr(srsrc, r128 != 0 ? 4 : 8));
        var samples = name.Contains("sample", StringComparison.Ordinal) ||
                      name.Contains("gather4", StringComparison.Ordinal) ||
                      name.Contains("get_lod", StringComparison.Ordinal);
        if (samples) inst.Operands.Add(Operands.Sgpr(ssamp, 4));
        inst.Modifiers.Add($"dmask:0x{dmask:x}");
        if (unorm != 0) inst.Modifiers.Add("unorm");
        if (glc != 0) inst.Modifiers.Add("glc");
        if (slc != 0) inst.Modifiers.Add("slc");
        if (r128 != 0) inst.Modifiers.Add("r128");
        if (tfe != 0) inst.Modifiers.Add("tfe");
        if (lwe != 0) inst.Modifiers.Add("lwe");
        if (da != 0) inst.Modifiers.Add("da");
    }

    static string ExportTarget(uint target) => target switch {
        <= 7 => $"mrt{target}",
        8 => "mrtz",
        9 => "null",
        >= 12 and <= 15 => $"pos{target - 12}",
        >= 32 and <= 63 => $"param{target - 32}",
        _ => $"invalid_target_{target}",
    };

    static void DecodeExp(Instruction inst, ref Reader reader) {
        var w0 = reader.Next();
        var w1 = reader.Next();
        var en = w0 & 0xf;
        var target = (w0 >> 4) & 0x3f;
        var compr = (w0 >> 10) & 1;
        var done = (w0 >> 11) & 1;
        var vm = (w0 >> 12) & 1;
        // LLVM syntax: the target is separated from the sources by a space, not a comma.
        inst.Mnemonic = $"{Name(inst, OpcodeTables.EXP, 0, "exp")} {ExportTarget(target)}";
        for (var i = 0; i < 4; ++i) {
            // Compressed exports carry two packed halves per VGPR in vsrc0/vsrc1; the enable
            // bits still name four components (LLVM prints the register once per component).
            var slot = compr != 0 ? i / 2 : i;
            var vsrc = (w1 >> (8 * slot)) & 0xff;
            inst.Operands.Add(((en >> i) & 1) != 0 ? Operands.Vgpr(vsrc) : "off");
        }
        if (compr != 0) inst.Modifiers.Add("compr");
        if (done != 0) inst.Modifiers.Add("done");
        if (vm != 0) inst.Modifiers.Add("vm");
    }
}
