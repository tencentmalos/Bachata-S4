"""Bounded streaming aggregate using the installed, exact Foundation SDK reader.
No raw events are edited. CPU self is per physical lane, elapsed not on-CPU.
GPU durations are separate: no CPU/GPU alignment is inferred.
"""
from pathlib import Path
from collections import defaultdict,Counter
from dataclasses import asdict
import sys,json,mmap,hashlib,time,array
ROOT=Path(__file__).resolve().parent
SDK=Path('/Users/bytedance/Library/Application Support/Litep/Mcp/versions/litep_mcp-0.3.0-local.20260926.insights2-osx-arm64/bin/litep')
sys.path.insert(0,str(SDK))
from wire import reader,FOUNDATION_REVISION,require_supported_tags
D=reader(FOUNDATION_REVISION)
SOURCE=next((ROOT/'capture').glob('*.prof'))
PAYLOAD=ROOT/'analysis.payload'
assert PAYLOAD.stat().st_size==1486927467

def digest(p):
    d=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1<<20),b''):d.update(b)
    return d.hexdigest()
assert digest(SOURCE)=='5ad196fa588e5d223343896b890328072e8df984ff46c679cfdcb68a86b25dd2'
class Aggregate:
    def __init__(self):
        self.count=0;self.kinds=Counter();self.stacks=defaultdict(list);self.names={};self.stats={};self.bins=defaultdict(int);self.counter_bins={};self.retained=[];self.frames=[];self.long=[];self.gpu={};self.gpu_begin={};self.gpu_frames=[];self.first=2**63;self.last=0;self.bad=0;self.started=time.monotonic()
    def __iter__(self):return iter(self.retained)
    def segment(self,tid,name,a,b):
        if b<=a:return
        while a<b:
            sec=a//1000000000;z=min(b,(sec+1)*1000000000)
            self.bins[tid,sec,name]+=z-a;a=z
    def append(self,e):
        self.count+=1
        if self.count>100_000_000:raise RuntimeError('100 million event bound exceeded')
        if self.count%2_000_000==0:
            (ROOT/'analysis-progress.json').write_text(json.dumps({'events':self.count,'seconds':time.monotonic()-self.started,'bins':len(self.bins),'long_scopes':len(self.long)}))
            if len(self.bins)>2_000_000 or len(self.long)>500_000:raise RuntimeError('aggregate cardinality bound exceeded')
        self.kinds[e.kind]+=1
        if e.kind.startswith('Gpu'):
            key=e.gpu_ctx,e.gpu_slot
            if e.kind=='GpuZoneBegin':self.gpu_begin[key]=(e.ts_ns,e.name,e.gpu_ctx_name)
            elif e.kind=='GpuZoneEnd':
                v=self.gpu_begin.pop(key,None)
                if v and e.ts_ns>=v[0]:
                    k=(e.gpu_ctx,v[1],v[2]);self.gpu.setdefault(k,array.array('d')).append((e.ts_ns-v[0])/1e6)
                    if v[1]=='GPU.GuestFrame':self.gpu_frames.append([e.gpu_ctx,v[0],e.ts_ns])
            return
        t=e.ts_ns;tid=e.tid
        self.first=min(self.first,t);self.last=max(self.last,t)
        if e.thread_name:self.names[tid]=e.thread_name
        if e.kind in ('Instant','RegionBegin','RegionEnd'):
            if len(self.retained)>=100000:raise RuntimeError('metadata bound exceeded')
            self.retained.append(e)
        if e.kind=='SpanBegin':
            s=self.stacks[tid]
            if s:self.segment(tid,s[-1][0],s[-1][3],t)
            s.append([e.name,t,0,t])
        elif e.kind=='SpanEnd':
            s=self.stacks[tid]
            if not s:self.bad+=1;return
            name,a,ch,last=s.pop()
            if name!=e.name or t<a:self.bad+=1;s.clear();return
            self.segment(tid,name,last,t)
            duration=t-a
            if s:s[-1][2]+=duration;s[-1][3]=t
            g=self.stats.setdefault((tid,name),[0,0,0,0])
            g[0]+=1;g[1]+=duration;g[2]+=max(0,duration-ch);g[3]=max(g[3],duration)
            if duration>=20_000_000 or name.startswith(('Texture.','GPU.Compile','Startup.')):
                self.long.append([tid,name,a,t,max(0,duration-ch)])
        elif e.kind=='FrameMark':self.frames.append([tid,t,e.i64,e.u64])
        elif e.kind=='Counter':
            k=(tid,t//1000000000,e.name);v=e.i64
            c=self.counter_bins.setdefault(k,[t,t,v,v,v,v,0,0])
            if t<c[0]:c[0]=t;c[2]=v
            if t>=c[1]:c[1]=t;c[3]=v
            c[4]=min(c[4],v);c[5]=max(c[5],v);c[6]+=v;c[7]+=1
    def save(self,result):
        gpu=[]
        for (ctx,name,ctxname),values in self.gpu.items():
            v=sorted(values);n=len(v)
            gpu.append({'ctx':ctx,'name':name,'ctx_name':ctxname,'count':n,'total_ms':sum(v),'p50_ms':v[n//2],'p95_ms':v[min(n-1,int(n*.95))],'max_ms':v[-1]})
        report={'source':str(SOURCE),'sha256':digest(SOURCE),'sdk_revision':FOUNDATION_REVISION,'events':self.count,'kinds':self.kinds,'start_ns':self.first,'end_ns':self.last,'elapsed_ms':(self.last-self.first)/1e6,'threads':self.names,'scope_stats':[{'tid':k[0],'name':k[1],'count':v[0],'inclusive_ms':v[1]/1e6,'self_ms':v[2]/1e6,'max_ms':v[3]/1e6} for k,v in self.stats.items()],'cpu_self_bins':[[*k,v] for k,v in self.bins.items()],'counter_bins':[[*k,*v] for k,v in self.counter_bins.items()],'metadata':[asdict(e) for e in self.retained],'frames':self.frames,'long_scopes':self.long,'gpu':gpu,'gpu_frames':self.gpu_frames,'gpu_records':asdict(result.gpu_records),'diagnostics':result.diagnostics,'truncated':result.truncated,'unmatched_ends':self.bad,'open_scopes':sum(map(len,self.stacks.values())),'analysis_seconds':time.monotonic()-self.started,'scope':'Closed-scope totals; self bins include known prefixes of open parents. CPU elapsed includes waits; no scheduler or calibrated GPU/CPU join.'}
        (ROOT/'full-analysis.json').write_text(json.dumps(report,separators=(',',':')))
        print(json.dumps({k:report[k] for k in ['events','elapsed_ms','diagnostics','truncated','unmatched_ends','open_scopes','analysis_seconds']}),flush=True)
with SOURCE.open('rb') as f:
    c=D.Cursor(f.read(132*1024));assert c.read(4)==b'PROF' and c.u32()==3;r=D.ReadResult();D._read_session_header(c,r)
a=Aggregate();r.events=a
with PAYLOAD.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as m:D._decode_payload(m,r)
require_supported_tags(r,FOUNDATION_REVISION)
a.save(r)
