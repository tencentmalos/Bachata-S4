using System.Globalization;
using GcnDisasm;

const string Usage = """
    gcn-disasm: GCN2 (PS4 / Sea Islands, incl. PS4 Pro extensions) shader disassembler.

    usage:
      gcn-disasm <code.bin> [--all] [--no-raw] [--no-labels] [--base <hex>] [--skip <bytes>]
      gcn-disasm --hex "<dword> <dword> ..." [--base <hex>]
      gcn-disasm --scan <dir> [--pattern <glob>]   decode every file; report unknown or illegal
                                                   instructions and programs without s_endpgm
      gcn-disasm --selftest

    <code.bin> is raw little-endian machine code, e.g. a shader dump (user/shader/dumps/*.bin)
    or `ps4_gpu_trace.py shader ... --out shader.bin`. Decoding stops at s_endpgm unless --all.
    """;

string[] valued = ["--base", "--skip", "--hex", "--scan", "--pattern"];
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
