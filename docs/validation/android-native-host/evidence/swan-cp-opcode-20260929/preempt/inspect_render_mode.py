from pathlib import Path
import sqlite3,struct,json,hashlib,argparse
p=argparse.ArgumentParser();p.add_argument('database',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
c=sqlite3.connect('file:'+str(a.database.resolve())+'?mode=ro',uri=True);c.row_factory=sqlite3.Row
identity=dict(c.execute('select * from snapshots').fetchone());raw=Path(identity['source_path']).read_bytes();assert hashlib.sha256(raw).hexdigest()==identity['sha256']
cur=dict(c.execute("select * from v_live_command_cursors where pipe_name='BR' and ib_level=1").fetchone());addr=cur['gpu_address'];count=cur['initial_dwords'];o=dict(c.execute('select * from gpu_objects where gpu_address<=? and gpu_address+size_bytes>=? order by size_bytes limit 1',(addr,addr+count*4)).fetchone());mem=struct.unpack_from(f'<{count}I',raw,o['payload_offset']+addr-o['gpu_address']);pos=0;packets=[]
def odd(v):return 1^(v.bit_count()&1)
while pos<count:
 h=mem[pos];kind=h>>28;assert kind in (4,7),(pos,hex(h));cnt=h&0x3fff if kind==7 else h&0x7f
 if kind==7:
  op=(h>>16)&127;assert ((h>>15)&1)==odd(cnt) and ((h>>23)&1)==odd(op) and not h&0x0f000000
 else:
  op=None;reg=(h>>8)&0x7ffff;assert ((h>>7)&1)==odd(cnt) and ((h>>27)&1)==odd(reg)
 assert pos+cnt+1<=count
 packets.append({'dw':pos,'op':op,'words':[hex(v) for v in mem[pos+1:pos+cnt+1]]});pos+=cnt+1
target=c.execute("select gpu_address from v_live_command_cursors where pipe_name='BR' and ib_level=2").fetchone()[0];matches=[]
for i,x in enumerate(packets):
 w=x['words']
 if x['op']==0x3f and len(w)==3 and int(w[0],16)+(int(w[1],16)<<32)==target:
  matches.append({'ib_call':x,'preceding_marker':next((y for y in reversed(packets[:i]) if y['op']==0x65),None),'nearby':packets[max(0,i-4):i+5]})
r={'snapshot_sha256':identity['sha256'],'BR_IB1_cursor':cur,'BR_IB2_address':hex(target),'pm4_packet_count':len(packets),'pm4_header_parity_and_length':'pass','all_matching_calls':matches,'marker_histogram':{x['words'][0]:sum(y['words']==x['words'] for y in packets if y['op']==0x65) for x in packets if x['op']==0x65},'boundary':'Static IB1 packet sequence with type/parity/length checks, no exact execution PC inferred. Not a full opcode semantic validator.'};a.output.write_text(json.dumps(r,indent=2)+'\n');print(json.dumps({k:r[k] for k in ['pm4_packet_count','pm4_header_parity_and_length','marker_histogram']}))
