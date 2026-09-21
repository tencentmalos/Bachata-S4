#!/usr/bin/env python3
"""Decode bounded Beat Saber guest-call diagnostics; raw blocks are authoritative.

Usage: decode_vr_call_log.py RUN.log
Provide one run and one diagnostic package per input file (filter by package= first).
Unknown opaque layer values are not labelled as poses or projection matrices.
"""
import re,json,struct,sys,collections,math
from pathlib import Path
src=Path(sys.argv[1]); records=[]; active={};bad=0
for line in src.read_text(errors='replace').splitlines():
 m=re.search(r'tag=GuestPatch\.[^.]+\.([^ ]+) value=(-?\d+) context=(\d+) thread=(\d+) generation=(\d+) invocation=(\d+)',line)
 if not m:continue
 tag,value,ctx,thread,gen,inv=m.groups();value=int(value);key=(ctx,thread,gen)
 if tag=='Call':
  if key in active:bad+=1
  active[key]={'call':value,'owner':key,'blocks':{},'args':{},'first_line':line.split('[PATCH_LOG]')[0]}
  continue
 r=active.get(key)
 if r is None:bad+=1;continue
 if tag in ['Sequence','Phase','Return']:r[tag.lower()]=value
 elif tag.startswith('Arg'):r['args'][tag[3:]]=value
 elif tag=='Block':r['_block']=str(value);r['blocks'][str(value)]={'bytes':[]}
 elif tag=='ByteCount':r['blocks'][r['_block']]['size']=value
 elif tag=='Word':r['blocks'][r['_block']]['bytes']+=list((value&((1<<64)-1)).to_bytes(8,'little'))
 elif tag=='End':
  r.pop('_block',None)
  for b in r['blocks'].values():b['hex']=bytes(b.pop('bytes')[:b['size']]).hex()
  records.append(r);del active[key]
summary={'records':len(records),'incomplete':len(active),'malformed':bad,'counts':dict(collections.Counter((str(r['call'])+':'+str(r.get('phase'))) for r in records))}
src.with_suffix('.decoded.json').write_text(json.dumps({'summary':summary,'records':records},indent=2))
print(json.dumps(summary))
for r in records:
 c=r['call']; phase=r.get('phase'); blocks={k:bytes.fromhex(v['hex']) for k,v in r['blocks'].items()}
 if c==12:
  print('UNITY',r.get('sequence'),*[b.decode('utf8','replace') for k,b in blocks.items() if k!='0'],sep=' | ')
 elif phase==1 and c==6 and '1' in blocks:
  b=blocks['1'];p=blocks['0'];poses=[struct.unpack_from('<8f',b,0x80+i*64) for i in range(4)]
  print('TRACKER',r['sequence'],'handle',struct.unpack_from('<i',p,4)[0],'result',r['return'],'status',struct.unpack_from('<III',b,0x34),'camera_quaternion',struct.unpack_from('<4f',b,0x70),'poses',poses)
 elif phase==1 and c==9:
  for k,b in blocks.items():
   if k in ['10','11']:print('SUBMIT',r['sequence'],'result',r['return'],'opaque28_floats',struct.unpack_from('<16f',b,40),'kind',struct.unpack_from('<I',b,120)[0])
 elif c==11:
  print('SELECT',phase,r.get('args'),{k:(v.decode('utf8','replace') if k!='0' else v.hex()) for k,v in blocks.items()})
 elif phase==1 and c in [1,2,3,4,5,7,8,10]:print('CALL',c,r['sequence'],'return',r['return'],'blocks',{k:b.hex() for k,b in blocks.items()})
