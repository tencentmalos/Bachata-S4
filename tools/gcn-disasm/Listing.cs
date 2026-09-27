using System.Text;

namespace GcnDisasm;

public sealed class ListingOptions {
    /// Keep decoding after s_endpgm (shader dumps may carry metadata after the program).
    public bool All { get; init; }
    public bool RawWords { get; init; } = true;
    public bool Labels { get; init; } = true;
    public long BaseAddress { get; init; }
}

/// Decodes a whole program and prints it with branch labels, basic block breaks and loop marks.
public static class Listing {
    public static List<Instruction> Decode(ReadOnlySpan<uint> code, bool all) {
        var list = new List<Instruction>();
        var index = 0;
        while (index < code.Length) {
            var inst = Decoder.Decode(code, index);
            list.Add(inst);
            index += inst.Dwords;
            // Stop at s_endpgm unless a later branch still targets code after it.
            if (inst.EndsProgram && !all && !list.Any(i => i.BranchTarget > inst.Offset)) {
                break;
            }
        }
        return list;
    }

    public static string Format(IReadOnlyList<Instruction> program, ListingOptions options) {
        var targets = program.Where(i => i.BranchTarget is not null)
                             .Select(i => i.BranchTarget!.Value).ToHashSet();
        var sb = new StringBuilder();
        foreach (var inst in program) {
            if (options.Labels && targets.Contains(inst.Offset)) {
                sb.Append('\n').Append(Label(inst.Offset + options.BaseAddress)).Append(":\n");
            }
            var address = inst.Offset + options.BaseAddress;
            sb.Append($"  {address:x5}: ");
            if (options.RawWords) {
                sb.Append(string.Join(" ", inst.Words.Select(w => w.ToString("x8"))).PadRight(27));
            }
            var text = inst.Text;
            if (options.Labels && inst.BranchTarget is { } target) {
                text = text.Replace($"0x{target:x}", Label(target + options.BaseAddress),
                                    StringComparison.Ordinal);
            }
            sb.Append(text);
            var comments = new List<string>(inst.Comments);
            if (inst.BranchTarget is { } t && t <= inst.Offset) {
                comments.Add("loop back-edge");
            }
            if (inst.Unknown) {
                comments.Add("not in the emulator's opcode tables");
            }
            if (comments.Count > 0) {
                sb.Append("  // ").Append(string.Join("; ", comments));
            }
            sb.Append('\n');
            if (inst.IsTerminator) {
                sb.Append('\n');
            }
        }
        return sb.ToString();
    }

    static string Label(long offset) => $"label_{offset:x4}";

    /// Reads a code file: raw little-endian dwords (shader dumps, PM4 trace extracts).
    public static uint[] ReadWords(string path) {
        var bytes = File.ReadAllBytes(path);
        var words = new uint[bytes.Length / 4];
        Buffer.BlockCopy(bytes, 0, words, 0, words.Length * 4);
        return words;
    }
}
