from pathlib import Path
import json,collections
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
p=Path(__file__).resolve().parent;d=json.loads((p/'full-analysis.json').read_text());t=d['start_ns'];end=(d['end_ns']-t)/1e9
categories=['Draw self','Resource binding','HLE copy','PM4 / other','Texture refresh','GCN translation']
colors=['#4e79a7','#76b7b2','#b07aa1','#a5a9b1','#f28e2b','#e15759']
def cat(n):
 if n=='Rasterizer.Draw':return 0
 if n=='Rasterizer.BindResources':return 1
 if n.startswith(('HLE.CopyShader','HLE.Guest')):return 2
 if n.startswith('Texture.'):return 4
 if n=='GPU.CompileGuestShader':return 5
 return 3
secs=sorted(set(x[1] for x in d['cpu_self_bins'] if x[0]==10668));pos={s:i for i,s in enumerate(secs)};v=np.zeros((6,len(secs)))
for tid,s,n,ns in d['cpu_self_bins']:
 if tid==10668:v[cat(n),pos[s]]+=ns/1e6
x=np.array([(s+.5)-t/1e9 for s in secs])
fig,(ax,bx,cx)=plt.subplots(3,1,figsize=(15,13),gridspec_kw={'height_ratios':[2,1.6,1.4]})
fig.subplots_adjust(left=.12,right=.98,bottom=.105,top=.875,hspace=.24)
fig.suptitle('Bloodborne on AYN Thor | 105.61 s observed run',fontsize=20,weight='bold',y=.975)
base=np.zeros(len(secs))
for a,n,c in zip(v,categories,colors):ax.bar(x,a,bottom=base,width=.96,label=n,color=c);base+=a
ax.set(ylabel='GpuComm elapsed self\n(ms per wall-second)',xlim=(0,end),ylim=(0,1100));ax.legend(ncols=3,loc='lower left',bbox_to_anchor=(0,1.035),fontsize=10);ax.grid(axis='y',alpha=.15)
frames=sorted(d['frames'],key=lambda x:x[1]);fx=[];fy=[]
for a,b in zip(frames,frames[1:]):
 if a[0]==b[0] and b[1]>a[1]:fx.append((a[1]-t)/1e9);fy.append((b[1]-a[1])/1e6)
bx.plot(fx,fy,color='#365b78',linewidth=.7);bx.axhline(33.33,color='#777',ls='--',lw=.8);bx.set(yscale='log',ylim=(5,1300),xlim=(0,end),ylabel='GNM marker interval (ms)\nNot display frame time',xlabel='Seconds since Session.Generation.1')
for at,txt in [(34.873,'929 ms'),(48.113,'588 ms'),(83.39,'489 ms')]:
 idx=min(range(len(fx)),key=lambda i:abs(fx[i]-at));bx.annotate(txt,(fx[idx],fy[idx]),xytext=(8,10),textcoords='offset points',fontsize=9)
for axis in [ax,bx]:
 for a,b,c in [(0,25,'#dbe9f5'),(25,62,'#faecd8'),(62,end,'#e2eee4')]:axis.axvspan(a,b,color=c,alpha=.45,zorder=-1)
 for at in [25,62]:axis.axvline(at,color='#8b8b8b',ls=':',lw=1)
ax.text(12.5,1045,'Early screens / manual input',ha='center',fontsize=9)
ax.text(43.5,1045,'Loading observation*',ha='center',fontsize=9)
ax.text(83.5,1045,'Hunters Dream / movement*',ha='center',fontsize=9)
chosen=['GPU.GuestRenderPass','GPU.GuestDispatch','GPU.HostReadback','GPU.HostTransfer','GPU.BufferUpload','GPU.PostProcess','GPU.Present','GPU.OverlayRedraw']
g={x['name']:x['total_ms']/1000 for x in d['gpu']};labels=[n.removeprefix('GPU.') for n in chosen];totals=[g[n] for n in chosen]
cx.barh(labels[::-1],totals[::-1],color=['#4e79a7' if n=='GPU.GuestRenderPass' else '#8eb7bc' for n in chosen][::-1]);cx.set(xlabel='GPU recorded zone totals (s); CPU/GPU clocks are not calibrated',xlim=(0,37));cx.grid(axis='x',alpha=.15)
for i,vv in enumerate(totals[::-1]):cx.text(vv+.2,i,f'{vv:.2f}',va='center',fontsize=9)
fig.text(.12,.03,'* 25 s and 62 s are reviewed observation cuts, not exact loading boundaries. CPU spans include waits / preemption.\n62,059,047 events; no dropped chunks / truncation; 131 boundary-open CPU spans.\nGPU zone totals can overlap; missing queries are reported separately.',fontsize=9,color='#555')
fig.savefig(p/'overview.png',dpi=140);fig.savefig(p/'overview.svg')
svg=p/'overview.svg';svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines())+'\n')
print(p/'overview.png')
