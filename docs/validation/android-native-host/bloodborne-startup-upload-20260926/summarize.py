from pathlib import Path
import json,collections,statistics,math,re
p=Path(__file__).resolve().parent;d=json.loads((p/'full-analysis.json').read_text());t=d['start_ns'];last=d['end_ns'];names=d['threads']
phases=[('Runtime to first present',0,1.849299),('Early screens / manual progression',1.849299,25),('Loading observation',25,62),('Hunters Dream / manual movement',62,(last-t)/1e9)]
frames=sorted(d['frames'],key=lambda x:x[1]);fr=[]
for a,b in zip(frames,frames[1:]):
 if a[0]==b[0] and b[1]>a[1]:fr.append({'sdk_frame':a[2],'tid':a[0],'start_ns':a[1],'end_ns':b[1],'ms':(b[1]-a[1])/1e6})
def quant(v,p):
 v=sorted(v);return v[min(len(v)-1,int(len(v)*p))] if v else None
def category(n):
 if n.startswith('Texture.'):return 'Texture refresh'
 if n=='GPU.CompileGuestShader':return 'GCN translation'
 if n=='Rasterizer.Draw':return 'Draw self'
 if n=='Rasterizer.BindResources':return 'Resource binding self'
 if n.startswith('HLE.CopyShader') or n.startswith('HLE.Guest'):return 'HLE copy/mirror'
 if n.startswith('Buffer.'):return 'Buffer work'
 if n=='PM4.Resume':return 'PM4 self'
 return 'Other instrumented'
results=[]
for label,a,b in phases:
 a,b=int(t+a*1e9),int(t+b*1e9)
 sums=collections.defaultdict(float)
 for tid,sec,n,ns in d['cpu_self_bins']:
  if tid!=10668:continue
  begin,end=sec*10**9,(sec+1)*10**9
  # Boundary seconds are prorated for a coarse phase overview, not precise scopes.
  w=max(0,min(b,end)-max(a,begin))/1e9
  sums[category(n)]+=ns/1e6*w
 durations=[f['ms'] for f in fr if a<=f['start_ns'] and f['end_ns']<=b]
 exact={}
 for n in ['Texture.Refresh','Texture.Stage','Texture.Detile','Texture.Upload','GPU.CompileGuestShader']:
  xs=[x for x in d['long_scopes'] if x[0]==10668 and x[1]==n and a<=x[2] and x[3]<=b]
  exact[n]={'count':len(xs),'total_ms':sum((x[3]-x[2])/1e6 for x in xs),'max_ms':max([(x[3]-x[2])/1e6 for x in xs],default=0),'p50_ms':quant([(x[3]-x[2])/1e6 for x in xs],.5),'p95_ms':quant([(x[3]-x[2])/1e6 for x in xs],.95)}
 presents=[x for x in d['counter_bins'] if x[2]=='VideoOut.PresentedGuestFrames' and a<=x[3] and x[4]<=b]
 present_count=sum(x[-1] for x in presents)
 results.append({'label':label,'start_s':(a-t)/1e9,'end_s':(b-t)/1e9,'gpucomm_cpu_categories_ms_approx':dict(sums),'texture_compile_exact_fully_contained':exact,'sdk_frames':len(durations),'sdk_frame_p50_ms':quant(durations,.5),'sdk_frame_p95_ms':quant(durations,.95),'sdk_frame_max_ms':max(durations,default=0),'presented_samples_full_seconds_only':present_count})
long=[]
for f in sorted(fr,key=lambda f:f['ms'],reverse=True)[:12]:
 v={**f,'start_s':(f['start_ns']-t)/1e9}
 v['gpucomm_long_scopes']=[{'name':x[1],'ms':(min(x[3],f['end_ns'])-max(x[2],f['start_ns']))/1e6,'self_total_ms':x[4]/1e6} for x in d['long_scopes'] if x[0]==10668 and x[2]<f['end_ns'] and x[3]>f['start_ns'] and x[1] in ['Texture.Refresh','GPU.CompileGuestShader','HLE.CopyShader','Rasterizer.Dispatch','Rasterizer.Draw'] and min(x[3],f['end_ns'])-max(x[2],f['start_ns'])>10_000_000]
 long.append(v)
check=collections.defaultdict(int)
for tid,sec,n,ns in d['cpu_self_bins']:check[tid,sec]+=ns
assert max(check.values())<=1_000_000_000, max(check.values())
out={'phases':results,'long_frames':long,'whole_sdk_frames':{'count':len(fr),'p50_ms':quant([f['ms'] for f in fr],.5),'p95_ms':quant([f['ms'] for f in fr],.95),'max_ms':max(f['ms'] for f in fr)},'gpucomm_cpu_self_ms':sum(x['self_ms'] for x in d['scope_stats'] if x['tid']==10668),'verification':{'max_per_thread_second_self_ns':max(check.values()),'per_thread_second_no_double_count':True},'phase_boundaries':'0 and first present exact runtime markers; 25 and 62 seconds approximate reviewed observation cuts, not exact game loading begin/end'}
(p/'summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
print('IO scopes',sorted([x for x in d['scope_stats'] if re.search('(?i)(Pread|Fread|Aio|ReadFile|KernelRead$)',x['name'])],key=lambda x:x['self_ms'],reverse=True)[:12])
