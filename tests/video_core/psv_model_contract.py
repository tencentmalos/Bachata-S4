#!/usr/bin/env python3
"""Validate the exported model's physical envelope and XR display/material contract."""
import json
import math
from pathlib import Path
import struct
import sys

path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / 'android/shadps4-app/app/src/main/assets/xr/psv_status.glb'
data = path.read_bytes()
assert struct.unpack_from('<III', data) == (0x46546c67, 2, len(data))
json_size = struct.unpack_from('<I', data, 12)[0]
doc = json.loads(data[20:20+json_size])
binary = data[28+json_size:]

def values(index):
    a = doc['accessors'][index]
    v = doc['bufferViews'][a['bufferView']]
    size = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
    fmt = {5126: 'f', 5125: 'I', 5123: 'H', 5121: 'B'}[a['componentType']]
    stride = v.get('byteStride', struct.calcsize('<'+fmt*size))
    offset = v.get('byteOffset', 0)+a.get('byteOffset', 0)
    return [struct.unpack_from('<'+fmt*size, binary, offset+i*stride) for i in range(a['count'])]

nodes = {n['name']: n for n in doc['nodes']}
assert {'psv_body', 'status_display', 'left_stick_anchor', 'right_stick_anchor'} <= nodes.keys()
# Check all exported geometry, not only the source builder's nominal dimensions.
# The former square shoulders exceeded the rounded outline by several mm.
for mesh in doc['meshes']:
    for primitive in mesh['primitives']:
        for x,y,z in values(primitive['attributes']['POSITION']):
            assert all(math.isfinite(v) for v in (x,y,z))
            assert abs(x) < .093 and abs(y) < .044 and -.012 < z < .018
            if abs(x) > .056 and y > .027 and z < .004:
                assert math.hypot(abs(x)-.055, y-.00575) <= .0378, ('shoulder protrudes beyond curved case', x,y,z)
shoulders = [n for n in doc['nodes'] if n['name'].startswith('shoulder_')]
assert len(shoulders) == 2, 'left and right transparent caps must remain separate'
for node in shoulders:
    for p in doc['meshes'][node['mesh']]['primitives']:
        m = doc['materials'][p['material']]
        assert m['extensions']['KHR_materials_transmission']['transmissionFactor'] > .5

p, = doc['meshes'][nodes['status_display']['mesh']]['primitives']
m = doc['materials'][p['material']]
assert m['name'] == 'status_screen' and 'KHR_materials_unlit' in m['extensions']
positions = values(p['attributes']['POSITION'])
uv = values(p['attributes']['TEXCOORD_0'])
width = max(v[0] for v in positions)-min(v[0] for v in positions)
height = max(v[1] for v in positions)-min(v[1] for v in positions)
assert abs(width/.1107-1) < .001 and abs(height/.06273-1) < .001
assert all(abs(v[2]-.00965) < 1e-6 for v in positions)
assert all(t[1] < .01 if v[1] > .0025 else t[1] > .99 for v,t in zip(positions,uv)), 'screen top-left raster UV convention'
assert all(v[2] > .99 for v in values(p['attributes']['NORMAL'])), 'screen must face viewer'
assert 'power_led' not in {m['name'] for m in doc['materials']}, 'no invented front status LED'
print('PASS: rounded case envelope, two transmissive shoulders, display dimensions/normals/UV/unlit, no stray LED')
