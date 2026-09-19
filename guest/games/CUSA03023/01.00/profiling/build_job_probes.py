#!/usr/bin/env python3
"""血源 01.00：从原始 EH 边界生成定点计时；不依赖 IDA 的函数合并结果。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

SOURCE_SHA = 'cc2826ae36b4df515d4b2f4ff9cb18a24057165d95cc51c3413ff98adf704209'
ANALYSIS_SHA = 'd7196926a386317f92f28b5f2f0128837523ffd0e5b5c364f773bcc70592d27f'
SELF_SHA = '6764938b23539d29c936bca9880fc4a774e7b0099ce31c7e8c4b0f8bd0befb80'
ROOTS = {
    0x1915760: 'CharacterUpdateCoordinator',
    0x19177f0: 'CharacterBehaviorAndWorkerPhases',
    0x20336c0: 'CSEzWorkExecuteTask',
    0x1a655d0: 'EnqueueCharacterComponentUpdate',
    0x1a656e0: 'EnqueueCharacterVirtual108',
    0x1a657f0: 'EnqueuePhaseSetup',
    0x18c0010: 'CharacterComponentUpdateThunk',
    0x15158e0: 'CharacterComponentUpdate',
    0x1a66390: 'CharacterMemberTaskExecute',
    0x1513590: 'CharacterTransformCommitCandidate',
    0x12a4680: 'TargetBankUpdate',
    0x18c0030: 'CharacterVirtual108Candidate',
}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def build(source, analysis, output, objdump):
    raw, image = source.read_bytes(), analysis.read_bytes()
    if sha(raw) != SOURCE_SHA or sha(image) != ANALYSIS_SHA:
        raise ValueError('Exact source/analysis SHA required')
    phoff = struct.unpack_from('<Q', raw, 32)[0]
    phnum = struct.unpack_from('<H', raw, 56)[0]
    headers = [struct.unpack_from('<IIQQQQQQ', raw, phoff+i*56) for i in range(phnum)]
    loads = [p for p in headers if p[0] == 1]
    for p in loads:
        if raw[p[2]:p[2]+p[5]] != image[p[2]:p[2]+p[5]]:
            raise ValueError('Analysis LOAD payload differs')
    def file_offset(va, size):
        for p in loads:
            if p[3] <= va and va+size <= p[3]+p[5]:
                return p[2]+va-p[3]
        raise ValueError('Address outside file-backed LOAD')
    hdr = next(p for p in headers if p[0] == 0x6474e550)
    off, base = hdr[2], hdr[3]
    if raw[off:off+4] != bytes.fromhex('011b033b'):
        raise ValueError('Unsupported EH table encoding')
    count = struct.unpack_from('<I', raw, off+8)[0]
    if 12+count*8 > hdr[5]:
        raise ValueError('Invalid EH table count')
    bounds = {}
    for i in range(count):
        start, fde = (base+v for v in struct.unpack_from('<ii', raw, off+12+i*8))
        if start not in ROOTS:
            continue
        f = file_offset(fde, 16)
        cie = fde+4-struct.unpack_from('<I', raw, f+4)[0]
        c = file_offset(cie, 24)
        # This exact-build recipe supports the observed zR / pcrel sdata4 CIE only.
        if raw[c:c+24] != bytes.fromhex('1400000000000000017a5200017810011b0c070890010000'):
            raise ValueError('Unverified CIE')
        pc = fde+8+struct.unpack_from('<i', raw, f+8)[0]
        size = struct.unpack_from('<I', raw, f+12)[0]
        if pc != start or not 0 < size <= 131072 or start in bounds:
            raise ValueError('Invalid/duplicate FDE bounds')
        bounds[start] = size
    if set(bounds) != set(ROOTS):
        raise ValueError('Missing selected EH function')
    output.mkdir(parents=True, exist_ok=False)
    probes, evidence = [], []
    def site(i):
        return dict(offset=i[0], expected=i[1].hex())
    for start, name in ROOTS.items():
        end = {0x1915760: 0x19176c4, 0x19177f0: 0x1918af4}.get(start, start+bounds[start])
        # Both excluded tails are RIP-relative switch tables after the stack-failure call.
        # Keep their full EH sizes in evidence; never decode table bytes as probes.
        text = subprocess.check_output([objdump, '-d', '--start-address='+hex(start),
            '--stop-address='+hex(end), str(analysis)], text=True)
        (output/(name+'.asm.txt')).write_text(text)
        ins = []
        for line in text.splitlines():
            m = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)\s*(\S.*)$', line)
            if not m:
                continue
            va, data, asm = int(m[1], 16), bytes.fromhex(m[2]), m[3]
            if va < start or va+len(data) > end or not 1 <= len(data) <= 15 or '<unknown>' in asm:
                raise ValueError('Invalid decoded instruction: '+line)
            if raw[file_offset(va,len(data)):file_offset(va,len(data))+len(data)] != data:
                raise ValueError('Disassembly bytes differ')
            ins.append((va, data, asm))
        if not ins or ins[0][0] != start or ins[-1][0]+len(ins[-1][1]) != end:
            raise ValueError('Incomplete disassembly coverage')
        for a,b in zip(ins,ins[1:]):
            if a[0]+len(a[1]) != b[0]:
                raise ValueError('Disassembly gap')
            if not re.match(r'^callq?\s', a[2]):
                continue
            p = dict(id=len(probes)+1, name=name+' / call '+hex(a[0]), kind='call',
                begin=site(a), end=site(b), function_offset=start, function_name=name,
                depth=1, confidence='EH-bound-static-callsite',
                prototype_status='unknown-not-needed-for-probe')
            target=re.match(r'^callq?\s+0x([0-9a-f]+)', a[2])
            if target:
                p['target_offset']=int(target[1],16)
            probes.append(p)
        probes.append(dict(id=len(probes)+1,name=name+' / entry',kind='count',
            begin=site(ins[0]),function_offset=start,function_name=name,depth=1))
        evidence.append(dict(offset=start,size=bounds[start],code_end=end,name=name,
            bytes_sha256=sha(raw[file_offset(start,bounds[start]):file_offset(start,bounds[start])+bounds[start]])))
    if len(probes)>1024 or len({p['begin']['offset'] for p in probes})!=len(probes):
        raise ValueError('Probe capacity/uniqueness failure')
    profile=dict(schema='spatial.guest-auto-tag.v1',architecture='x86_64',abi='x86_64-sysv',
        endianness='little',pointer_bits=64,
        identity=dict(emulator='shadps4',title='CUSA03023',module='eboot.bin',module_sha256=SELF_SHA),
        analysis=dict(module='main',sha256=ANALYSIS_SHA,imagebase=0),depth=1,max_probes=0,
        selected_targets=[dict(offset=a) for a in ROOTS],probes=probes,
        provenance=dict(backend='llvm-objdump-and-original-EH',source_sha256=SOURCE_SHA,
            llvm_version=subprocess.check_output([objdump,'--version'],text=True).splitlines()[0],
            coverage='All calls in explicitly selected EH functions; entries count only; indirect targets unresolved',
            requires_explicit_next_iteration=True,functions=evidence))
    path=output/'auto-tag.json'
    path.write_text(json.dumps(profile,indent=2)+'\n')
    print(json.dumps(dict(profile=str(path.resolve()),sha256=sha(path.read_bytes()),probes=len(probes))))

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--analysis',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--objdump',default='llvm-objdump')
    a=p.parse_args()
    build(a.source,a.analysis,a.output,a.objdump)
