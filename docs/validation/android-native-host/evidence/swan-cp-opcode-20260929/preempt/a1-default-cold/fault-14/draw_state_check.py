from pathlib import Path
import json,sqlite3,struct,sys
p=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path(__file__).resolve().parent;c=sqlite3.connect(p/'devcd14.sqlite3');c.row_factory=sqlite3.Row
raw=next(p.glob('*.bin')).read_bytes();objects=[dict(r) for r in c.execute('select * from gpu_objects')]
def read(addr,count):
 o=next((o for o in objects if o['gpu_address']<=addr and o['gpu_address']+o['size_bytes']>=addr+count*4),None)
 return list(struct.unpack_from(f'<{count}I',raw,o['payload_offset']+addr-o['gpu_address'])) if o else None
def decode(mem):
 pos=0;pack=[]
 while pos<len(mem):
  h=mem[pos];t=h>>28
  if t not in (4,7):return pack,{'offset':pos,'word':hex(h),'reason':'invalid header type'}
  cnt=h&0x3fff if t==7 else h&0x7f
  if pos+cnt+1>len(mem):return pack,{'offset':pos,'count':cnt,'reason':'packet extends past group'}
  pack.append((pos,t,(h>>16)&127,cnt));pos+=cnt+1
 return pack,None
addr=283411919340;mem=read(addr,1395);pack,err=decode(mem);refs=[];seen=set()
for dw,t,op,cnt in pack:
 if t!=7 or op!=0x43:continue
 assert cnt%3==0
 for i in range(dw+1,dw+cnt+1,3):
  flag,lo,hi=mem[i:i+3];n=flag&0xffff;a=lo+(hi<<32)
  if not n or not a or (a,n) in seen:continue
  seen.add((a,n));child=read(a,n);pp,ee=decode(child) if child else ([],None)
  refs.append({'sds_dw':dw,'group':(flag>>24)&31,'flags':hex(flag),'address':hex(a),'dwords':n,'captured':child is not None,'packets':len(pp),'parse_error':ee,'crosses_page':(a&4095)+n*4>4096})
result={'ib2_packets':len(pack),'ib2_error':err,'nop_packets':sum(t==7 and op==0x10 for dw,t,op,n in pack),'sds_cross_page': [{'dw':dw,'address':hex(addr+dw*4),'dwords':n+1} for dw,t,op,n in pack if t==7 and op==0x43 and ((addr+dw*4)&4095)+(n+1)*4>4096],'unique_draw_states':refs,'boundary':'Static PM4 shape and captured payload only; not runtime DDE execution proof; opcode-specific semantics and private DDE FIFO layout unverified'}
(p/'draw-state-check.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
