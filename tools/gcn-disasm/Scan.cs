namespace GcnDisasm;

/// Decodes a directory of programs and reports what the tables do not cover.
internal static class Scan {
    public static int Run(string directory, string pattern) {
        var files = Directory.GetFiles(directory, pattern, SearchOption.AllDirectories);
        var unknown = new Dictionary<string, int>();
        var instructions = 0;
        var incomplete = new List<string>();
        foreach (var file in files) {
            var program = Listing.Decode(Listing.ReadWords(file), all: false);
            instructions += program.Count;
            foreach (var inst in program.Where(i => i.Unknown)) {
                var key = inst.Mnemonic == ".word" ? $".word ({inst.Encoding})" : inst.Mnemonic;
                unknown[key] = unknown.GetValueOrDefault(key) + 1;
            }
            if (!program.Any(i => i.EndsProgram)) {
                incomplete.Add(Path.GetFileName(file));
            }
        }
        Console.WriteLine($"{files.Length} programs, {instructions} instructions");
        Console.WriteLine($"programs without s_endpgm: {incomplete.Count}");
        foreach (var name in incomplete.Take(20)) {
            Console.WriteLine($"  {name}");
        }
        Console.WriteLine($"unknown or illegal instructions: {unknown.Values.Sum()}");
        foreach (var (name, count) in unknown.OrderByDescending(p => p.Value)) {
            Console.WriteLine($"  {count,8}  {name}");
        }
        return unknown.Count == 0 && incomplete.Count == 0 ? 0 : 1;
    }
}
