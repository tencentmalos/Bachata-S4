namespace GcnDisasm;

public enum Encoding {
    SOP2, SOPK, SOP1, SOPC, SOPP, SMRD, VOP2, VOP1, VOPC, VOP3, VOP3P, VINTRP, DS, MUBUF, MTBUF,
    MIMG, EXP, Illegal,
}

/// One decoded instruction. Offsets are in bytes from the start of the program.
public sealed class Instruction {
    public required int Offset { get; init; }
    public required Encoding Encoding { get; init; }
    public uint Op { get; set; }
    public string Mnemonic { get; set; } = "";
    public List<string> Operands { get; } = [];
    public List<string> Modifiers { get; } = [];
    public List<string> Comments { get; } = [];
    public uint[] Words { get; set; } = [];
    public int Dwords => Words.Length;
    public int? BranchTarget { get; set; }
    public bool IsConditionalBranch { get; set; }
    public bool EndsProgram { get; set; }
    /// True when the mnemonic is not in the emulator's tables.
    public bool Unknown { get; set; }

    public int Size => Dwords * 4;
    public int NextOffset => Offset + Size;
    /// Control never falls through to the next instruction.
    public bool IsTerminator => EndsProgram || (BranchTarget is not null && !IsConditionalBranch);

    public string Text {
        get {
            var text = Mnemonic;
            if (Operands.Count > 0) {
                text += " " + string.Join(", ", Operands);
            }
            if (Modifiers.Count > 0) {
                text += " " + string.Join(" ", Modifiers);
            }
            return text;
        }
    }
}
