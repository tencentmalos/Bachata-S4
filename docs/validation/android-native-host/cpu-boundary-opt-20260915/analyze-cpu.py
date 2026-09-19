import sys,json,collections
from pathlib import Path
root=Path(__file__).resolve().parent
sys.path.insert(0,'/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865/simpleperf')
from simpleperf_report_lib import GetReportLib
lib=GetReportLib(str(root/'perf.data'));lib.ShowIpForUnknownSymbol();samples=[]
while True:
 s=lib.GetNextSample()
 if s is None:break
 leaf=lib.GetSymbolOfCurrentSample();chain=lib.GetCallChainOfCurrentSample()
 symbols=[leaf]+[chain.entries[i].symbol for i in range(chain.nr)]
 frames=[dict(symbol=f.symbol_name,dso=f.dso_name) for f in symbols]
 samples.append(dict(tid=s.tid,name=s.thread_comm,time=s.time,period=s.period,frames=frames))
lib.Close()
(root/'samples.json').write_text(json.dumps(samples))
def summarize(data):
 n=len(data);leaves=collections.Counter(s['frames'][0]['symbol'] for s in data)
 vm=sum(any('GuestAddressSpace::' in f['symbol'] for f in s['frames']) for s in data)
 leaf=sum('GuestAddressSpace::' in s['frames'][0]['symbol'] for s in data)
 checks=sum(any(k in s['frames'][0]['symbol'] for k in ['ValidateRangeLocked','FindContainingMappingLocked']) for s in data)
 threads=collections.Counter(s['name'] for s in data)
 return dict(samples=n,period_s=sum(s['period'] for s in data)/1e9,vm_stack_samples=vm,vm_stack_pct=100*vm/n,vm_leaf_samples=leaf,vm_leaf_pct=100*leaf/n,lookup_leaf_samples=checks,lookup_leaf_pct=100*checks/n,top_leaves=leaves.most_common(25),threads=threads)
old=json.loads((root.parent/'gameplay-profile-20260915/samples.json').read_text())
summary=dict(before=summarize(old),after=summarize(samples))
(root/'cpu-comparison.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
