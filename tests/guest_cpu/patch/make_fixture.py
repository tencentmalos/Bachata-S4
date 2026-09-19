#!/usr/bin/env python3
"""Authored x86-64 fixture and exact identity recipe for patch tests."""
import argparse, importlib.util, json, subprocess
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--clang',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args(); root=Path(__file__).resolve().parents[3]; src=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('builder',root/'tools/guest-functions/build.py');b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
a.out.mkdir(parents=True,exist_ok=True)
subprocess.run([str(a.clang),'--target=x86_64-none-elf','-c',str(src/'target.S'),'-o',str(a.out/'target.o')],check=True)
subprocess.run([str(a.clang.parent/'ld.lld'),'-static','-e','target_sum8','-T',str(root/'guest/custom/v1/payload.ld'),'-o',str(a.out/'target.elf'),str(a.out/'target.o')],check=True)
data,h,sections,names,symbols,take=b.read_elf(a.out/'target.elf')
import struct
ph=struct.unpack_from('<IIQQQQQQ',data,h[5]);blob=take(ph[2],ph[5]);(a.out/'target.bin').write_bytes(blob)
hooks=[]
for name,prototype in [('sum8','U64 original_sum8(U64,U64,U64,U64,U64,U64,U64,U64)'),('rip','U64 original_rip(U64)'),('call','U64 original_call(U64)'),('branch','U64 original_branch(U64)'),('float','double original_float(double,double)'),('sret','struct Triple original_sret(U64)'),('varargs','U64 original_varargs(U64,...)')]:
    address,size,_,_=symbols['target_'+name]
    hooks.append(dict(name=name,original='original_'+name,replacement='patch_'+name,prototype=prototype,
                      evidence='Authored SysV fixture tests/guest_cpu/patch/target.S; sole public entry; no interior callers',
                      offset=address,expected=blob[address:address+min(size,32)].hex()))
recipe=dict(schema='shadps4.guest-functions.recipe.v1',abi='x86_64-sysv',id='fixture',title='SHAD_PATCH_TEST',module='target.elf',
            module_sha256=b.sha(data),hooks=hooks,headers=[str(src/'abi.h')],sources=[str(src/n) for n in ('patch.cpp','helpers.c','helpers.S')],
            exports=['probe_fp_sret','probe_public','probe_sdk','read_count','loop_public','probe_varargs','probe_callee_saved'],counters=[dict(id=1,name='duration_ns'),dict(id=2,name='calls')],logs=[dict(id=1,name='fixture_probe')])
recipe['bindings'] = [dict(name='guest_helper',kind='function',offset=symbols['helper'][0],
    expected=blob[symbols['helper'][0]:symbols['helper'][0]+6].hex(),prototype='U64 guest_helper(void)',
    evidence='Authored unhooked x86 helper returns 9'),
    dict(name='guest_count',kind='data',offset=0x3000,size=8,alignment=8,access='rw',type='U64',evidence='Owned fixture guest RW storage'),
    dict(name='guest_bias',kind='data',offset=0x3008,size=8,alignment=8,access='r',type='U64',evidence='Owned fixture guest read binding')]
recipe['exports'].append('probe_bindings')
(a.out/'recipe.json').write_text(json.dumps(recipe,indent=2)+'\n')
b.build(a.out/'recipe.json',a.clang,a.out/'package')
