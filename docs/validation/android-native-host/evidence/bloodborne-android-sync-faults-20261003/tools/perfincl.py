"""Inclusive and leaf ms/frame for symbols matching regexes, in one thread of a simpleperf file.

usage: perfincl.py <perf.data> <frames> <thread-regex> <symbol-regex>...
"""
import collections
import re
import sys

sys.path.insert(0, r'D:\Android\android-sdk\ndk\29.0.14206865\simpleperf')
from simpleperf_report_lib import ReportLib  # noqa: E402

path, frames, thread = sys.argv[1], float(sys.argv[2]), re.compile(sys.argv[3])
pats = [re.compile(p) for p in sys.argv[4:]]
MS = 0.5
lib = ReportLib()
lib.ShowIpForUnknownSymbol()
lib.SetRecordFile(path)
total = 0
incl = collections.Counter()
leaf = collections.Counter()
leafs = collections.Counter()
while True:
    s = lib.GetNextSample()
    if s is None:
        break
    if not thread.search(s.thread_comm):
        continue
    total += 1
    sym = lib.GetSymbolOfCurrentSample()
    chain = lib.GetCallChainOfCurrentSample()
    names = [sym.symbol_name] + [chain.entries[i].symbol.symbol_name for i in range(chain.nr)]
    leafs[re.sub(r'\(.*', '', sym.symbol_name)[:90]] += 1
    for p in pats:
        if p.search(names[0]):
            leaf[p.pattern] += 1
        if any(p.search(n) for n in names):
            incl[p.pattern] += 1
print(f'samples {total} = {total * MS / frames:.2f} ms/frame')
for p in pats:
    print(f'  {p.pattern:40s} incl {incl[p.pattern] * MS / frames:6.2f}  leaf {leaf[p.pattern] * MS / frames:6.2f} ms/frame')
print('top leaves:')
for name, n in leafs.most_common(25):
    print(f'  {n * MS / frames:6.2f}  {name}')
