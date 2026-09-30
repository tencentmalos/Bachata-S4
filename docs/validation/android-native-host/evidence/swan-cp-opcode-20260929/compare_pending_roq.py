from pathlib import Path
import sqlite3,struct,json
cases=[(3,'build/validation/turnip-cp-opcode-20260928/parsed/devcd3.sqlite3'),(4,'build/validation/turnip-cp-opcode-20260928/parsed/devcd4.sqlite3'),(9,'build/validation/swan-turnip-repair-20260929/devcd9.sqlite3'),(12,'build/validation/swan-cp-live-20260929/fault-01/devcd12.sqlite3')]
out=[]
for n,path in cases:
 db=sqlite3.connect(path);db.row_factory=sqlite3.Row
 c=dict(db.execute("select * from v_live_command_cursors where pipe_name='BR' and ib_level=2").fetchone());snap=dict(db.execute('select * from snapshots').fetchone());raw=Path(snap['source_path']).read_bytes();addr=c['gpu_address'];count=c['initial_dwords'];o=dict(db.execute('select * from gpu_objects where gpu_address<=? and gpu_address+size_bytes>=? order by size_bytes limit 1',(addr,addr+4*count)).fetchone());mem=struct.unpack_from(f'{count}I',raw,o['payload_offset']+addr-o['gpu_address']);si=db.execute("select section_index from v_indexed_register_inventory where pipe_name='BR' and source_profile_name LIKE 'GEN8_CP_ROQ_DBG%' limit 1").fetchone()[0];roq=[r[0] for r in db.execute('select value from indexed_register_values where section_index=? order by logical_index',(si,))];fetched=count-c['remaining_dwords'];inds=range(max(0,fetched-512),fetched);scores=[sum(mem[d]!=0 and mem[d]==roq[512+(d+k)%512] for d in inds) for k in range(512)];rot=scores.index(max(scores));remaining=db.execute("select ib2_remaining_dwords from roq_states where pipe_name='BR'").fetchone()[0];candidate=fetched-remaining
 z=candidate
 while z>max(0,fetched-512) and roq[512+(z-1+rot)%512]==0:z-=1
 tail=[roq[512+(d+rot)%512] for d in range(candidate,fetched)]
 out.append(dict(snapshot=n,ib2=hex(addr),count=count,rotation=rot,best_count=scores.count(max(scores)),inferred_consumed=candidate,pending_dwords=remaining,pending_all_zero=all(v==0 for v in tail),pending_nonzero_cpu=sum(mem[d]!=0 for d in range(candidate,fetched)),zero_run_start_dw=z,zero_run_start_va=hex(addr+4*z),zero_run_start_page_offset=hex((addr+4*z)&4095)))
print(json.dumps(out,indent=2));Path('build/validation/swan-cp-live-20260929/pending-roq-comparison.json').write_text(json.dumps(out,indent=2))
