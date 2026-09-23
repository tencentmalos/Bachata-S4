"""Children and leaves under chosen functions for one thread of a simpleperf recording.
usage: perf_subtree.py PERF_DATA BINARY_CACHE THREAD_REGEX FUNC_REGEX [FUNC_REGEX...]"""
import re, sys, collections
sys.path.insert(0, r"C:/Users/Admin/AppData/Local/Android/Sdk/ndk/29.0.14206865/simpleperf")
from simpleperf_report_lib import ReportLib
perf, cache, treg = sys.argv[1], sys.argv[2], re.compile(sys.argv[3])
funcs = [(f, re.compile(f)) for f in sys.argv[4:]]
lib = ReportLib(); lib.SetRecordFile(perf); lib.SetSymfs(cache); lib.ShowIpForUnknownSymbol()
tot = 0
under = collections.Counter(); child = collections.defaultdict(collections.Counter); leaf = collections.defaultdict(collections.Counter)
short = lambda s: re.sub(r"\(.*", "", s)[:120]
while True:
    s = lib.GetNextSample()
    if s is None: break
    if not treg.search(s.thread_comm): continue
    tot += 1
    sym = lib.GetSymbolOfCurrentSample(); cc = lib.GetCallChainOfCurrentSample()
    frames = [sym.symbol_name] + [cc.entries[i].symbol.symbol_name for i in range(cc.nr)]
    for fname, fr in funcs:
        for i, name in enumerate(frames):
            if fr.search(name):
                under[fname] += 1
                child[fname][short(frames[i - 1]) if i > 0 else "<self>"] += 1
                leaf[fname][short(frames[0])] += 1
                break
for fname, _ in funcs:
    n = under[fname]
    print(f"\n### {fname}: {n} samples = {100*n/max(tot,1):.1f}% of thread")
    print("-- direct children")
    for k, c in child[fname].most_common(14): print(f"  {100*c/max(n,1):5.1f}%  {k}")
    print("-- leaves")
    for k, c in leaf[fname].most_common(14): print(f"  {100*c/max(n,1):5.1f}%  {k}")
