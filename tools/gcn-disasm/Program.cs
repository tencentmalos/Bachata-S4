using System.Globalization;
using GcnDisasm;

const string Usage = """
    gcn-disasm: GCN2 (PS4 / Sea Islands, incl. PS4 Pro extensions) shader disassembler.

    usage:
      gcn-disasm <code.bin> [--all] [--no-raw] [--no-labels] [--base <hex>] [--skip <bytes>]
      gcn-disasm --hex "<dword> <dword> ..." [--base <hex>]
      gcn-disasm --hash <hex> --dumps <dir>        the dump of that shader hash, as named in
                                                   RenderDoc labels (shadps4.draw ps=0x...,
                                                   shadps4.dispatch cs=0x...)
      gcn-disasm --scan <dir> [--pattern <glob>]   decode every file; report unknown or illegal
                                                   instructions and programs without s_endpgm
      gcn-disasm --selftest

    <code.bin> is raw little-endian machine code, e.g. a shader dump (user/shader/dumps/*.bin)
    or `ps4_gpu_trace.py shader ... --out shader.bin`. Decoding stops at s_endpgm unless --all.
    """;

string[] valued = ["--base", "--skip", "--hex", "--scan", "--pattern", "--hash", "--dumps"];
var options = new Dictionary<string, string>();
var flags = new HashSet<string>();
var positional = new List<string>();
for (var i = 0; i < args.Length; ++i) {
    if (valued.Contains(args[i]) && i + 1 < args.Length) {
        options[args[i]] = args[++i];
    } else if (args[i].StartsWith("--", StringComparison.Ordinal) || args[i] == "-h") {
        flags.Add(args[i]);
    } else {
        positional.Add(args[i]);
    }
}

if (args.Length == 0 || flags.Contains("-h") || flags.Contains("--help")) {
    Console.WriteLine(Usage);
    return args.Length == 0 ? 2 : 0;
}
if (flags.Contains("--selftest")) {
    return SelfTest.Run();
}
if (options.TryGetValue("--scan", out var directory)) {
    return Scan.Run(directory, options.GetValueOrDefault("--pattern", "*.bin"));
}

static long Number(string? text) =>
    text is null ? 0
    : text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
        ? long.Parse(text[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
        : long.Parse(text, CultureInfo.InvariantCulture);

uint[] code;
if (options.TryGetValue("--hex", out var hex)) {
    code = hex.Split([' ', ',', '\n', '\t'], StringSplitOptions.RemoveEmptyEntries)
              .Select(t => uint.Parse(t.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? t[2..] : t,
                                      NumberStyles.HexNumber, CultureInfo.InvariantCulture))
              .ToArray();
} else if (options.TryGetValue("--hash", out var hashText)) {
    // Dumps are <stage>_<hash as 0x + 16 hex digits>_<permutation>.bin; the guest code is the
    // same for every permutation, so any one of them is the shader.
    var hash = ulong.Parse(hashText.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? hashText[2..] : hashText,
                           NumberStyles.HexNumber, CultureInfo.InvariantCulture);
    var dumps = options.GetValueOrDefault("--dumps", Path.Combine("user", "shader", "dumps"));
    var matches = Directory.Exists(dumps)
        ? Directory.GetFiles(dumps, $"*_0x{hash:x16}*.bin")
                   .Where(f => !f.EndsWith(".fetch.bin", StringComparison.Ordinal) &&
                               !f.EndsWith(".copy.bin", StringComparison.Ordinal))
                   .Order(StringComparer.Ordinal).ToArray()
        : [];
    if (matches.Length == 0) {
        Console.Error.WriteLine($"no dump for 0x{hash:x16} in {dumps} (run with dump_shaders on)");
        return 1;
    }
    Console.WriteLine($"// {Path.GetFileName(matches[0])}" +
                      (matches.Length > 1 ? $" (+{matches.Length - 1} permutation dumps of the same code)" : ""));
    code = Listing.ReadWords(matches[0]);
} else if (positional.Count == 1) {
    code = Listing.ReadWords(positional[0]);
    code = code[(int)(Number(options.GetValueOrDefault("--skip")) / 4)..];
} else {
    Console.Error.WriteLine(Usage);
    return 2;
}

var listing = new ListingOptions {
    All = flags.Contains("--all"),
    RawWords = !flags.Contains("--no-raw"),
    Labels = !flags.Contains("--no-labels"),
    BaseAddress = Number(options.GetValueOrDefault("--base")),
};
Console.Write(Listing.Format(Listing.Decode(code, listing.All), listing));
return 0;
