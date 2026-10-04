"""For one thread of a simpleperf file, group samples whose leaf is in the kernel by the first
user-space frame of the call chain (the syscall or fault site), ms per frame.

usage: kcallers.py <perf.data> <frames> <thread> [leaf-substring]
"""
import collections
import sys

sys.path.insert(0, r'D:\Android\android-sdk\ndk\29.0.14206865\simpleperf')
from simpleperf_report_lib import ReportLib  # noqa: E402

path, frames, thread = sys.argv[1], float(sys.argv[2]), sys.argv[3]
leaf_filter = sys.argv[4] if len(sys.argv) > 4 else None
lib = ReportLib()
lib.SetRecordFile(path)
by_site = collections.Counter()
by_chain = collections.Counter()
total = 0
while True:
    s = lib.GetNextSample()
    if s is None:
        break
    if s.thread_comm != thread:
        continue
    sym = lib.GetSymbolOfCurrentSample()
    if not sym.dso_name.endswith('kallsyms]'):
        continue
    leaf = f'{sym.symbol_name}'
    if leaf_filter and leaf_filter not in leaf:
        continue
    total += 1
    chain = lib.GetCallChainOfCurrentSample()
    user = []
    for i in range(chain.nr):
        e = chain.entries[i]
        if e.symbol.dso_name.endswith('kallsyms]'):
            continue
        user.append(f'{e.symbol.symbol_name.split("(")[0][:70]} [{e.symbol.dso_name.rsplit("/", 1)[-1]}]')
        if len(user) == 3:
            break
    by_site[user[0] if user else '(no user frame)'] += 1
    by_chain[' <- '.join(user) if user else '(no user frame)'] += 1
print(f'{thread} kernel-leaf samples {total} = {total * 0.5 / frames:.2f} ms/frame')
for site, n in by_site.most_common(15):
    print(f'  {n * 0.5 / frames:6.2f}  {site}')
print('-- chains')
for c, n in by_chain.most_common(12):
    print(f'  {n * 0.5 / frames:6.2f}  {c}')
