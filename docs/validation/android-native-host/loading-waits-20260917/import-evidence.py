import json,struct,re,hashlib
from pathlib import Path
def elf(path):
 b=Path(path).read_bytes();phoff=struct.unpack_from('<Q',b,32)[0];sz,n=struct.unpack_from('<HH',b,54)
 ph=[struct.unpack_from('<IIQQQQQQ',b,phoff+i*sz) for i in range(n)]
 dyn=next(p for p in ph if p[0]==2);data=next(p for p in ph if p[0]==0x61000000)[2]
 tags=dict(struct.unpack_from('<QQ',b,dyn[2]+i) for i in range(0,dyn[5],16))
 strings=data+tags[0x61000035];syms=data+tags[0x61000039]
 def symbol(i):
  name,info,other,section,value,size=struct.unpack_from('<IBBHQQ',b,syms+24*i)
  return b[strings+name:b.index(b'\0',strings+name)].decode(),value,size
 def bytes_at(va,n):
  p=next(p for p in ph if p[0] in (1,0x61000010) and p[3]<=va< p[3]+p[5])
  return b[p[2]+va-p[3]:p[2]+va-p[3]+n]
 rel={}
 if 0x61000029 in tags:
  for i in range(0,tags[0x6100002d],24):
   offset,info,addend=struct.unpack_from('<QQq',b,data+tags[0x61000029]+i);rel[offset]=symbol(info>>32)
 return b,tags,rel,symbol,bytes_at
root=Path(__file__).resolve().parent
aero=dict(re.findall(r'STUB\("([^"]+)", ([^)]+)\)',Path('src/core/aerolib/aerolib.inl').read_text()))
path='build/graphics-toolkit-review/renderdoc/tmnt-font-check.elf';b,t,rel,sym,at=elf(path);rows=[]
for plt in [0x15c02d0,0x15c0370,0x15c0380,0x15c1570,0x15c08a0]:
 code=at(plt,6);assert code[:2]==b'\xff\x25';got=plt+6+struct.unpack('<i',code[2:])[0];symbol,value,size=rel[got];nid=symbol.split('#')[0]
 rows.append(dict(plt=hex(plt),got=hex(got),symbol=symbol,nid=nid,name=aero.get(nid)))
for module,path2 in [('libc.prx','build/sysmodule-review/libc-inspect.elf'),('libfmodstudio.prx','build/graphics-toolkit-review/libfmodstudio.prx.elf')]:
 b2,t2,rel2,sym2,at2=elf(path2)
 for i in range(t2[0x6100003f]//24):
  name,value,size=sym2(i)
  for row in rows:
   if row['nid']==name.split('#')[0] and value:row.update(provider=module,export=hex(value),export_size=size,provider_sha256=hashlib.sha256(b2).hexdigest())
(root/'imports.json').write_text(json.dumps(dict(image_sha256=hashlib.sha256(b).hexdigest(),imports=rows),indent=2));print(json.dumps(rows,indent=2))
