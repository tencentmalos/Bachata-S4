#!/usr/bin/env python3
"""Opaque-entry ABI tests, including capacity beyond the former 32 hooks."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

p=argparse.ArgumentParser();p.add_argument('--clang',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[3];src=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('builder',root/'tools/guest-functions/build.py');b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
a.out.mkdir(parents=True,exist_ok=True)
target=(src/'target.S').read_text()+'\n.text\n'
for n in range(48):
    target+=f'.p2align 4\n.globl target_state{n}\n.type target_state{n},@function\ntarget_state{n}:\n.rept 5\nnop\n.endr\nret\n.size target_state{n},.-target_state{n}\n'
(a.out/'target.S').write_text(target)
subprocess.run([str(a.clang),'--target=x86_64-none-elf','-c',str(a.out/'target.S'),'-o',str(a.out/'target.o')],check=True)
subprocess.run([str(a.clang.parent/'ld.lld'),'-static','-e','target_sum8','-T',str(root/'guest/custom/v1/payload.ld'),'-o',str(a.out/'target.elf'),str(a.out/'target.o')],check=True)
data,h,sections,names,symbols,take=b.read_elf(a.out/'target.elf')
ph=struct.unpack_from('<IIQQQQQQ',data,h[5]);blob=take(ph[2],ph[5]);(a.out/'target.bin').write_bytes(blob)
hooks=[]
for name in ['sum8','rip','call','branch','float','sret','varargs']+[f'state{i}' for i in range(48)]:
    address,size,_,_=symbols['target_'+name]
    hooks.append(dict(name=name,original='original_'+name,replacement='entry_'+name,
        mode='entry-observer-x86_64-avx',observer='observe',prototype='opaque-machine-entry',
        evidence='Authored complete entry fixture; no middle entries; AVX opaque state adapter',
        offset=address,expected=blob[address:address+min(size,32)].hex()))
recipe=dict(schema='shadps4.guest-functions.recipe.v1',abi='x86_64-sysv',id='entry_fixture',
    title='SHAD_PATCH_TEST',module='target.elf',module_sha256=b.sha(data),hooks=hooks,
    sources=[str(src/'entry_observer.cpp'),str(src/'entry_observer.S')],
    exports=['entry_count','argument_count','probe_observer_fp','probe_entry_state','probe_entry_x87','probe_entry_varargs','loop_observer'],
    counters=[dict(id=1,name='entry_hit')])
(a.out/'recipe.json').write_text(json.dumps(recipe,indent=2)+'\n')
b.build(a.out/'recipe.json',a.clang,a.out/'package')
