import sys,json,ctypes,collections
from pathlib import Path
root=Path('/Users/bytedance/workspace/spatial_mcp_publish/dev_tools/mcp/litep')
sys.path[:0]=[str(root),str(root/'sdk/skills/spatial-trace-analyzer/scripts')]
import stage_file
lz=ctypes.CDLL('/opt/homebrew/lib/liblz4.dylib');lz.LZ4_decompress_safe.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int];lz.LZ4_decompress_safe.restype=ctypes.c_int

def decompress(data,size):
 if not 0<size<=stage_file.MAX_CHUNK:raise ValueError('chunk budget')
 out=ctypes.create_string_buffer(size)
 if lz.LZ4_decompress_safe(data,out,len(data),size)!=size:raise ValueError('LZ4 length mismatch')
 return out.raw
stage_file.bounded_lz4=decompress
class Reduced:
 def __init__(self,start,end,check,progress):
  self.stacks=collections.defaultdict(list);self.rows={};self.events=0;self.bookmarks=[];self.progress={};self.bad=0;self.threads={};self.io=[];self.io_fields=collections.defaultdict(dict);self.paths={};self.vm=[];self.progress_samples=[]
 def __iter__(self):return iter(())
 def append(self,e):
  self.events+=1
  if self.events>60000000:raise ValueError('event budget')
  if self.events%2000000==0:print(self.events,flush=True)
  self.threads[e.tid]=e.thread_name
  if e.kind=='Instant':
   self.bookmarks.append(dict(t=e.ts_ns,tid=e.tid,name=e.name))
   if e.name.startswith('Storage.Open.'):
    fd,path=e.name[len('Storage.Open.'):].split(':',1);self.paths[int(fd)]=path
   elif e.name=='Storage.Read.Complete':
    row=self.io_fields.pop(e.tid,{})
    if len(row)!=7:raise ValueError('incomplete IO record')
    row.update(t=e.ts_ns,tid=e.tid,path=self.paths.get(row['Fd'],'unknown'))
    self.io.append(row)
  if e.kind=='Counter' and e.name.startswith('Storage.Read.'):
   self.io_fields[e.tid][e.name[len('Storage.Read.'):]]=e.i64
  if e.kind=='SpanBegin':self.stacks[e.tid].append((e.name,e.ts_ns))
  elif e.kind=='SpanEnd':
   if not self.stacks[e.tid]:self.bad+=1;return
   name,t=self.stacks[e.tid].pop()
   if name!=e.name or e.ts_ns<t:self.bad+=1;self.stacks[e.tid].clear();return
   if name.startswith('VM.'):
    self.vm.append(dict(tid=e.tid,name=name,begin_ns=t,end_ns=e.ts_ns,elapsed_ns=e.ts_ns-t))
   if not name.startswith(('HLE.scePthreadMutex','HLE.pthread_mutex','HLE.scePthreadCond','HLE.pthread_cond','HLE.sceKernelUsleep','HLE.sceKernelPread','HLE.sceKernelRead','GNM.SubmissionGate','Storage.','VM.')):return
   # Whole-capture per-owner elapsed, no claim of CPU cycles or a critical path.
   key=e.tid,name;g=self.rows.setdefault(key,dict(tid=e.tid,name=name,count=0,elapsed_ns=0,max_ns=0))
   g['count']+=1;g['elapsed_ns']+=e.ts_ns-t;g['max_ns']=max(g['max_ns'],e.ts_ns-t)
  elif e.kind=='Counter' and e.name.startswith('VideoOut.'):
   self.progress_samples.append(dict(t=e.ts_ns,tid=e.tid,name=e.name,value=e.i64))
   key=e.tid,e.name;g=self.progress.setdefault(key,dict(tid=e.tid,name=e.name,first=e.i64,last=e.i64,first_ns=e.ts_ns,last_ns=e.ts_ns))
   if e.ts_ns<g['first_ns']:g.update(first=e.i64,first_ns=e.ts_ns)
   if e.ts_ns>=g['last_ns']:g.update(last=e.i64,last_ns=e.ts_ns)
 def finish(self,kind,label):
  for row in self.io: row['path']=self.paths.get(row['Fd'],'unknown')
  entries=[x for x in self.bookmarks if x['name']=='Startup.GuestEntry'];main=entries[0]['tid'] if len(entries)==1 else None
  starts=[x for x in self.bookmarks if x['name'].startswith('Session.Generation.')]
  present=[x for x in self.bookmarks if x['name']=='Startup.FirstGuestPresent']
  return dict(vm=self.vm,progress_samples=self.progress_samples,io=self.io,paths=self.paths,events=self.events,unmatched=self.bad,open_scopes=sum(map(len,self.stacks.values())),main_tid=main,threads=self.threads,first_present_ms=(present[0]['t']-starts[0]['t'])/1e6 if len(starts)==len(present)==1 else None,bookmarks=self.bookmarks,main=[g for g in self.rows.values() if g['tid']==main],rows=list(self.rows.values()),progress=list(self.progress.values()),diagnostics=[])
stage_file.StageReducer=Reduced
p=Path(sys.argv[1]);out=Path(sys.argv[2]);m=json.loads(Path(str(p)+'.json').read_text())
r=stage_file.reduce_file(p,m,{},lambda:None,lambda **k:None);out.write_text(json.dumps(r,indent=2));print('DONE',out,flush=True)
