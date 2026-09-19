import sys,json,collections,ctypes
from pathlib import Path
root=Path('/Users/bytedance/workspace/spatial_mcp_publish/dev_tools/mcp/litep')
sys.path[:0]=[str(root),str(root/'sdk/skills/spatial-trace-analyzer/scripts')]
import stage_file
lz=ctypes.CDLL('/opt/homebrew/lib/liblz4.dylib');lz.LZ4_decompress_safe.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int];lz.LZ4_decompress_safe.restype=ctypes.c_int
def decompress(data,size):
 if size>stage_file.MAX_CHUNK:raise ValueError('chunk budget')
 out=ctypes.create_string_buffer(size)
 if lz.LZ4_decompress_safe(data,out,len(data),size)!=size:raise ValueError('LZ4 length mismatch')
 return out.raw
stage_file.bounded_lz4=decompress
class SyncReducer:
 def __init__(self,start,end,check,progress):
  self.start,self.end=start,end;self.stack=collections.defaultdict(list);self.events=0;self.rows={};self.phases=[];self.owners=collections.defaultdict(dict);self.bookmarks=[];self.bad=0
 def __iter__(self):return iter(())
 def append(self,e):
  self.events+=1
  if self.events%1000000==0:print(self.events,flush=True)
  if e.kind=='Instant':self.bookmarks.append(dict(t=e.ts_ns,tid=e.tid,name=e.name))
  if e.kind=='Counter' and e.name.startswith('GuestSync.Owner'):self.owners[e.tid][e.name[10:]]=e.i64
  if e.kind=='SpanBegin':self.stack[e.tid].append([e.name,e.ts_ns,{},0])
  elif e.kind=='Counter' and e.name.startswith('GuestSync.') and self.stack[e.tid]:
   f=self.stack[e.tid][-1][2];f[e.name[10:]]=e.i64
   if e.name=='GuestSync.CondPhase' and self.start<=e.ts_ns<self.end:
    self.phases.append(dict(t=e.ts_ns,tid=e.tid,name=self.stack[e.tid][-1][0],**f))
    if len(self.phases)>1000000:raise ValueError('phase budget')
  elif e.kind=='SpanEnd':
   if not self.stack[e.tid]:self.bad+=1;return
   name,t,f,child=self.stack[e.tid].pop()
   if name!=e.name:self.bad+=1;self.stack[e.tid].clear();return
   dt=max(0,min(e.ts_ns,self.end)-max(t,self.start))
   if self.stack[e.tid]:self.stack[e.tid][-1][3]+=dt
   if not dt or not f:return
   key=(e.tid,name,*(f.get(k) for k in ['Caller','Parent0','Parent1','Parent2','Parent3','Parent4','Parent5','Arg0','Arg1','Result']))
   # Usleep arg1 is not an argument; do not accidentally split by scratch GPRs.
   if 'sleep' in name.lower():key=key[:-2]+(None,key[-1])
   g=self.rows.setdefault(key,dict(tid=e.tid,name=name,count=0,elapsed_ns=0,self_ns=0,max_ns=0,first_ns=t,last_ns=t,fields=dict(f)))
   g['count']+=1;g['elapsed_ns']+=dt;g['self_ns']+=max(0,dt-child);g['max_ns']=max(g['max_ns'],dt);g['last_ns']=max(g['last_ns'],t)
   if len(self.rows)>100000:raise ValueError('group budget')
 def finish(self,kind,label):return dict(label=label,start_ns=self.start,end_ns=self.end,events=self.events,unmatched=self.bad,rows=sorted(self.rows.values(),key=lambda r:r['elapsed_ns'],reverse=True),phases=sorted(self.phases,key=lambda r:r['t']),owners=dict(self.owners),bookmarks=self.bookmarks,diagnostics=[],open_scopes=sum(map(len,self.stack.values())))
stage_file.StageReducer=SyncReducer
p=Path(sys.argv[1]);out=Path(sys.argv[2]);a,b=map(int,sys.argv[3:5]);m=json.loads(Path(str(p)+'.json').read_text())
r=stage_file.reduce_file(p,m,dict(start_ns=a,end_ns=b,label=out.stem),lambda:None,lambda **k:None)
out.write_text(json.dumps(r,separators=(',',':')))
print('DONE',out,len(r['rows']),len(r['phases']),flush=True)
