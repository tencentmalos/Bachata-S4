#!/usr/bin/env python3
"""Prepare isolated differential replay from the user's exact local TMNT image.
No game bytes are stored in the repository. Original instruction bytes are retained;
only loader GOT pointers are supplied by the replay harness.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--analysis', type=Path, required=True)
p.add_argument('--clang', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--frame-dispatch', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('builder', root/'tools/guest-functions/build.py')
builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
data = a.analysis.read_bytes()
h = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
def take(offset, length):
    assert 0 <= offset <= len(data) and 0 <= length <= len(data) - offset
    return data[offset:offset+length]
assert builder.sha(data) == '504fc22628a2a7c563e593aab298b9273e2a80eaf8814a095ffb6f464152da99', 'wrong analysis image'
segments = [struct.unpack('<IIQQQQQQ', take(h[5] + i*h[9], 56)) for i in range(h[10])]
size = max(va + memsz for t, flags, off, va, pa, filesz, memsz, align in segments if t == 1)
size = (size + 0x3fff) & ~0x3fff
assert size < 0x3ff0000
image = bytearray(size)
for t, flags, off, va, pa, filesz, memsz, align in segments:
    if t == 1: image[va:va+filesz] = take(off, filesz)
def got(plt):
    assert image[plt:plt+2] == b'\xff\x25'
    return plt + 6 + struct.unpack_from('<i', image, plt+2)[0]
a.out.mkdir(parents=True, exist_ok=True)
(a.out/'target.bin').write_bytes(image)
recipe_path = root/'guest/games/CUSA50828/01.08'/('frame_recompiled.recipe.json' if a.frame_dispatch else 'rooftop_recompiled.recipe.json')
r = json.loads(recipe_path.read_text())
if 'recompile_contract' in r:
    r['recompile_contract'] = str(recipe_path.parent/r['recompile_contract'])
r['hooks'] = [x for x in r['hooks'] if x['name'] in ['write_label', 'default_state', 'submit_packets', 'submit_label'] + (['frame_dispatch'] if a.frame_dispatch else [])]
r['sources'] = [str(recipe_path.parent/'rooftop_recompiled.cpp'), str(Path(__file__).parent/'recompile_probe.cpp')]
r['exports'] = ['replay_submit_label']
if a.frame_dispatch: r['sources'].append(str(recipe_path.parent/'frame_dispatch.cpp'))
r['headers'] = [str(recipe_path.parent/'rooftop_types.h')]
(a.out/'recipe.json').write_text(json.dumps(r, indent=2)+'\n')
builder.build(a.out/'recipe.json', a.clang.resolve(), a.out/'package')
(a.out/'recompile.json').write_text(json.dumps(dict(module_size=size, image_sha256=builder.sha(image),
    analysis_sha256=builder.sha(data), canary_got=0x1d3b1b8, draw_got=got(0x15c1a00),
    submit_got=got(0x15c1890), device_got=got(0x15c18a0)), indent=2)+'\n')
