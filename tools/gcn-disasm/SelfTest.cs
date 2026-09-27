namespace GcnDisasm;

/// Encodings from real PS4 shaders (Monster Hunter: World, compute and pixel shaders) and
/// hand-assembled ones, with the text the disassembler must produce.
internal static class SelfTest {
    static readonly (uint[] Words, string Text)[] Cases = [
        ([0xbeea287e], "s_orn2_saveexec_b64 vcc, exec"),
        ([0xbefe046a], "s_mov_b64 exec, vcc"),
        ([0xbefe0880], "s_not_b64 exec, 0"),
        ([0xbf8c007f], "s_waitcnt lgkmcnt(0)"),
        ([0xbf8c0f70], "s_waitcnt vmcnt(0)"),
        ([0xb902f801], "s_getreg_b32 s2, hwreg(HW_REG_MODE)"),
        ([0xc0820d30], "s_load_dwordx4 s[4:7], s[12:13], 0x30"),
        ([0xc21504ff, 0x00000192], "s_buffer_load_dword s42, s[4:7], 0x192"),
        ([0x000c08c1], "v_cndmask_b32 v6, -1, v4, vcc"),
        ([0x02093f06], "v_readlane_b32 s4, v6, 31"),
        ([0x7e0602c1], "v_mov_b32 v3, -1"),
        ([0x360404ff, 0x0003ffff], "v_and_b32 v2, 0x3ffff, v2"),
        ([0x7da8000f], "v_cmpx_gt_u32 vcc, s15, v0"),
        ([0xd1840012, 0x00022c80], "v_cmp_eq_u32 s[18:19], 0, v22"),
        ([0xd2ba0001, 0x0401000e], "v_sad_u32 v1, s14, 0, v0"),
        ([0xd2860002, 0x04020290], "v_mad_u32_u24 v2, 16, v1, v0"),
        ([0xd8d4401f, 0x05000006], "ds_swizzle_b32 v5, v6 offset:swizzle(SWAP,16)"),
        ([0xd8dc0201, 0x04000003], "ds_read2_b32 v[4:5], v3 offset0:1 offset1:2"),
        ([0xe0302000, 0x80001800], "buffer_load_dword v24, v0, s[0:3], 0 idxen"),
        ([0xe0701000, 0x80010204], "buffer_store_dword v2, v4, s[4:7], 0 offen"),
        ([0xf0800f00, 0x00020800], "image_sample v[8:11], v0, s[8:15], s[0:3] dmask:0xf"),
        ([0xc8080000], "v_interp_p1_f32 v2, v0, attr0.x"),
        ([0xc8090000], "v_interp_p2_f32 v2, v0, attr0.x"),
        ([0xf800180f, 0x03020100], "exp mrt0 v0, v1, v2, v3 done vm"),
    ];

    public static int Run() {
        var failures = 0;
        foreach (var (words, expected) in Cases) {
            var inst = Decoder.Decode(words, 0);
            var ok = inst.Text == expected && inst.Dwords == words.Length && !inst.Unknown;
            if (!ok) {
                failures++;
                Console.WriteLine($"FAIL {string.Join(" ", words.Select(w => w.ToString("x8")))}: " +
                                  $"got \"{inst.Text}\" ({inst.Dwords} dwords), want \"{expected}\"");
            }
        }
        // Branch targets are relative to the next instruction.
        uint[] branch = [0xbf820001, 0xbf800000, 0xbf810000];
        var program = Listing.Decode(branch, all: false);
        if (program[0].Text != "s_branch 0x8" || program[0].BranchTarget != 8 || program.Count != 3) {
            failures++;
            Console.WriteLine($"FAIL branch: got \"{program[0].Text}\", {program.Count} instructions");
        }
        Console.WriteLine($"selftest: {Cases.Length + 1} cases, {failures} failures");
        return failures == 0 ? 0 : 1;
    }
}
