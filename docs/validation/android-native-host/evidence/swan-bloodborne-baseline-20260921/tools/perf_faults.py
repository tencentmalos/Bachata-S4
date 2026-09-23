"""On-CPU cost of GPU write-watch faults per thread: samples whose stack is in the fault path.
usage: perf_faults.py PERF_DATA BINARY_CACHE [FREQ=2000]"""
import re, sys, collections
sys.path.insert(0, r"C:/Users/Admin/AppData/Local/Android/Sdk/ndk/29.0.14206865/simpleperf")
from simpleperf_report_lib import ReportLib
perf, cache = sys.argv[1], sys.argv[2]; freq = float(sys.argv[3]) if len(sys.argv) > 3 else 2000.0
lib = ReportLib(); lib.SetRecordFile(perf); lib.SetSymfs(cache); lib.ShowIpForUnknownSymbol()
# kernel entry of a user data abort, signal delivery/return, and the user-space handler chain
PAT = {
    "user_handler": re.compile(r"art::SignalChain::Handler|DispatchAccessViolation|GuestFaultSignalHandler"),
    "abort_entry": re.compile(r"^el0_da$|do_mem_abort|do_page_fault"),
    "sigreturn": re.compile(r"sys_rt_sigreturn"),
    "signal_setup": re.compile(r"do_notify_resume|do_signal|setup_rt_frame|arm64_notify_die|force_sig_fault"),
}
tot = collections.Counter(); hit = collections.defaultdict(collections.Counter); anyhit = collections.Counter()
while True:
    s = lib.GetNextSample()
    if s is None: break
    th = re.sub(r"\d+$", "N", s.thread_comm) if s.thread_comm.startswith("Guest-") and s.thread_comm != "Guest-1" else s.thread_comm
    tot[th] += 1
    sym = lib.GetSymbolOfCurrentSample(); cc = lib.GetCallChainOfCurrentSample()
    frames = [sym.symbol_name] + [cc.entries[i].symbol.symbol_name for i in range(cc.nr)]
    matched = False
    for k, p in PAT.items():
        if any(p.search(f) for f in frames):
            hit[th][k] += 1; matched = True
    if matched: anyhit[th] += 1
for th, n in sorted(tot.items(), key=lambda x: -anyhit[x[0]])[:8]:
    print(f"{th:28s} on-cpu {n/freq*1000:8.1f} ms  fault-path {anyhit[th]/freq*1000:7.1f} ms ({100*anyhit[th]/max(n,1):4.1f}%)  " +
          " ".join(f"{k}={v/freq*1000:.1f}" for k, v in hit[th].items()))
print(f"ALL fault-path {sum(anyhit.values())/freq*1000:.1f} ms of {sum(tot.values())/freq*1000:.1f} ms")
