#!/usr/bin/env python3
"""Original TMNT heap-wrapper replay; requires the exact local analysis image."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--analysis',type=Path,required=True);p.add_argument('--clang',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('builder',root/'tools/guest-functions/build.py');builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
data=a.analysis.read_bytes();assert builder.sha(data)=='504fc22628a2a7c563e593aab298b9273e2a80eaf8814a095ffb6f464152da99'
h=struct.unpack_from('<16sHHIQQQIHHHHHH',data);segs=[struct.unpack_from('<IIQQQQQQ',data,h[5]+i*h[9]) for i in range(h[10])]
size=(max(va+memsz for t,flags,off,va,pa,filesz,memsz,align in segs if t==1)+0x3fff)&~0x3fff
assert size<0x3ff0000
image=bytearray(size)
for t,flags,off,va,pa,filesz,memsz,align in segs:
 if t==1:image[va:va+filesz]=data[off:off+filesz]
def got(plt):
 assert image[plt:plt+2]==b'\xff\x25'
 return plt+6+struct.unpack_from('<i',image,plt+2)[0]
a.out.mkdir(parents=True,exist_ok=True);(a.out/'target.bin').write_bytes(image)
rp=root/'guest/games/CUSA50828/01.08/heap_stats.recipe.json';r=json.loads(rp.read_text());r['sources']=[str(rp.parent/s) for s in r['sources']];(a.out/'recipe.json').write_text(json.dumps(r,indent=2)+'\n')
builder.build(a.out/'recipe.json',a.clang.resolve(),a.out/'package')
(a.out/'recompile.json').write_text(json.dumps(dict(module_size=size,image_sha256=builder.sha(image),analysis_sha256=builder.sha(data),canary_got=0x1d3b1b8,heap_slot=0x1e16ac8,malloc_got=got(0x15c0370),free_got=got(0x15c0380),stats_got=got(0x15c02d0)),indent=2)+'\n')
