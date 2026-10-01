#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the actual Editor World/GLB geometry, including seated-view occlusion."""
import itertools
import json
import math
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'assets/xr/cinema'


def rotate(q, v):
    x, y, z, w = q
    a, b, c = v
    tx, ty, tz = 2*(y*c-z*b), 2*(z*a-x*c), 2*(x*b-y*a)
    return (a+w*tx+y*tz-z*ty, b+w*ty+z*tx-x*tz, c+w*tz+x*ty-y*tx)


def transform(obj, p):
    if 'matrix' in obj:
        m = obj['matrix']
        return tuple(sum(m[i+4*j]*p[j] for j in range(3))+m[i+12] for i in range(3))
    v = rotate(obj.get('rotation', [0, 0, 0, 1]),
               [a*b for a, b in zip(p, obj.get('scale', [1, 1, 1]))])
    return tuple(a+b for a, b in zip(v, obj.get('translation', [0, 0, 0])))


def model_vertices(path):
    raw = path.read_bytes()
    assert struct.unpack_from('<III', raw) == (0x46546c67, 2, len(raw))
    size = struct.unpack_from('<I', raw, 12)[0]
    doc = json.loads(raw[20:20+size])
    binary = raw[28+size:]

    def visit(index, chain):
        node = doc['nodes'][index]
        chain = [node] + chain
        if 'mesh' in node:
            for primitive in doc['meshes'][node['mesh']]['primitives']:
                accessor = doc['accessors'][primitive['attributes']['POSITION']]
                assert accessor['componentType'] == 5126 and accessor['type'] == 'VEC3'
                view = doc['bufferViews'][accessor['bufferView']]
                offset = view.get('byteOffset', 0)+accessor.get('byteOffset', 0)
                stride = view.get('byteStride', 12)
                for i in range(accessor['count']):
                    p = struct.unpack_from('<fff', binary, offset+i*stride)
                    for obj in chain:
                        p = transform(obj, p)
                    yield index, p
        for child in node.get('children', []):
            yield from visit(child, chain)

    return list(itertools.chain.from_iterable(
        visit(n, []) for n in doc['scenes'][doc.get('scene', 0)]['nodes']))


def validate(path):
    doc = json.loads(path.read_text(encoding='utf-8'))
    objects = {o['id']: o for o in doc['graph']['objects']}
    names = {o['name']: o for o in objects.values()}
    assert len(objects) == len(doc['graph']['objects'])
    models, bounds, node_bounds = {}, {}, {}
    for entry in doc['graph']['mesh_renderers']:
        obj = objects[entry['object']]
        model = entry['component']['model']
        if model not in models:
            models[model] = model_vertices(ASSETS / model)
        chain = [obj]
        while chain[-1]['parent']:
            chain.append(objects[chain[-1]['parent']])
            assert len(chain) <= len(objects), 'cyclic hierarchy'
        vertices, per_node = [], {}
        for node, v in models[model]:
            for parent in chain:
                v = transform(parent, v)
            assert all(math.isfinite(x) for x in v)
            vertices.append(v)
            per_node.setdefault(node, []).append(v)
        box = lambda vs: tuple(tuple(fn(v[i] for v in vs) for i in range(3)) for fn in (min, max))
        bounds[obj['name']] = box(vertices)
        # Per mesh node boxes for the occlusion test (a whole-model box would
        # join e.g. a console and a vase at its far end into one phantom block).
        node_bounds[obj['name']] = [box(vs) for vs in per_node.values()]

    eps = .003
    floor = 0.0
    top = bounds['Coffee table'][1][1]
    assert abs(top - .52) < eps, top
    supports = {'Coffee table': floor, 'PS4 console': None, 'PSV dock': top,
                'DualShock 4 L': top, 'DualShock 4 R': top, 'Soundbar': .30}
    for name, expected in supports.items():
        if name not in bounds:
            continue
        low = bounds[name][0][1]
        if expected is None:   # stands on the console (0.30) or a 0.42 side cabinet/plinth
            assert min(abs(low - .30), abs(low - .42)) < eps, (name, low)
        else:
            assert -eps <= low - expected <= eps, (name, low, expected)
    gap = bounds['PSV status display'][0][1] - (bounds['PSV dock'][0][1] + .0386)
    assert -eps < gap < .01, ('PSV must rest on its ledge', gap)

    eye = names['Viewer - seated']['translation']
    screen = 'Game screen - preview'
    s_low, s_high = bounds[screen]
    y0, y1 = s_low[1], s_high[1]
    sz = s_low[2]
    assert abs(eye[2] - sz - 3.0) < .002, eye[2] - sz
    assert abs(s_high[0] - s_low[0] - 4.40) < 1e-3 and abs(y1 - y0 - 2.475) < 1e-3

    def screen_hit(e, d):
        """Distance along the horizontal unit direction d from eye e to the
        screen surface, or None when the ray misses the aperture."""
        ex, ez = e[0], e[2]
        if d[1] >= 0:
            return None
        t = (sz - ez) / d[1]
        hx = ex + d[0] * t
        return t if s_low[0] <= hx <= s_high[0] else None

    excluded = ('Game screen', 'Room', 'Terrace', 'Sky', 'Rug', 'Viewer', 'TV', 'Outdoor screen',
                'Seaside', 'Void space')
    foreground = [n for n in bounds if not n.startswith(excluded) and 'shadow' not in n]
    checked, min_gap = 0, math.inf
    for dx, dy, dz, ipd, hand in itertools.product(
            (-.2, 0, .2), (-.15, 0, .15), (-.2, 0, .2), (.05, .064, .078), (-1, 1)):
        e = (eye[0] + dx + hand * ipd / 2, eye[1] + dy, eye[2] + dz)
        for name, (low, high) in ((n_, b) for n_ in foreground for b in node_bounds[n_]):
            n = 4
            for i, j, k in itertools.product(range(n + 1), repeat=3):
                p = (low[0] + (high[0] - low[0]) * i / n, low[1] + (high[1] - low[1]) * j / n,
                     low[2] + (high[2] - low[2]) * k / n)
                hx, hz = p[0] - e[0], p[2] - e[2]
                dist = math.hypot(hx, hz)
                if dist < 1e-6:
                    continue
                t = screen_hit(e, (hx / dist, hz / dist))
                checked += 1
                if t is None or dist >= t:
                    continue          # outside the aperture's azimuth, or behind the screen
                el = (p[1] - e[1]) / dist
                lo, hi = (y0 - e[1]) / t, (y1 - e[1]) / t
                assert not (lo < el < hi), (name, e, p)
                if el <= lo:
                    min_gap = min(min_gap, math.degrees(math.atan(lo) - math.atan(el)))
    return {'scene': path.name, 'objects': len(objects), 'models': len(models), 'screen': screen, 'foreground_objects': sorted(foreground), 'sample_checks': checked,
            'min_vertical_gap_degrees_below_screen': round(min_gap, 3),
            'support_contacts': 'pass', 'foreground_occlusion': 'pass',
            'head_motion_m': {'x': [-.2, .2], 'y': [-.15, .15], 'z': [-.2, .2]},
            'ipd_m': [.05, .064, .078]}


if __name__ == '__main__':
    print(json.dumps([validate(ASSETS / (name+'.world.json'))
                      for name in ('tv-lounge', 'dusk-terrace', 'dark-room', 'seaside', 'void')], indent=2))
