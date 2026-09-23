import sys, collections
sys.path.insert(0, r"C:/Users/Admin/AppData/Local/Android/Sdk/ndk/29.0.14206865/simpleperf")
from simpleperf_report_lib import ReportLib
lib = ReportLib(); lib.SetRecordFile(sys.argv[1])
want = sys.argv[2]
ips = collections.Counter(); maps = {}; n = 0
while True:
    s = lib.GetNextSample()
    if s is None: break
    if s.thread_comm != want: continue
    n += 1
    sym = lib.GetSymbolOfCurrentSample()
    if sym.dso_name in ("unknown", "[unknown]") or "anon" in sym.dso_name or "jit" in sym.dso_name.lower():
        ips[s.ip] += 1; maps[s.ip] = sym.dso_name
print("samples", n)
pages = collections.Counter()
for ip, c in ips.items(): pages[ip & ~0xff] += c
for pg, c in pages.most_common(12): print(f"{pg:#x} {c} {100*c/n:.1f}%")
